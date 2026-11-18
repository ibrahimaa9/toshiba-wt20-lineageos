/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.wt20"

#include "GloveMode.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <chrono>
#include <vector>

using ::android::base::GetBoolProperty;
using ::android::base::GetIntProperty;
using ::android::base::ReadFileToString;
using ::android::base::StartsWith;
using ::android::base::Trim;

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

namespace {

// The controller this HAL was written for: the Atmel mXT1664T of the Toshiba Portege WT20-B,
// info block and object table as read with mxt-app (logs/mxt/mxt-wt20-tuned-nvram.xcfg).
// Anything else, and the controller is left alone.
constexpr uint8_t kFamilyId = 164;
constexpr uint8_t kVariantId = 4;
constexpr uint8_t kVersion = 17;
constexpr uint8_t kBuild = 172;
constexpr uint16_t kInfoBlockSize = 7;  // family, variant, version, build, x, y, objects
constexpr size_t kObjectEntrySize = 6;  // type, start (LE16), size - 1, instances - 1, IDs
constexpr uint8_t kT100Type = 100;      // TOUCH_MULTITOUCHSCREEN_T100
constexpr uint16_t kT100Addr = 1490;
constexpr size_t kT100Size = 54;
constexpr uint16_t kThresholdOffset = 30;  // T100 bytes 30-32: TCHTHR, TCHHYST, INTTHR
constexpr uint16_t kThresholdAddr = kT100Addr + kThresholdOffset;

// Factory (NVRAM) values, written back when the switch goes off
constexpr Thresholds kFactory = {80, 20, 70};

// Higher sensitivity = lower touch threshold (persist.vendor.wt20.glove.tchthr)
constexpr int kGloveTchthrDefault = 56;
constexpr int kGloveTchthrMin = 40;
constexpr int kGloveTchthrMax = 79;

constexpr auto kWatchInterval = std::chrono::seconds(10);

constexpr const char* kDisableProp = "persist.vendor.wt20.glove.disable";
constexpr const char* kTchthrProp = "persist.vendor.wt20.glove.tchthr";

static_assert(kThresholdOffset + std::tuple_size<Thresholds>::value <= kT100Size,
              "the thresholds must lie inside T100");

Thresholds gloveThresholds() {
    const int t = GetIntProperty(kTchthrProp, kGloveTchthrDefault, kGloveTchthrMin,
                                 kGloveTchthrMax);
    // Same ratios as the factory values: TCHHYST = TCHTHR / 4, INTTHR = TCHTHR * 7 / 8
    return {static_cast<uint8_t>(t), static_cast<uint8_t>((t * 20 + 40) / 80),
            static_cast<uint8_t>((t * 70 + 40) / 80)};
}

std::string toString(const Thresholds& t) {
    return std::to_string(t[0]) + "/" + std::to_string(t[1]) + "/" + std::to_string(t[2]);
}

bool isWt20() {
    std::string vendor, product;
    if (!ReadFileToString("/sys/devices/virtual/dmi/id/sys_vendor", &vendor) ||
        !ReadFileToString("/sys/devices/virtual/dmi/id/product_name", &product)) {
        return false;
    }
    return Trim(vendor) == "TOSHIBA" && StartsWith(Trim(product), "PORTEGE WT20");
}

}  // anonymous namespace

GloveMode::GloveMode() : mIsWt20(isWt20()) {
    if (!mIsWt20) {
        LOG(WARNING) << "Not a Toshiba Portege WT20: high touch sensitivity stays off";
    }
}

GloveMode::~GloveMode() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mStop = true;
    }
    mCond.notify_all();
    if (mThread.joinable()) {
        mThread.join();
    }
}

void GloveMode::start() {
    mThread = std::thread(&GloveMode::watchLoop, this);
}

ndk::ScopedAStatus GloveMode::getEnabled(bool* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    *_aidl_return = mWanted;
    return ndk::ScopedAStatus::ok();
}

// Always ok: system_server does not expect an error from this call.
ndk::ScopedAStatus GloveMode::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    LOG(INFO) << "High touch sensitivity " << (enabled ? "on" : "off");
    mWanted = enabled;
    syncLocked();
    mCond.notify_all();
    return ndk::ScopedAStatus::ok();
}

bool GloveMode::allowedLocked() const {
    return mIsWt20 && !GetBoolProperty(kDisableProp, false);
}

void GloveMode::noteLocked(const std::string& message) {
    if (message != mLastNote) {
        mLastNote = message;
        LOG(WARNING) << message;
    }
}

// Opens the controller and checks it is the one this HAL knows: the info block every time,
// the object table again after each time the controller was not found.
bool GloveMode::attachLocked() {
    if (!mDev.open()) {
        mVerified = false;
        noteLocked("maXTouch object protocol interface (03eb:8a55, vendor page) not found");
        return false;
    }
    uint8_t info[kInfoBlockSize] = {};
    if (!mDev.read(0, info, sizeof(info))) {
        mDev.close();
        mVerified = false;
        noteLocked("Cannot read the maXTouch info block");
        return false;
    }
    if (info[0] != kFamilyId || info[1] != kVariantId || info[2] != kVersion ||
        info[3] != kBuild) {
        mDev.close();
        mVerified = false;
        noteLocked("Unknown maXTouch controller/firmware " + std::to_string(info[0]) + "/" +
                   std::to_string(info[1]) + "/" + std::to_string(info[2]) + "/" +
                   std::to_string(info[3]) + ": not touching it");
        return false;
    }
    if (mVerified) {
        return true;
    }

    const size_t count = info[6];
    std::vector<uint8_t> table(count * kObjectEntrySize);
    if (!mDev.read(kInfoBlockSize, table.data(), table.size())) {
        mDev.close();
        noteLocked("Cannot read the maXTouch object table");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const uint8_t* object = &table[i * kObjectEntrySize];
        if (object[0] != kT100Type) {
            continue;
        }
        const uint16_t addr = static_cast<uint16_t>(object[1] | (object[2] << 8));
        const size_t size = static_cast<size_t>(object[3]) + 1;
        if (addr != kT100Addr || size != kT100Size) {
            mDev.close();
            noteLocked("T100 at " + std::to_string(addr) + " size " + std::to_string(size) +
                       ", expected " + std::to_string(kT100Addr) + " size " +
                       std::to_string(kT100Size) + ": not touching it");
            return false;
        }
        mVerified = true;
        mLastNote.clear();
        return true;
    }
    mDev.close();
    noteLocked("No T100 object in the maXTouch object table");
    return false;
}

bool GloveMode::readLocked(Thresholds* out) {
    return mDev.read(kThresholdAddr, out->data(), out->size());
}

// The only write this HAL makes: T100 bytes 30-32, in the controller's RAM. Nothing is ever
// backed up to NVRAM (no T6 BACKUPNV), so a reset or power cycle brings the factory values
// back by itself. The write is read back.
bool GloveMode::writeLocked(const Thresholds& values) {
    Thresholds check = {};
    if (!mDev.write(kThresholdAddr, values.data(), values.size()) || !readLocked(&check) ||
        check != values) {
        noteLocked("Writing the T100 thresholds " + toString(values) + " failed");
        return false;
    }
    mWritten = values;
    return true;
}

// Brings the controller to the wanted state, moving only between known states (the factory
// thresholds and this HAL's own); anything else, e.g. a config written with mxt-app, is left
// alone.
void GloveMode::syncLocked() {
    const bool wanted = mWanted && allowedLocked();
    if (!wanted && !mApplied) {
        return;  // off, and nothing of ours in the controller
    }
    if (!attachLocked()) {
        return;  // not there (yet), or re-enumerating: the watch tries again
    }

    Thresholds current = {};
    if (!readLocked(&current)) {
        mDev.close();
        mVerified = false;
        noteLocked("Cannot read the T100 thresholds");
        return;
    }
    const Thresholds glove = gloveThresholds();
    const Thresholds target = wanted ? glove : kFactory;
    const bool known =
            current == kFactory || current == glove || (mApplied && current == mWritten);

    if (current == target) {
        mApplied = wanted;
        if (wanted) {
            mWritten = current;  // ours now, even if found there (e.g. by an earlier HAL)
        }
    } else if (!known) {
        noteLocked("T100 thresholds " + toString(current) +
                   " are neither the factory ones nor this HAL's: leaving them");
        if (!wanted) {
            mApplied = false;
        }
    } else if (writeLocked(target)) {
        if (wanted && mApplied && current == kFactory) {
            LOG(INFO) << "Controller reset (factory thresholds back): " << toString(target)
                      << " written again";
        } else {
            LOG(INFO) << "T100 thresholds " << toString(current) << " -> " << toString(target);
        }
        mApplied = wanted;
        mLastNote.clear();
    }
    mDev.close();
}

// A restarted HAL may find its own thresholds still in the controller's RAM, and Settings
// will not ask again: let syncLocked() put the factory ones back (unless it is asked for).
void GloveMode::restoreLeftoverLocked() {
    if (!attachLocked()) {
        return;
    }
    Thresholds current = {};
    if (readLocked(&current) && current != kFactory && current == gloveThresholds()) {
        mWritten = current;
        mApplied = true;
    }
    mDev.close();
}

void GloveMode::watchLoop() {
    std::unique_lock<std::mutex> lock(mLock);
    if (mIsWt20) {
        restoreLeftoverLocked();
        syncLocked();
    }
    while (!mStop) {
        if (mWanted || mApplied) {
            mCond.wait_for(lock, kWatchInterval);
            if (!mStop) {
                syncLocked();
            }
        } else {
            mCond.wait(lock);
        }
    }
}

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
