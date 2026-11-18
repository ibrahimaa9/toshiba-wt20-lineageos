/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

// Atmel maXTouch object protocol over the vendor HID interface of a native USB maXTouch
// controller (Toshiba Portege WT20-B: mXT1664T, 03eb:8a55, interface 0, report ID 1). The
// packets are mxt-app's (usb_device.c / hidraw_device.c):
//   read:  [01][51][02][n][addr lo][addr hi]          -> [01][00][..][n data bytes]
//   write: [01][51][2+n][00][addr lo][addr hi][data]  -> [01][04]
class MxtHidraw {
  public:
    MxtHidraw() = default;
    ~MxtHidraw();
    MxtHidraw(const MxtHidraw&) = delete;
    MxtHidraw& operator=(const MxtHidraw&) = delete;

    // Finds and opens the controller's object protocol hidraw node
    bool open();
    void close();
    bool read(uint16_t addr, uint8_t* buf, size_t len);
    bool write(uint16_t addr, const uint8_t* buf, size_t len);

  private:
    static constexpr size_t kReportSize = 64;  // report ID + 63 bytes

    // Sends one command and waits for the controller's report ID 1 answer
    bool transact(const uint8_t* cmd, size_t cmdLen, uint8_t* resp, size_t* respLen);

    int mFd = -1;
    int mLastIndex = 0;  // hidraw number found last time, tried first
};

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
