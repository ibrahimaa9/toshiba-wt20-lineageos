/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Toshiba Portege WT20-B thermal HAL: the CPU package temperature (x86_pkg_temp) reported as
 * a CPU and a SKIN sensor, with throttling callbacks when its severity changes.
 */

#pragma once

#include <aidl/android/hardware/thermal/BnThermal.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aidl::android::hardware::thermal::impl::wt20 {

class Thermal : public BnThermal {
  public:
    ::ndk::ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* out_devices) override;
    ::ndk::ScopedAStatus getCoolingDevicesWithType(
            CoolingType in_type, std::vector<CoolingDevice>* out_devices) override;

    ::ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* out_temperatures) override;
    ::ndk::ScopedAStatus getTemperaturesWithType(
            TemperatureType in_type, std::vector<Temperature>* out_temperatures) override;

    ::ndk::ScopedAStatus getTemperatureThresholds(
            std::vector<TemperatureThreshold>* out_temperatureThresholds) override;
    ::ndk::ScopedAStatus getTemperatureThresholdsWithType(
            TemperatureType in_type,
            std::vector<TemperatureThreshold>* out_temperatureThresholds) override;

    ::ndk::ScopedAStatus registerThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& in_callback) override;
    ::ndk::ScopedAStatus registerThermalChangedCallbackWithType(
            const std::shared_ptr<IThermalChangedCallback>& in_callback,
            TemperatureType in_type) override;
    ::ndk::ScopedAStatus unregisterThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& in_callback) override;

    ::ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback,
            CoolingType in_type) override;
    ::ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback) override;

    ::ndk::ScopedAStatus forecastSkinTemperature(int32_t in_forecastSeconds,
                                                 float* _aidl_return) override;

    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

    // Reads the package temperature periodically and notifies the callbacks when the
    // throttling severity changes. Never returns (run it on its own thread).
    void pollLoop();

  private:
    struct Callback {
        std::shared_ptr<IThermalChangedCallback> callback;
        bool filtered;
        TemperatureType type;
    };

    // Callers hold sensor_mutex_
    bool readCelsiusLocked(float* out_celsius);

    ::ndk::ScopedAStatus addCallback(const std::shared_ptr<IThermalChangedCallback>& callback,
                                     bool filtered, TemperatureType type);
    void notify(const std::vector<Callback>& targets, const std::vector<Temperature>& temps);

    std::mutex sensor_mutex_;
    std::string temp_path_;
    ThrottlingSeverity severity_ = ThrottlingSeverity::NONE;
    float last_celsius_ = NAN;

    std::mutex callback_mutex_;
    std::vector<Callback> callbacks_;
};

}  // namespace aidl::android::hardware::thermal::impl::wt20
