/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.wt20"

#include "MxtHidraw.h"

#include <android-base/logging.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

namespace {

constexpr uint16_t kVendorId = 0x03eb;   // Atmel
constexpr uint16_t kProductId = 0x8a55;  // maXTouch mXT1664T of the Toshiba Portege WT20-B
constexpr uint8_t kReportId = 0x01;      // native USB maXTouch (I2C-HID ones use 6)
constexpr uint8_t kCmdRegister = 0x51;   // read / write object memory
constexpr uint8_t kStatusReadOk = 0x00;
constexpr uint8_t kStatusWriteOk = 0x04;
constexpr size_t kHeaderSize = 6;        // report ID, command, 2 + write len, read len, address
constexpr size_t kMaxRead = 15;          // data bytes per packet, as mxt-app's hidraw backend
constexpr size_t kMaxWrite = 12;
constexpr int kResponseTimeoutMs = 500;
constexpr int kMaxHidraw = 32;

// The object protocol interface: USB 03eb:8a55 with a vendor-defined report descriptor
// (Usage Page 0xFF00). The digitizer interface (Usage Page 0x0D) is never used.
bool isObjectInterface(int fd) {
    struct hidraw_devinfo info = {};
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 || info.bustype != BUS_USB ||
        static_cast<uint16_t>(info.vendor) != kVendorId ||
        static_cast<uint16_t>(info.product) != kProductId) {
        return false;
    }
    int size = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size < 3 || size > HID_MAX_DESCRIPTOR_SIZE) {
        return false;
    }
    struct hidraw_report_descriptor desc = {};
    desc.size = static_cast<uint32_t>(size);
    if (ioctl(fd, HIDIOCGRDESC, &desc) < 0) {
        return false;
    }
    return desc.value[0] == 0x06 && desc.value[1] == 0x00 && desc.value[2] == 0xff;
}

}  // anonymous namespace

MxtHidraw::~MxtHidraw() {
    close();
}

bool MxtHidraw::open() {
    close();
    for (int n = 0; n < kMaxHidraw; n++) {
        const int index = (mLastIndex + n) % kMaxHidraw;
        const std::string path = "/dev/hidraw" + std::to_string(index);
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (isObjectInterface(fd)) {
            mFd = fd;
            mLastIndex = index;
            return true;
        }
        ::close(fd);
    }
    return false;
}

void MxtHidraw::close() {
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
}

bool MxtHidraw::transact(const uint8_t* cmd, size_t cmdLen, uint8_t* resp, size_t* respLen) {
    // Drop input reports left from earlier traffic
    uint8_t stale[kReportSize];
    while (::read(mFd, stale, sizeof(stale)) > 0) {
    }

    if (::write(mFd, cmd, cmdLen) != static_cast<ssize_t>(cmdLen)) {
        PLOG(ERROR) << "hidraw write";
        return false;
    }

    const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kResponseTimeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
        if (left <= 0) {
            break;
        }
        struct pollfd pfd = {mFd, POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            break;
        }
        const ssize_t n = ::read(mFd, resp, kReportSize);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            PLOG(ERROR) << "hidraw read";
            return false;
        }
        if (n >= 2 && resp[0] == kReportId) {
            *respLen = static_cast<size_t>(n);
            return true;
        }
    }
    LOG(ERROR) << "No answer from the maXTouch controller";
    return false;
}

bool MxtHidraw::read(uint16_t addr, uint8_t* buf, size_t len) {
    if (mFd < 0) {
        return false;
    }
    for (size_t done = 0; done < len;) {
        const size_t count = std::min(len - done, kMaxRead);
        const uint16_t at = static_cast<uint16_t>(addr + done);
        const uint8_t cmd[kHeaderSize] = {kReportId,
                                          kCmdRegister,
                                          2,
                                          static_cast<uint8_t>(count),
                                          static_cast<uint8_t>(at & 0xff),
                                          static_cast<uint8_t>(at >> 8)};
        uint8_t resp[kReportSize] = {};
        size_t respLen = 0;
        if (!transact(cmd, sizeof(cmd), resp, &respLen)) {
            return false;
        }
        if (resp[1] != kStatusReadOk || respLen < 3 + count) {
            LOG(ERROR) << "maXTouch read of " << count << " bytes at " << at
                       << " failed: status " << static_cast<int>(resp[1]) << ", " << respLen
                       << " bytes";
            return false;
        }
        memcpy(buf + done, resp + 3, count);
        done += count;
    }
    return true;
}

bool MxtHidraw::write(uint16_t addr, const uint8_t* buf, size_t len) {
    if (mFd < 0) {
        return false;
    }
    for (size_t done = 0; done < len;) {
        const size_t count = std::min(len - done, kMaxWrite);
        const uint16_t at = static_cast<uint16_t>(addr + done);
        uint8_t cmd[kHeaderSize + kMaxWrite] = {kReportId,
                                                kCmdRegister,
                                                static_cast<uint8_t>(2 + count),
                                                0,
                                                static_cast<uint8_t>(at & 0xff),
                                                static_cast<uint8_t>(at >> 8)};
        memcpy(cmd + kHeaderSize, buf + done, count);
        uint8_t resp[kReportSize] = {};
        size_t respLen = 0;
        if (!transact(cmd, kHeaderSize + count, resp, &respLen)) {
            return false;
        }
        if (resp[1] != kStatusWriteOk) {
            LOG(ERROR) << "maXTouch write of " << count << " bytes at " << at
                       << " failed: status " << static_cast<int>(resp[1]);
            return false;
        }
        done += count;
    }
    return true;
}

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
