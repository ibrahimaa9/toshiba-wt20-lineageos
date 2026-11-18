/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lineage/touch/BnGloveMode.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "MxtHidraw.h"

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

// T100 TCHTHR, TCHHYST, INTTHR
using Thresholds = std::array<uint8_t, 3>;

class GloveMode : public BnGloveMode {
  public:
    GloveMode();
    ~GloveMode() override;

    // Starts the controller-reset watch. Call after the service is registered.
    void start();

    ndk::ScopedAStatus getEnabled(bool* _aidl_return) override;
    ndk::ScopedAStatus setEnabled(bool enabled) override;

  private:
    void watchLoop();
    bool allowedLocked() const;
    bool attachLocked();
    bool readLocked(Thresholds* out);
    bool writeLocked(const Thresholds& values);
    void syncLocked();
    void restoreLeftoverLocked();
    void noteLocked(const std::string& message);

    const bool mIsWt20;
    std::mutex mLock;
    std::condition_variable mCond;
    std::thread mThread;
    MxtHidraw mDev;
    bool mWanted = false;    // what Settings asked for
    bool mApplied = false;   // this HAL's thresholds may be in the controller's RAM
    bool mVerified = false;  // object table checked since the controller was last found
    bool mStop = false;
    Thresholds mWritten = {};
    std::string mLastNote;
};

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
