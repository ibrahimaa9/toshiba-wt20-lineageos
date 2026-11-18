/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.wt20"

#include "GloveMode.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <memory>
#include <string>

using aidl::vendor::lineage::touch::GloveMode;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<GloveMode> glovemode = ndk::SharedRefBase::make<GloveMode>();

    // Register before touching the controller: system_server waits for this declared
    // service at boot
    const std::string instance = std::string(GloveMode::descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(glovemode->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to add service " << instance << " " << status;

    glovemode->start();

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
