/*
 * Copyright (C) 2022 Michael Goffioul <michael.goffioul@gmail.com>
 * Copyright (C) 2025 KonstaKANG
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "C2FFMPEGComponentStore"
#include <android-base/properties.h>
#include <log/log.h>

#include "C2FFMPEGCommon.h"
#include "C2FFMPEGVaapi.h"
#include "C2FFMPEGVideoDecodeComponent.h"
#include "C2FFMPEGVideoDecodeInterface.h"

#include "C2FFMPEGComponentStore.h"

namespace android {

// WT20: only the codecs the HD 5300 decodes (i965 VLD). Audio and the other video codecs stay
// on the platform's c2.android components (and dav1d).
static const C2FFMPEGComponentInfo kFFMPEGVideoComponents[] = {
    { "c2.ffmpeg.h264.decoder"  , MEDIA_MIMETYPE_VIDEO_AVC   , AV_CODEC_ID_H264       },
    { "c2.ffmpeg.mpeg2.decoder" , MEDIA_MIMETYPE_VIDEO_MPEG2 , AV_CODEC_ID_MPEG2VIDEO },
    { "c2.ffmpeg.vp8.decoder"   , MEDIA_MIMETYPE_VIDEO_VP8   , AV_CODEC_ID_VP8        },
};

static const size_t kNumVideoComponents =
    (sizeof(kFFMPEGVideoComponents) / sizeof(kFFMPEGVideoComponents[0]));

// Below the software codecs' 0x200; device/pc/wt20/media/media_codecs_c2.xml sets the rank
// MediaCodecList uses (0x80).
static constexpr C2Component::rank_t kVideoRank = 0x80;

// A component is available when the kill switch is on and the VA driver decodes its codec.
static const C2FFMPEGComponentInfo* findAvailableComponent(const C2String& name) {
    for (size_t i = 0; i < kNumVideoComponents; i++) {
        const C2FFMPEGComponentInfo* info = &kFFMPEGVideoComponents[i];
        if (name == info->name) {
            if (!wt20::HwDecodeEnabled() || !wt20::VaSupportsCodec(info->codecID)) {
                return nullptr;
            }
            return info;
        }
    }
    return nullptr;
}

C2FFMPEGComponentStore::C2FFMPEGComponentStore()
    : mReflectorHelper(std::make_shared<C2ReflectorHelper>()),
      mInterface(mReflectorHelper) {
}

C2FFMPEGComponentStore::~C2FFMPEGComponentStore() = default;

C2String C2FFMPEGComponentStore::getName() const {
    return "ffmpeg";
}

c2_status_t C2FFMPEGComponentStore::createComponent(
        C2String name,
        std::shared_ptr<C2Component>* const component) {
    ALOGD("createComponent: %s", name.c_str());
    component->reset();
    const C2FFMPEGComponentInfo* info = findAvailableComponent(name);
    if (!info) {
        ALOGE("createComponent: unknown or disabled component = %s", name.c_str());
        return C2_NOT_FOUND;
    }
    *component = std::shared_ptr<C2Component>(
            new C2FFMPEGVideoDecodeComponent(
                    info, std::make_shared<C2FFMPEGVideoDecodeInterface>(info, mReflectorHelper)));
    return C2_OK;
}

c2_status_t C2FFMPEGComponentStore::createInterface(
        C2String name,
        std::shared_ptr<C2ComponentInterface>* const interface) {
    ALOGD("createInterface: %s", name.c_str());
    interface->reset();
    const C2FFMPEGComponentInfo* info = findAvailableComponent(name);
    if (!info) {
        ALOGE("createInterface: unknown or disabled component = %s", name.c_str());
        return C2_NOT_FOUND;
    }
    *interface = std::shared_ptr<C2ComponentInterface>(
            new SimpleInterface<C2FFMPEGVideoDecodeInterface>(
                    info->name, 0, std::make_shared<C2FFMPEGVideoDecodeInterface>(info, mReflectorHelper)));
    return C2_OK;
}

std::vector<std::shared_ptr<const C2Component::Traits>>
        C2FFMPEGComponentStore::listComponents() {
    std::vector<std::shared_ptr<const C2Component::Traits>> ret;

    // Kill switch first: with it off, not even the VA driver is loaded.
    if (!wt20::HwDecodeEnabled()) {
        ALOGI("listComponents: persist.vendor.wt20.hwdec=false, no component");
        return ret;
    }
    wt20::VaProbeResult probe = wt20::ProbeVa();
    if (!probe.display) {
        ALOGW("listComponents: VA probe failed, no component");
        return ret;
    }
    if (!probe.vpp) {
        ALOGW("listComponents: no VA VideoProc, frames will be copied by the CPU");
    }
    for (size_t i = 0; i < kNumVideoComponents; i++) {
        const C2FFMPEGComponentInfo* info = &kFFMPEGVideoComponents[i];
        if (!wt20::VaSupportsCodec(info->codecID)) {
            ALOGW("listComponents: %s: no VLD in the VA driver, not listed", info->name);
            continue;
        }
        auto traits = std::make_shared<C2Component::Traits>();
        traits->name = info->name;
        traits->domain = C2Component::DOMAIN_VIDEO;
        traits->kind = C2Component::KIND_DECODER;
        traits->mediaType = info->mediaType;
        traits->rank = kVideoRank;
        ret.push_back(traits);
    }
    ALOGD("listComponents: %zu component(s)", ret.size());
    return ret;
}

c2_status_t C2FFMPEGComponentStore::copyBuffer(
        std::shared_ptr<C2GraphicBuffer> /* src */,
        std::shared_ptr<C2GraphicBuffer> /* dst */) {
    return C2_OMITTED;
}

c2_status_t C2FFMPEGComponentStore::query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
}

c2_status_t C2FFMPEGComponentStore::config_sm(
        const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    return mInterface.config(params, C2_MAY_BLOCK, failures);
}

std::shared_ptr<C2ParamReflector> C2FFMPEGComponentStore::getParamReflector() const {
    return mReflectorHelper;
}

c2_status_t C2FFMPEGComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    return mInterface.querySupportedParams(params);
}

c2_status_t C2FFMPEGComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
}

} // namespace android
