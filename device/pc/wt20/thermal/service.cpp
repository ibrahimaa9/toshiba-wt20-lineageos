/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Toshiba Portege WT20-B thermal HAL (see Thermal.h).
 */

#define LOG_TAG "android.hardware.thermal-service.wt20"

#include "Thermal.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <thread>

using aidl::android::hardware::thermal::impl::wt20::Thermal;

int main() {
    android::base::SetDefaultTag(LOG_TAG);
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Thermal> thermal = ndk::SharedRefBase::make<Thermal>();

    // Register first: system_server waits for the declared IThermal while it boots
    const std::string instance = std::string() + Thermal::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(thermal->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);
    LOG(INFO) << "WT20 thermal HAL started";

    std::thread([thermal] { thermal->pollLoop(); }).detach();

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
