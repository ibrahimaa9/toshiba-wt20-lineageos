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

/*
 * VA-API support for the WT20 (Broadwell HD 5300, i965_drv_video) FFmpeg Codec2 HAL:
 *
 *  - the kill switch and software fallback properties,
 *  - the VA probe the component store uses before it lists any component,
 *  - the FFmpeg VAAPI device on /dev/dri/renderD128,
 *  - the one GPU (VPP) copy from an FFmpeg-owned VA surface into the gralloc buffer of a
 *    C2GraphicBlock, imported with the legacy VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME path
 *    (i965 2.4 does not accept DRM_PRIME_2: i965_drv_video.c, i965_CreateSurfaces2).
 */

#ifndef C2_FFMPEG_VAAPI_H
#define C2_FFMPEG_VAAPI_H

#include <stdint.h>

#include <list>
#include <memory>
#include <set>
#include <tuple>

#include <C2Buffer.h>
#include <va/va.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/buffer.h"
}

namespace android {
namespace wt20 {

// The only render node of the WT20 (i915 HD 5300).
constexpr char kVaRenderNode[] = "/dev/dri/renderD128";

// At most this many components decode on the GPU at the same time; further instances decode in
// software inside the same component (FFmpeg's VA frame pool is about 65 MB per 1080p stream).
constexpr int kMaxHwSessions = 4;

// H.264 levels above 5.1 and pictures larger than 1920x1088 (8160 macroblocks, either
// orientation) are decoded in software.
constexpr int kMaxHwH264Level = 51;
constexpr int kMaxHwDimension = 1920;
constexpr int kMaxHwMacroblocks = (1920 / 16) * (1088 / 16);

// persist.vendor.wt20.hwdec (default true): false lists no component at all.
bool HwDecodeEnabled();

// persist.vendor.wt20.hwdec.swfallback (default true): false turns every case where the
// component would continue in software (stream outside the hardware limits, VA errors, too many
// instances) into C2_CORRUPTED / C2_NO_MEMORY instead.
bool SwFallbackEnabled();

struct VaProbeResult {
    bool display = false;  // vaGetDisplayDRM + vaInitialize worked
    bool h264 = false;     // VLD for H264ConstrainedBaseline, H264Main and H264High
    bool vp8 = false;      // VLD for VP8Version0_3
    bool mpeg2 = false;    // VLD for MPEG2Simple and MPEG2Main
    bool vpp = false;      // VideoProc on VAProfileNone (the GPU output copy)
};

// Probes the VA driver once per process (a missing render node is retried on the next call).
VaProbeResult ProbeVa();

// True when the probe found VLD decode for this codec.
bool VaSupportsCodec(enum AVCodecID codecId);

// Creates an FFmpeg VAAPI device on its own DRM display. Returns 0 or a negative AVERROR.
// The display is terminated and the fd closed when the last reference goes away.
int CreateVaapiDevice(AVBufferRef **device);

// Hardware session accounting (kMaxHwSessions).
bool AcquireHwSession();
void ReleaseHwSession();

// Layout of a linear NV12 gralloc buffer, as gralloc reports it.
struct Nv12BufferLayout {
    int fd = -1;            // dmabuf of both planes (not owned)
    uint32_t width = 0;     // allocation size in pixels
    uint32_t height = 0;
    uint32_t pitch[2] = {0, 0};
    uint32_t offset[2] = {0, 0};
    uint64_t size = 0;      // size of the dmabuf in bytes (lseek; not minigbm's total_size,
                            // which adds the separate metadata region)
    uint64_t modifier = 0;
    bool modifierKnown = false;
    const char *source = "none";  // "mapper" or "cros_gralloc_handle"
};

/*
 * Copies decoded VA surfaces into gralloc buffers with one VPP pass.
 * Not thread safe: the component calls it from its work thread only.
 */
class VaSurfaceCopier {
public:
    VaSurfaceCopier() = default;
    ~VaSurfaceCopier();

    VaSurfaceCopier(const VaSurfaceCopier &) = delete;
    VaSurfaceCopier &operator=(const VaSurfaceCopier &) = delete;

    /*
     * Copies |srcRect| of |src| (a surface of |display|) to (0,0) of |dst|.
     *
     * \retval C2_OK        copied, and the GPU is done writing (vaSyncSurface)
     * \retval C2_OMITTED   this buffer cannot be imported into VA (its layout does not meet the
     *                      i965 constraints), or VPP is off for this stream (no VideoProc, or
     *                      kMaxVppFailures in a row): use the CPU path for it
     * \retval C2_CORRUPTED a VPP call failed: use the CPU path for this frame. This is not a
     *                      decode error (the VA surface is fine), so the caller must not count
     *                      it toward the software fallback; the copier itself turns VPP off
     *                      after kMaxVppFailures consecutive failures.
     */
    c2_status_t copy(VADisplay display, VASurfaceID src, const VARectangle &srcRect,
                     const C2GraphicBlock &dst);

    // Destroys the imported surfaces and the VPP context. Must run before the display goes away.
    void reset();

private:
    // dmabuf inode, device, allocation width and height
    typedef std::tuple<uint64_t, uint64_t, uint32_t, uint32_t> Key;

    struct Entry {
        Key key;
        VASurfaceID surface;
        uint64_t lastUse;  // mCopies when this buffer was last copied to
    };

    c2_status_t ensureContext(uint32_t width, uint32_t height);
    c2_status_t importSurface(const Nv12BufferLayout &layout, VASurfaceID *surface);
    void dropEntry(std::list<Entry>::iterator it);

    VADisplay mDisplay = nullptr;
    VAConfigID mConfig = VA_INVALID_ID;
    VAContextID mContext = VA_INVALID_ID;
    bool mNoVpp = false;
    int mVppFailures = 0;     // consecutive VPP failures
    uint64_t mCopies = 0;     // copy() calls since reset()
    std::list<Entry> mCache;  // most recently used first
    std::set<Key> mRejected;  // buffers that cannot be imported (CPU path)
};

}  // namespace wt20
}  // namespace android

#endif  // C2_FFMPEG_VAAPI_H
