/*
 * Copyright (C) 2026 The WT20 Android build
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

#define LOG_TAG "C2FFMPEGVaapi"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <android-base/properties.h>
#include <log/log.h>

#include <C2AllocatorGralloc.h>
#include <aidl/android/hardware/graphics/common/PlaneLayout.h>
#include <aidl/android/hardware/graphics/common/PlaneLayoutComponentType.h>
#include <cros_gralloc/cros_gralloc_handle.h>
#include <gralloctypes/Gralloc4.h>
#include <ui/GraphicBufferMapper.h>

#include <va/va_drm.h>
#include <va/va_drmcommon.h>  // VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME
#include <va/va_vpp.h>

extern "C" {
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"
}

#include "C2FFMPEGVaapi.h"

namespace android {
namespace wt20 {

using ::aidl::android::hardware::graphics::common::PlaneLayout;
using ::aidl::android::hardware::graphics::common::PlaneLayoutComponent;
using ::aidl::android::hardware::graphics::common::PlaneLayoutComponentType;

namespace {

// external/minigbm-upstream/cros_gralloc/cros_gralloc_helpers.h: cros_gralloc_magic
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;
// DRM_FORMAT_NV12 = fourcc_code('N', 'V', '1', '2'), DRM_FORMAT_MOD_LINEAR = 0 (drm_fourcc.h)
constexpr uint32_t kDrmFormatNv12 = 0x3231564E;
constexpr uint64_t kDrmFormatModLinear = 0;
// i965_device_info.c: Gen8 (Broadwell) min_linear_wpitch = 64, min_linear_hpitch = 4.
constexpr uint32_t kI965LinearPitchAlign = 64;
constexpr uint32_t kI965LinearHeightAlign = 4;
// Imported gralloc buffers kept as VA surfaces. An imported surface holds a GEM reference to
// the dmabuf, so a cached entry keeps the buffer's memory alive (~3.1 MB at 1080p) after the
// pool has dropped it (setOutputSurface, an IGBA reallocation at the same size).
//  - kMaxCachedSurfaces bounds the cache: CCodec dequeues at most output delay (H.264 up to
//    34) + 4 (smoothness) + 3 (rendering depth) buffers, ~41. A smaller cap would make the LRU
//    evict exactly the buffer that comes back next, so every frame would be imported again.
//  - kSurfaceIdleCopies retires buffers that are no longer in the pool: a buffer in the
//    rotation comes back within ~41 copies, one that did not come back for 64 copies is gone
//    (or held by the consumer; it is then simply imported again).
constexpr size_t kMaxCachedSurfaces = 48;
constexpr uint64_t kSurfaceIdleCopies = 64;
// Consecutive VPP failures after which this stream uses the CPU copy only.
constexpr int kMaxVppFailures = 3;

std::atomic<int> gHwSessions{0};

std::mutex gProbeLock;
bool gProbed = false;
VaProbeResult gProbe;

int OpenVaDisplay(VADisplay *display) {
    int fd = open(kVaRenderNode, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        ALOGE("cannot open %s: %s", kVaRenderNode, strerror(err));
        return -err;
    }
    VADisplay dpy = vaGetDisplayDRM(fd);
    if (!dpy) {
        ALOGE("vaGetDisplayDRM(%s) failed", kVaRenderNode);
        close(fd);
        return -ENODEV;
    }
    int major = 0, minor = 0;
    VAStatus st = vaInitialize(dpy, &major, &minor);
    if (st != VA_STATUS_SUCCESS) {
        ALOGE("vaInitialize failed: %s", vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return -ENODEV;
    }
    *display = dpy;
    return fd;
}

bool HasEntrypoint(VADisplay dpy, VAProfile profile, VAEntrypoint entrypoint) {
    int max = vaMaxNumEntrypoints(dpy);
    if (max <= 0) {
        return false;
    }
    std::vector<VAEntrypoint> entrypoints(max);
    int num = 0;
    if (vaQueryConfigEntrypoints(dpy, profile, entrypoints.data(), &num) != VA_STATUS_SUCCESS) {
        return false;
    }
    for (int i = 0; i < num && i < max; i++) {
        if (entrypoints[i] == entrypoint) {
            return true;
        }
    }
    return false;
}

void FreeVaDevice(AVHWDeviceContext *ctx) {
    AVVAAPIDeviceContext *hwctx = static_cast<AVVAAPIDeviceContext *>(ctx->hwctx);
    if (hwctx->display) {
        vaTerminate(hwctx->display);
        hwctx->display = nullptr;
    }
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx->user_opaque));
    if (fd >= 0) {
        close(fd);
    }
    ctx->user_opaque = reinterpret_cast<void *>(static_cast<intptr_t>(-1));
}

bool IsComponent(const PlaneLayoutComponent &component, PlaneLayoutComponentType type) {
    return gralloc4::isStandardPlaneLayoutComponentType(component.type) &&
           static_cast<PlaneLayoutComponentType>(component.type.value) == type;
}

// Layout from the gralloc mapper (minigbm's stable-C mapper on this device).
bool LayoutFromMapper(const native_handle_t *nh, Nv12BufferLayout *out) {
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
    buffer_handle_t buffer = nullptr;
    if (mapper.importBufferNoValidate(nh, &buffer) != OK || buffer == nullptr) {
        return false;
    }

    bool ok = false;
    std::vector<PlaneLayout> planes;
    if (mapper.getPlaneLayouts(buffer, &planes) == OK && planes.size() == 2) {
        const PlaneLayout &y = planes[0];
        const PlaneLayout &uv = planes[1];
        ok = y.components.size() == 1 &&
             IsComponent(y.components[0], PlaneLayoutComponentType::Y) &&
             y.components[0].offsetInBits == 0 && y.components[0].sizeInBits == 8 &&
             y.sampleIncrementInBits == 8 &&
             uv.components.size() == 2 &&
             IsComponent(uv.components[0], PlaneLayoutComponentType::CB) &&
             uv.components[0].offsetInBits == 0 && uv.components[0].sizeInBits == 8 &&
             IsComponent(uv.components[1], PlaneLayoutComponentType::CR) &&
             uv.components[1].offsetInBits == 8 && uv.components[1].sizeInBits == 8 &&
             uv.sampleIncrementInBits == 16 &&
             uv.horizontalSubsampling == 2 && uv.verticalSubsampling == 2 &&
             y.offsetInBytes >= 0 && uv.offsetInBytes >= 0 &&
             y.strideInBytes > 0 && uv.strideInBytes > 0 &&
             uv.offsetInBytes <= UINT32_MAX && y.strideInBytes <= UINT32_MAX &&
             uv.strideInBytes <= UINT32_MAX;
        if (ok) {
            out->pitch[0] = static_cast<uint32_t>(y.strideInBytes);
            out->pitch[1] = static_cast<uint32_t>(uv.strideInBytes);
            out->offset[0] = static_cast<uint32_t>(y.offsetInBytes);
            out->offset[1] = static_cast<uint32_t>(uv.offsetInBytes);
            out->source = "mapper";
        }
    }
    if (ok) {
        uint64_t value = 0;
        if (mapper.getPixelFormatModifier(buffer, &value) == OK) {
            out->modifier = value;
            out->modifierKnown = true;
        }
        // minigbm reports total_size here (dmabuf + the separate metadata region): only a
        // last resort when the dmabuf cannot be lseek()ed, see VaSurfaceCopier::copy().
        if (mapper.getAllocationSize(buffer, &value) == OK && value > 0) {
            out->size = value;
        }
    }
    mapper.freeBuffer(buffer);
    return ok;
}

// Layout from minigbm's own handle (external/minigbm-upstream/cros_gralloc/cros_gralloc_handle.h).
bool LayoutFromCrosHandle(const native_handle_t *nh, Nv12BufferLayout *out) {
    constexpr int kHandleInts =
            (sizeof(cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int);
    if (nh->numFds < 1 || nh->numFds + nh->numInts != kHandleInts) {
        return false;
    }
    const cros_gralloc_handle *h = reinterpret_cast<const cros_gralloc_handle *>(nh);
    if (h->magic != kCrosGrallocMagic || h->format != kDrmFormatNv12 || h->num_planes != 2) {
        return false;
    }
    out->pitch[0] = h->strides[0];
    out->pitch[1] = h->strides[1];
    out->offset[0] = h->offsets[0];
    out->offset[1] = h->offsets[1];
    // total_size = reserved_region_size + bo size (cros_gralloc_driver.cc); the reserved
    // (metadata) region is its own fd, not part of the planes' dmabuf.
    out->size = h->total_size >= h->reserved_region_size
                        ? h->total_size - h->reserved_region_size : 0;
    out->modifier = h->format_modifier;
    out->modifierKnown = true;
    out->source = "cros_gralloc_handle";
    return true;
}

bool SameDmabuf(int fd1, int fd2) {
    struct stat s1, s2;
    if (fstat(fd1, &s1) != 0 || fstat(fd2, &s2) != 0) {
        return false;
    }
    return s1.st_ino == s2.st_ino && s1.st_dev == s2.st_dev;
}

/*
 * i965_suface_external_memory() (i965_drv_video.c, legacy DRM_PRIME import) uses the bo from
 * buffers[0] with offsets[0] ignored (Y at the start of the bo), pitches[0] 16- (Gen8: 64-)
 * aligned and >= width, height = offsets[1] / pitches[0] >= surface height and 4-aligned, and
 * pitches[1] == pitches[0]. It cannot tell a modifier-tiled bo from a linear one, so only
 * linear buffers are accepted here.
 */
bool CheckI965Constraints(const Nv12BufferLayout &l, const char **why) {
    if (l.modifierKnown && l.modifier != kDrmFormatModLinear) {
        *why = "buffer is not linear";
        return false;
    }
    if (l.offset[0] != 0) {
        *why = "Y plane does not start at offset 0";
        return false;
    }
    if (l.pitch[0] != l.pitch[1]) {
        *why = "Y and UV pitches differ";
        return false;
    }
    if (l.pitch[0] % kI965LinearPitchAlign != 0 || l.pitch[0] < l.width) {
        *why = "pitch not 64-aligned or smaller than the width";
        return false;
    }
    if (l.offset[1] % l.pitch[0] != 0) {
        *why = "UV offset is not a whole number of rows";
        return false;
    }
    uint32_t rows = l.offset[1] / l.pitch[0];
    if (rows < l.height || rows % kI965LinearHeightAlign != 0) {
        *why = "UV offset rows below the height or not 4-aligned";
        return false;
    }
    uint64_t needed = uint64_t(l.offset[1]) + uint64_t(l.pitch[1]) * ((l.height + 1) / 2);
    if (l.size < needed || l.size > UINT32_MAX) {
        *why = "buffer size does not cover both planes";
        return false;
    }
    return true;
}

}  // namespace

bool HwDecodeEnabled() {
    return base::GetBoolProperty("persist.vendor.wt20.hwdec", true);
}

bool SwFallbackEnabled() {
    return base::GetBoolProperty("persist.vendor.wt20.hwdec.swfallback", true);
}

VaProbeResult ProbeVa() {
    std::lock_guard<std::mutex> lock(gProbeLock);
    if (gProbed) {
        return gProbe;
    }

    // pc_gpu_detect waits for the i915 module at post-fs-data, before class hal starts, so the
    // node is normally there. If it is not, wait a little and do not cache the miss.
    for (int i = 0; i < 30 && access(kVaRenderNode, F_OK) != 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (access(kVaRenderNode, F_OK) != 0) {
        ALOGW("ProbeVa: %s does not exist (yet); no component listed", kVaRenderNode);
        return VaProbeResult();
    }

    VaProbeResult result;
    VADisplay dpy = nullptr;
    int fd = OpenVaDisplay(&dpy);
    if (fd >= 0) {
        result.display = true;
        result.h264 = HasEntrypoint(dpy, VAProfileH264ConstrainedBaseline, VAEntrypointVLD) &&
                      HasEntrypoint(dpy, VAProfileH264Main, VAEntrypointVLD) &&
                      HasEntrypoint(dpy, VAProfileH264High, VAEntrypointVLD);
        result.vp8 = HasEntrypoint(dpy, VAProfileVP8Version0_3, VAEntrypointVLD);
        result.mpeg2 = HasEntrypoint(dpy, VAProfileMPEG2Simple, VAEntrypointVLD) &&
                       HasEntrypoint(dpy, VAProfileMPEG2Main, VAEntrypointVLD);
        result.vpp = HasEntrypoint(dpy, VAProfileNone, VAEntrypointVideoProc);
        ALOGI("ProbeVa: %s: vendor \"%s\", h264=%d vp8=%d mpeg2=%d vpp=%d", kVaRenderNode,
              vaQueryVendorString(dpy) ? vaQueryVendorString(dpy) : "?", result.h264,
              result.vp8, result.mpeg2, result.vpp);
        vaTerminate(dpy);
        close(fd);
    } else {
        ALOGE("ProbeVa: no VA display on %s; no component listed", kVaRenderNode);
    }
    gProbe = result;
    gProbed = true;
    return gProbe;
}

bool VaSupportsCodec(enum AVCodecID codecId) {
    VaProbeResult probe = ProbeVa();
    switch (codecId) {
        case AV_CODEC_ID_H264:
            return probe.h264;
        case AV_CODEC_ID_VP8:
            return probe.vp8;
        case AV_CODEC_ID_MPEG2VIDEO:
            return probe.mpeg2;
        default:
            return false;
    }
}

int CreateVaapiDevice(AVBufferRef **device) {
    *device = nullptr;
    VADisplay dpy = nullptr;
    int fd = OpenVaDisplay(&dpy);
    if (fd < 0) {
        return AVERROR(-fd);
    }
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    if (!ref) {
        vaTerminate(dpy);
        close(fd);
        return AVERROR(ENOMEM);
    }
    AVHWDeviceContext *ctx = reinterpret_cast<AVHWDeviceContext *>(ref->data);
    AVVAAPIDeviceContext *hwctx = static_cast<AVVAAPIDeviceContext *>(ctx->hwctx);
    hwctx->display = dpy;
    ctx->user_opaque = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
    ctx->free = FreeVaDevice;
    int err = av_hwdevice_ctx_init(ref);
    if (err < 0) {
        ALOGE("av_hwdevice_ctx_init(VAAPI) failed: %s", av_err2str(err));
        av_buffer_unref(&ref);  // runs FreeVaDevice
        return err;
    }
    *device = ref;
    return 0;
}

bool AcquireHwSession() {
    int current = gHwSessions.load();
    while (current < kMaxHwSessions) {
        if (gHwSessions.compare_exchange_weak(current, current + 1)) {
            return true;
        }
    }
    return false;
}

void ReleaseHwSession() {
    gHwSessions.fetch_sub(1);
}

VaSurfaceCopier::~VaSurfaceCopier() {
    reset();
}

void VaSurfaceCopier::reset() {
    if (mDisplay) {
        for (Entry &entry : mCache) {
            vaDestroySurfaces(mDisplay, &entry.surface, 1);
        }
        if (mContext != VA_INVALID_ID) {
            vaDestroyContext(mDisplay, mContext);
        }
        if (mConfig != VA_INVALID_ID) {
            vaDestroyConfig(mDisplay, mConfig);
        }
    }
    mCache.clear();
    mRejected.clear();
    mContext = VA_INVALID_ID;
    mConfig = VA_INVALID_ID;
    mNoVpp = false;
    mVppFailures = 0;
    mCopies = 0;
    mDisplay = nullptr;
}

c2_status_t VaSurfaceCopier::ensureContext(uint32_t width, uint32_t height) {
    if (mContext != VA_INVALID_ID) {
        return C2_OK;
    }
    VAStatus st = vaCreateConfig(mDisplay, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                 &mConfig);
    if (st != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateConfig(VideoProc) failed: %s; CPU copies only", vaErrorStr(st));
        mConfig = VA_INVALID_ID;
        mNoVpp = true;
        return C2_OMITTED;
    }
    st = vaCreateContext(mDisplay, mConfig, width, height, VA_PROGRESSIVE, nullptr, 0, &mContext);
    if (st != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateContext(VideoProc) failed: %s; CPU copies only", vaErrorStr(st));
        vaDestroyConfig(mDisplay, mConfig);
        mConfig = VA_INVALID_ID;
        mContext = VA_INVALID_ID;
        mNoVpp = true;
        return C2_OMITTED;
    }
    return C2_OK;
}

c2_status_t VaSurfaceCopier::importSurface(const Nv12BufferLayout &l, VASurfaceID *surface) {
    uintptr_t buffer = static_cast<uintptr_t>(l.fd);
    VASurfaceAttribExternalBuffers ext;
    memset(&ext, 0, sizeof(ext));
    ext.pixel_format = VA_FOURCC_NV12;
    ext.width = l.width;
    ext.height = l.height;
    ext.data_size = static_cast<uint32_t>(l.size);
    ext.num_planes = 2;
    ext.pitches[0] = l.pitch[0];
    ext.pitches[1] = l.pitch[1];
    ext.offsets[0] = l.offset[0];
    ext.offsets[1] = l.offset[1];
    ext.buffers = &buffer;
    ext.num_buffers = 1;
    ext.flags = 0;
    ext.private_data = nullptr;

    VASurfaceAttrib attribs[2];
    memset(attribs, 0, sizeof(attribs));
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &ext;

    VAStatus st = vaCreateSurfaces(mDisplay, VA_RT_FORMAT_YUV420, l.width, l.height, surface, 1,
                                   attribs, 2);
    if (st != VA_STATUS_SUCCESS) {
        ALOGE("vaCreateSurfaces(DRM_PRIME %ux%u pitch %u uv@%u size %llu, %s) failed: %s",
              l.width, l.height, l.pitch[0], l.offset[1], (unsigned long long)l.size, l.source,
              vaErrorStr(st));
        *surface = VA_INVALID_SURFACE;
        return C2_OMITTED;
    }
    return C2_OK;
}

void VaSurfaceCopier::dropEntry(std::list<Entry>::iterator it) {
    vaDestroySurfaces(mDisplay, &it->surface, 1);
    mCache.erase(it);
}

c2_status_t VaSurfaceCopier::copy(VADisplay display, VASurfaceID src, const VARectangle &srcRect,
                                  const C2GraphicBlock &dst) {
    if (display != mDisplay) {
        reset();
        mDisplay = display;
        // The store lists components without VideoProc too (the CPU copy still works): do
        // not even try to create the VPP context then.
        if (!ProbeVa().vpp) {
            ALOGI("no VideoProc entrypoint: VA frames are copied by the CPU");
            mNoVpp = true;
        } else if (!base::GetBoolProperty("debug.wt20.vpp", true)) {
            // debug.wt20.vpp=0 takes the CPU copy (vaGetImage + memcpy): ~20 fps at 1080p
            // against ~33 with VPP, for comparing the two paths.
            ALOGI("VPP copy off (debug.wt20.vpp): VA frames are copied by the CPU");
            mNoVpp = true;
        }
    }
    if (mNoVpp) {
        return C2_OMITTED;
    }
    mCopies++;
    const uint32_t width = dst.width();
    const uint32_t height = dst.height();
    if (srcRect.width > width || srcRect.height > height) {
        return C2_OMITTED;
    }

    const C2Handle *handle = dst.handle();
    native_handle_t *nh = handle ? UnwrapNativeCodec2GrallocHandle(handle) : nullptr;
    if (!nh) {
        return C2_OMITTED;
    }
    // The unwrapped handle shares the block's fds: delete it, never close it.
    std::unique_ptr<native_handle_t, int (*)(native_handle_t *)> nhHolder(nh,
                                                                          native_handle_delete);
    if (nh->numFds < 1 || nh->data[0] < 0) {
        return C2_OMITTED;
    }
    const int fd = nh->data[0];
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return C2_OMITTED;
    }
    const Key key(st.st_ino, st.st_dev, width, height);
    if (mRejected.count(key)) {
        return C2_OMITTED;
    }

    VASurfaceID dstSurface = VA_INVALID_SURFACE;
    auto it = mCache.begin();
    for (; it != mCache.end(); ++it) {
        if (it->key == key) {
            break;
        }
    }
    if (it != mCache.end()) {
        mCache.splice(mCache.begin(), mCache, it);
        mCache.front().lastUse = mCopies;
        dstSurface = mCache.front().surface;
    } else {
        Nv12BufferLayout layout;
        layout.fd = fd;
        layout.width = width;
        layout.height = height;
        bool haveMapper = LayoutFromMapper(nh, &layout);
        Nv12BufferLayout cros = layout;
        bool haveCros = LayoutFromCrosHandle(nh, &cros);
        if (!haveMapper && haveCros) {
            layout = cros;
        } else if (haveMapper && haveCros &&
                   (layout.pitch[0] != cros.pitch[0] || layout.offset[1] != cros.offset[1])) {
            ALOGW("mapper and cros_gralloc_handle disagree (pitch %u/%u, uv@%u/%u); using mapper",
                  layout.pitch[0], cros.pitch[0], layout.offset[1], cros.offset[1]);
        }
        if (haveMapper && haveCros && !layout.modifierKnown) {
            layout.modifier = cros.modifier;
            layout.modifierKnown = true;
        }
        // The real dmabuf size (what libdrm's create_from_prime uses too). minigbm's
        // total_size, and so getAllocationSize, also counts the metadata region, which is a
        // separate fd: use them only if lseek fails, the handle's value (minus that region)
        // first.
        off_t end = lseek(fd, 0, SEEK_END);
        if (end > 0) {
            layout.size = static_cast<uint64_t>(end);
        } else if (haveCros && cros.size > 0) {
            layout.size = cros.size;
        }
        const char *why = nullptr;
        if (!haveMapper && !haveCros) {
            why = "no NV12 plane layout from the mapper or the minigbm handle";
        } else if (nh->numFds > 1 && nh->data[1] >= 0 && !SameDmabuf(fd, nh->data[1])) {
            why = "UV plane is in a different dmabuf";
        } else {
            CheckI965Constraints(layout, &why);
        }
        if (why) {
            ALOGW("gralloc buffer %ux%u (pitch %u, uv@%u, %s) cannot be imported: %s; CPU copy",
                  width, height, layout.pitch[0], layout.offset[1], layout.source, why);
            mRejected.insert(key);
            return C2_OMITTED;
        }
        c2_status_t err = ensureContext(width, height);
        if (err != C2_OK) {
            return err;
        }
        err = importSurface(layout, &dstSurface);
        if (err != C2_OK) {
            mRejected.insert(key);
            return err;
        }
        // A new allocation size retires the buffers of the old one.
        for (auto e = mCache.begin(); e != mCache.end();) {
            auto next = std::next(e);
            if (std::get<2>(e->key) != width || std::get<3>(e->key) != height) {
                dropEntry(e);
            }
            e = next;
        }
        mCache.push_front(Entry{key, dstSurface, mCopies});
        while (mCache.size() > kMaxCachedSurfaces) {
            dropEntry(std::prev(mCache.end()));
        }
        ALOGD("imported gralloc buffer %ux%u pitch %u uv@%u size %llu (%s) as VA surface %#x "
              "(%zu cached)", width, height, layout.pitch[0], layout.offset[1],
              (unsigned long long)layout.size, layout.source, dstSurface, mCache.size());
    }
    // Retire buffers that left the pool (the list is most recently used first).
    while (!mCache.empty() && mCopies - mCache.back().lastUse > kSurfaceIdleCopies) {
        dropEntry(std::prev(mCache.end()));
    }

    VARectangle outRect;
    outRect.x = 0;
    outRect.y = 0;
    outRect.width = srcRect.width;
    outRect.height = srcRect.height;

    VAProcPipelineParameterBuffer params;
    memset(&params, 0, sizeof(params));
    params.surface = src;
    params.surface_region = &srcRect;
    params.surface_color_standard = VAProcColorStandardBT601;
    params.output_region = &outRect;
    params.output_background_color = 0xff000000;
    params.output_color_standard = VAProcColorStandardBT601;
    // Same size, same fourcc: i965_proc_picture_fast picks PP_NV12_LOAD_SAVE_N12, a plain copy.
    params.pipeline_flags = VA_PROC_PIPELINE_FAST;
    params.filter_flags = VA_FRAME_PICTURE | VA_FILTER_SCALING_FAST;

    VABufferID paramsBuffer = VA_INVALID_ID;
    VAStatus vst = vaCreateBuffer(mDisplay, mContext, VAProcPipelineParameterBufferType,
                                  sizeof(params), 1, &params, &paramsBuffer);
    const char *failed = nullptr;
    if (vst != VA_STATUS_SUCCESS) {
        failed = "vaCreateBuffer";
    } else if ((vst = vaBeginPicture(mDisplay, mContext, dstSurface)) != VA_STATUS_SUCCESS) {
        failed = "vaBeginPicture";
    } else {
        vst = vaRenderPicture(mDisplay, mContext, &paramsBuffer, 1);
        VAStatus endSt = vaEndPicture(mDisplay, mContext);
        if (vst != VA_STATUS_SUCCESS) {
            failed = "vaRenderPicture";
        } else if ((vst = endSt) != VA_STATUS_SUCCESS) {
            failed = "vaEndPicture";
        } else if ((vst = vaSyncSurface(mDisplay, dstSurface)) != VA_STATUS_SUCCESS) {
            failed = "vaSyncSurface";
        }
    }
    if (paramsBuffer != VA_INVALID_ID) {
        vaDestroyBuffer(mDisplay, paramsBuffer);
    }
    if (failed) {
        mVppFailures++;
        ALOGE("VPP copy %#x -> %#x: %s failed: %s (%d of %d)", src, dstSurface, failed,
              vaErrorStr(vst), mVppFailures, kMaxVppFailures);
        // Import this buffer again next time.
        for (auto e = mCache.begin(); e != mCache.end(); ++e) {
            if (e->key == key) {
                dropEntry(e);
                break;
            }
        }
        if (mVppFailures >= kMaxVppFailures) {
            // VA decode itself is not affected: keep it, and copy with the CPU from now on.
            ALOGW("VPP copy failed %d times in a row: CPU copies for the rest of this stream",
                  mVppFailures);
            mNoVpp = true;
        }
        return C2_CORRUPTED;
    }
    mVppFailures = 0;
    return C2_OK;
}

}  // namespace wt20
}  // namespace android
