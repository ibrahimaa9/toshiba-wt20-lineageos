/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Toshiba Portege WT20-B thermal HAL (see Thermal.h).
 */

#define LOG_TAG "android.hardware.thermal-service.wt20"

#include "Thermal.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <dirent.h>
#include <stdio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace aidl::android::hardware::thermal::impl::wt20 {

using ::ndk::ScopedAStatus;

namespace {

constexpr char kThermalRoot[] = "/sys/devices/virtual/thermal";
constexpr char kZonePrefix[] = "thermal_zone";
constexpr char kPackageZoneType[] = "x86_pkg_temp";

constexpr char kCpuName[] = "CPU";
constexpr char kSkinName[] = "SKIN";

// Indexed by ThrottlingSeverity: NONE, LIGHT, MODERATE, SEVERE, CRITICAL, EMERGENCY, SHUTDOWN.
// Core M-5Y10c, fanless: the firmware's own trip is 97 C, so nothing at CRITICAL and above
// (Android shuts down at SHUTDOWN).
const std::array<float, 7> kHotThresholds = {NAN, 80.0f, 88.0f, 94.0f, NAN, NAN, NAN};
// A severity already reached is only left once the temperature is this far below its threshold
constexpr float kHysteresis = 2.0f;

constexpr std::chrono::seconds kIdlePoll(3);
constexpr std::chrono::seconds kWarmPoll(1);
constexpr std::chrono::seconds kRetryPoll(10);
// Poll faster from here on (and while throttling), so LIGHT is not reported late
constexpr float kWarmCelsius = 72.0f;

bool interfacesEqual(const std::shared_ptr<::ndk::ICInterface>& left,
                     const std::shared_ptr<::ndk::ICInterface>& right) {
    if (left == nullptr || right == nullptr || !left->isRemote() || !right->isRemote()) {
        return left == right;
    }
    return left->asBinder() == right->asBinder();
}

ThrottlingSeverity severityFor(float celsius, ThrottlingSeverity previous) {
    if (std::isnan(celsius)) {
        return ThrottlingSeverity::NONE;
    }
    const size_t reached = static_cast<size_t>(previous);
    size_t level = 0;
    for (size_t i = 1; i < kHotThresholds.size(); ++i) {
        float threshold = kHotThresholds[i];
        if (std::isnan(threshold)) {
            continue;
        }
        if (i <= reached) {
            threshold -= kHysteresis;
        }
        if (celsius >= threshold) {
            level = i;
        }
    }
    return static_cast<ThrottlingSeverity>(level);
}

// The "temp" node of the x86_pkg_temp zone, or "" if there is none (yet)
std::string findPackageZone() {
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(kThermalRoot), closedir);
    if (dir == nullptr) {
        PLOG(ERROR) << "Cannot open " << kThermalRoot;
        return "";
    }
    while (struct dirent* entry = readdir(dir.get())) {
        const std::string name = entry->d_name;
        if (!::android::base::StartsWith(name, kZonePrefix)) {
            continue;
        }
        const std::string zone = std::string(kThermalRoot) + "/" + name;
        std::string type;
        if (!::android::base::ReadFileToString(zone + "/type", &type)) {
            continue;
        }
        if (::android::base::Trim(type) == kPackageZoneType) {
            return zone + "/temp";
        }
    }
    return "";
}

std::vector<Temperature> makeTemperatures(float celsius, ThrottlingSeverity severity) {
    std::vector<Temperature> temps(2);
    temps[0].type = TemperatureType::CPU;
    temps[0].name = kCpuName;
    temps[1].type = TemperatureType::SKIN;
    temps[1].name = kSkinName;
    for (Temperature& t : temps) {
        t.value = celsius;
        t.throttlingStatus = severity;
    }
    return temps;
}

std::vector<TemperatureThreshold> makeThresholds() {
    std::vector<TemperatureThreshold> thresholds(2);
    thresholds[0].type = TemperatureType::CPU;
    thresholds[0].name = kCpuName;
    thresholds[1].type = TemperatureType::SKIN;
    thresholds[1].name = kSkinName;
    for (TemperatureThreshold& t : thresholds) {
        t.hotThrottlingThresholds.assign(kHotThresholds.begin(), kHotThresholds.end());
        t.coldThrottlingThresholds.assign(kHotThresholds.size(), NAN);
    }
    return thresholds;
}

template <typename T>
std::vector<T> ofType(std::vector<T> all, TemperatureType type) {
    all.erase(std::remove_if(all.begin(), all.end(),
                             [type](const T& t) { return t.type != type; }),
              all.end());
    return all;
}

}  // namespace

bool Thermal::readCelsiusLocked(float* out_celsius) {
    if (temp_path_.empty()) {
        temp_path_ = findPackageZone();
        if (temp_path_.empty()) {
            return false;
        }
        LOG(INFO) << "CPU package temperature: " << temp_path_;
    }
    std::string text;
    int millicelsius = 0;
    if (!::android::base::ReadFileToString(temp_path_, &text) ||
        !::android::base::ParseInt(::android::base::Trim(text), &millicelsius)) {
        PLOG(WARNING) << "Cannot read " << temp_path_;
        // The zone may have gone away and come back under another number: look it up again
        temp_path_.clear();
        return false;
    }
    *out_celsius = static_cast<float>(millicelsius) / 1000.0f;
    return true;
}

void Thermal::pollLoop() {
    while (true) {
        float celsius = NAN;
        bool ok = false;
        bool changed = false;
        ThrottlingSeverity severity = ThrottlingSeverity::NONE;
        {
            std::lock_guard<std::mutex> lock(sensor_mutex_);
            ok = readCelsiusLocked(&celsius);
            if (ok) {
                severity = severityFor(celsius, severity_);
                changed = severity != severity_;
                severity_ = severity;
                last_celsius_ = celsius;
            } else {
                severity = severity_;
            }
        }
        if (changed) {
            LOG(INFO) << "CPU package at " << celsius << " C: " << toString(severity);
            std::vector<Callback> targets;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                targets = callbacks_;
            }
            notify(targets, makeTemperatures(celsius, severity));
        }
        std::chrono::seconds wait = kIdlePoll;
        if (!ok) {
            wait = kRetryPoll;
        } else if (severity != ThrottlingSeverity::NONE || celsius >= kWarmCelsius) {
            wait = kWarmPoll;
        }
        std::this_thread::sleep_for(wait);
    }
}

void Thermal::notify(const std::vector<Callback>& targets, const std::vector<Temperature>& temps) {
    std::vector<std::shared_ptr<IThermalChangedCallback>> dead;
    for (const Callback& target : targets) {
        for (const Temperature& t : temps) {
            if (target.filtered && t.type != target.type) {
                continue;
            }
            ScopedAStatus status = target.callback->notifyThrottling(t);
            if (!status.isOk()) {
                LOG(WARNING) << "notifyThrottling failed: " << status.getDescription();
                if (status.getStatus() == STATUS_DEAD_OBJECT) {
                    dead.push_back(target.callback);
                    break;
                }
            }
        }
    }
    if (dead.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_.erase(std::remove_if(callbacks_.begin(), callbacks_.end(),
                                    [&dead](const Callback& c) {
                                        return std::any_of(
                                                dead.begin(), dead.end(),
                                                [&c](const std::shared_ptr<
                                                        IThermalChangedCallback>& d) {
                                                    return interfacesEqual(c.callback, d);
                                                });
                                    }),
                     callbacks_.end());
}

ScopedAStatus Thermal::getCoolingDevices(std::vector<CoolingDevice>* out_devices) {
    out_devices->clear();
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getCoolingDevicesWithType(CoolingType /* in_type */,
                                                 std::vector<CoolingDevice>* out_devices) {
    out_devices->clear();
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatures(std::vector<Temperature>* out_temperatures) {
    std::lock_guard<std::mutex> lock(sensor_mutex_);
    float celsius = NAN;
    if (!readCelsiusLocked(&celsius)) {
        celsius = NAN;
    }
    // The severity is the one the poll loop last notified, so readers and callbacks agree
    *out_temperatures = makeTemperatures(celsius, severity_);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperaturesWithType(TemperatureType in_type,
                                               std::vector<Temperature>* out_temperatures) {
    std::vector<Temperature> all;
    ScopedAStatus status = getTemperatures(&all);
    if (!status.isOk()) {
        return status;
    }
    *out_temperatures = ofType(std::move(all), in_type);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatureThresholds(
        std::vector<TemperatureThreshold>* out_temperatureThresholds) {
    *out_temperatureThresholds = makeThresholds();
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatureThresholdsWithType(
        TemperatureType in_type, std::vector<TemperatureThreshold>* out_temperatureThresholds) {
    *out_temperatureThresholds = ofType(makeThresholds(), in_type);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::addCallback(const std::shared_ptr<IThermalChangedCallback>& callback,
                                   bool filtered, TemperatureType type) {
    if (callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (std::any_of(callbacks_.begin(), callbacks_.end(), [&callback](const Callback& c) {
                return interfacesEqual(c.callback, callback);
            })) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                               "Callback already registered");
        }
        callbacks_.push_back(Callback{callback, filtered, type});
    }
    // A new listener learns about throttling that is already going on
    float celsius = NAN;
    ThrottlingSeverity severity = ThrottlingSeverity::NONE;
    {
        std::lock_guard<std::mutex> lock(sensor_mutex_);
        celsius = last_celsius_;
        severity = severity_;
    }
    if (severity != ThrottlingSeverity::NONE) {
        notify({Callback{callback, filtered, type}}, makeTemperatures(celsius, severity));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& in_callback) {
    return addCallback(in_callback, false, TemperatureType::UNKNOWN);
}

ScopedAStatus Thermal::registerThermalChangedCallbackWithType(
        const std::shared_ptr<IThermalChangedCallback>& in_callback, TemperatureType in_type) {
    return addCallback(in_callback, true, in_type);
}

ScopedAStatus Thermal::unregisterThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& in_callback) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    std::lock_guard<std::mutex> lock(callback_mutex_);
    const auto it = std::remove_if(callbacks_.begin(), callbacks_.end(),
                                   [&in_callback](const Callback& c) {
                                       return interfacesEqual(c.callback, in_callback);
                                   });
    if (it == callbacks_.end()) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Callback wasn't registered");
    }
    callbacks_.erase(it, callbacks_.end());
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerCoolingDeviceChangedCallbackWithType(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback,
        CoolingType /* in_type */) {
    // There are no cooling devices, so there is never anything to report
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::unregisterCoolingDeviceChangedCallback(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::forecastSkinTemperature(int32_t /* in_forecastSeconds */,
                                               float* /* _aidl_return */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

binder_status_t Thermal::dump(int fd, const char** /* args */, uint32_t /* numArgs */) {
    std::string path;
    float celsius = NAN;
    ThrottlingSeverity severity = ThrottlingSeverity::NONE;
    {
        std::lock_guard<std::mutex> lock(sensor_mutex_);
        path = temp_path_;
        celsius = last_celsius_;
        severity = severity_;
    }
    size_t callbacks = 0;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callbacks = callbacks_.size();
    }
    dprintf(fd, "CPU package temperature node: %s\n",
            path.empty() ? "(x86_pkg_temp not found)" : path.c_str());
    dprintf(fd, "Last poll: %.1f C, severity %s\n", celsius, toString(severity).c_str());
    dprintf(fd, "Hot thresholds LIGHT/MODERATE/SEVERE: %.0f/%.0f/%.0f C, hysteresis %.0f C\n",
            kHotThresholds[1], kHotThresholds[2], kHotThresholds[3], kHysteresis);
    dprintf(fd, "Registered callbacks: %zu\n", callbacks);
    return STATUS_OK;
}

}  // namespace aidl::android::hardware::thermal::impl::wt20
