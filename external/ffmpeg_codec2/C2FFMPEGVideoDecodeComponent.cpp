/*
 * Copyright 2022 Michael Goffioul <michael.goffioul@gmail.com>
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

#define LOG_TAG "C2FFMPEGVideoDecodeComponent"
#include <android-base/properties.h>
#include <log/log.h>
#include <string.h>
#include <algorithm>

#include <SimpleC2Interface.h>
#include "C2FFMPEGVideoDecodeComponent.h"

extern "C" {
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"
#include "libavutil/pixdesc.h"
}

#include <android/hardware/graphics/common/1.0/types.h>

using android::hardware::graphics::common::V1_0::BufferUsage;

#define DEBUG_FRAMES 0
#define DEBUG_WORKQUEUE 0
#define DEBUG_EXTRADATA 0

namespace android {

namespace {

// WT20: after this many consecutive VA errors the stream continues in software. FFmpeg's
// software decoders then resume at the next IDR / recovery point (H.264 does not output
// unrecovered frames, VP8 discards inter frames without a key frame).
constexpr int kMaxHwErrors = 3;

// VA surfaces on top of FFmpeg's DPB-sized pool: mFrame is held between two
// avcodec_receive_frame() calls while it is copied out.
constexpr int kExtraHwFrames = 2;

// Output buffers are allocated larger than the picture and cropped to it, for the i965 import
// of the GPU copy (CheckI965Constraints in C2FFMPEGVaapi.cpp):
//  - width 64-aligned: minigbm's gbm_mesa backend makes a VIDEO_DECODER NV12 buffer with
//    pitch = ALIGN(width, 32) and nothing more (gbm_mesa_bo_create, drv_bo_from_format), and
//    Gen8 i965 needs the pitch 64-aligned (min_linear_wpitch). With 16 (or 32) every width
//    that is an odd multiple of 32 (352, 480, 720, 854->864, 1440) could only use the CPU
//    copy. C2SoftAvcDec over-allocates the same way (ALIGN128).
//  - height 16-aligned (the macroblock-aligned coded height): the UV plane starts at
//    pitch * allocHeight, a whole number of rows >= the height and 4-aligned, as i965 wants.
// The same size for hardware and software frames keeps the pool from reallocating when a
// stream changes path.
constexpr uint32_t kOutputWidthAlign = 64;
constexpr uint32_t kOutputHeightAlign = 16;

// Packets kept until the first get_format of a VA instance (normally one or two: parameter
// sets and the first picture), so that a stream that turns out to need software decoding can
// be fed again to a decoder opened with threads.
constexpr size_t kMaxPrerollPackets = 16;
constexpr size_t kMaxPrerollBytes = 8u << 20;

bool IsHwSwFormat(int format) {
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P ||
           format == AV_PIX_FMT_NV12;
}

// Bitstream errors are the stream's fault, not the GPU's.
bool IsVaError(int err) {
    return err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF &&
           err != AVERROR_INVALIDDATA;
}

uint32_t AlignUp(uint32_t value, uint32_t align) {
    return (value + align - 1) / align * align;
}

}  // namespace

C2FFMPEGVideoDecodeComponent::C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<C2FFMPEGVideoDecodeInterface>>(componentInfo->name, 0, intf)),
      mInfo(componentInfo),
      mIntf(intf),
      mCodecID(componentInfo->codecID),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mSwFrame(NULL),
      mPacket(NULL),
      mFFMPEGInitialized(false),
      mCodecAlreadyOpened(false),
      mExtradataReady(false),
      mEOSSignalled(false),
      mHwDevice(NULL),
      mHwSession(false),
      mForceSoftware(false),
      mSoftwareReason(nullptr),
      mHwRefused(false),
      mSwitchToSoftware(false),
      mLoggedFormat(false),
      mLoggedCpuCopy(false),
      mHwErrors(0),
      mInSend(false),
      mFormatInSend(false),
      mFormatDecided(false),
      mReopenReason(nullptr),
      mPrerollOverflow(false),
      mPrerollBytes(0) {
    ALOGD("C2FFMPEGVideoDecodeComponent: mediaType = %s", componentInfo->mediaType);
}

C2FFMPEGVideoDecodeComponent::~C2FFMPEGVideoDecodeComponent() {
    ALOGD("~C2FFMPEGVideoDecodeComponent: mCtx = %p", mCtx);
    onRelease();
}

c2_status_t C2FFMPEGVideoDecodeComponent::initDecoder() {
    if (! mFFMPEGInitialized) {
        if (initFFmpeg() != C2_OK) {
            ALOGE("initDecoder: FFMPEG initialization failed.");
            return C2_NO_INIT;
        }
        mFFMPEGInitialized = true;
    }

    // SimpleC2Component::start() after reset() calls onInit() again, and onReset() has already
    // made a fresh context: free it (and anything opened on it) instead of leaking it.
    if (mCtx) {
        closeCodec();
    }

    mCtx = avcodec_alloc_context3(NULL);
    if (! mCtx) {
        ALOGE("initDecoder: avcodec_alloc_context failed.");
        return C2_NO_MEMORY;
    }

    C2StreamPictureSizeInfo::output size(0u, 320, 240);
    c2_status_t err = mIntf->query({ &size }, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK) {
        ALOGE("initDecoder: cannot query picture size, err = %d", err);
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    mCtx->codec_id = mCodecID;
    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = size.width;
    mCtx->height = size.height;

    mCtx->workaround_bugs   = 1;
    mCtx->idct_algo         = 0;
    mCtx->skip_frame        = AVDISCARD_DEFAULT;
    mCtx->skip_idct         = AVDISCARD_DEFAULT;
    mCtx->skip_loop_filter  = AVDISCARD_DEFAULT;
    mCtx->error_concealment = 3;

    if (base::GetBoolProperty("debug.ffmpeg_codec2.fast", false)) {
        mCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    ALOGD("initDecoder: %p [%s], %d x %d, %s",
          mCtx, avcodec_get_name(mCtx->codec_id), size.width, size.height, mInfo->mediaType);

    return C2_OK;
}

enum AVPixelFormat C2FFMPEGVideoDecodeComponent::GetFormat(
        AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    return static_cast<C2FFMPEGVideoDecodeComponent*>(ctx->opaque)->chooseFormat(ctx, fmts);
}

// FFmpeg calls this on every sequence header. If the hwaccel then fails to initialize, it
// calls it again without AV_PIX_FMT_VAAPI in |fmts|.
enum AVPixelFormat C2FFMPEGVideoDecodeComponent::chooseFormat(
        AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    bool vaOffered = false;
    enum AVPixelFormat sw = AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (*p == AV_PIX_FMT_VAAPI) {
            vaOffered = true;
        } else if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            if (sw == AV_PIX_FMT_NONE || *p == ctx->sw_pix_fmt) {
                sw = *p;
            }
        }
    }

    if (mInSend) {
        mFormatInSend = true;
    } else {
        // Called from avcodec_receive_frame() (the rest of a packet): too late to reopen.
        mFormatDecided = true;
    }

    const int mbs = ((ctx->coded_width + 15) / 16) * ((ctx->coded_height + 15) / 16);
    const char* why = nullptr;
    if (!ctx->hw_device_ctx) {
        why = mForceSoftware && mSoftwareReason ? mSoftwareReason
                                                : "no VA device for this instance";
    } else if (!vaOffered) {
        why = "no VA-API hwaccel for this profile";
    } else if (!IsHwSwFormat(ctx->sw_pix_fmt)) {
        why = "not 8-bit 4:2:0";
    } else if (mCodecID == AV_CODEC_ID_H264 && ctx->level > wt20::kMaxHwH264Level) {
        why = "H.264 level above 5.1";
    } else if (ctx->coded_width > wt20::kMaxHwDimension ||
               ctx->coded_height > wt20::kMaxHwDimension || mbs > wt20::kMaxHwMacroblocks) {
        why = "picture larger than 1920x1088";
    }

    if (!why) {
        // H.264 Baseline (profile_idc 66 without constraint_set1): FFmpeg's VA-API profile map
        // has only ConstrainedBaseline, Main and High (vaapi_decode.c vaapi_profile_map), so
        // without AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH the hwaccel refuses the stream (EINVAL)
        // and it would be decoded in software. Baseline adds only FMO, ASO and redundant
        // slices to ConstrainedBaseline, and FFmpeg's own decoder does none of them either
        // (h264_ps.c reports FMO as a missing feature, h264_slice.c drops redundant slices
        // before the hwaccel sees them), so the VA config FFmpeg then picks (the last
        // supported entry, VAProfileH264High; High is a superset of ConstrainedBaseline)
        // decodes such streams no worse than FFmpeg software would. Only for Baseline: every
        // other unmapped profile (Extended, the 10-bit / 4:2:2 / 4:4:4 ones) stays an exact
        // match and goes to software. hwaccel_flags may be set here (avcodec.h).
        const bool baseline = mCodecID == AV_CODEC_ID_H264 &&
                              ctx->profile == AV_PROFILE_H264_BASELINE;
        if (baseline) {
            ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
        } else {
            ctx->hwaccel_flags &= ~AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
        }
        if (!mLoggedFormat) {
            ALOGI("get_format: VA-API for %s %dx%d (%s), profile %d%s, level %d",
                  avcodec_get_name(mCodecID), ctx->coded_width, ctx->coded_height,
                  av_get_pix_fmt_name(ctx->sw_pix_fmt), ctx->profile,
                  baseline ? " (Baseline, decoded as High)" : "", ctx->level);
            mLoggedFormat = true;
        }
        mHwRefused = false;
        mReopenReason = nullptr;
        return AV_PIX_FMT_VAAPI;
    }

    if (ctx->hw_device_ctx && !wt20::SwFallbackEnabled()) {
        ALOGE("get_format: %s %dx%d (%s): %s, and persist.vendor.wt20.hwdec.swfallback=false",
              avcodec_get_name(mCodecID), ctx->coded_width, ctx->coded_height,
              av_get_pix_fmt_name(ctx->sw_pix_fmt), why);
        mHwRefused = true;
        return AV_PIX_FMT_NONE;
    }
    if (ctx->hw_device_ctx && mInSend && !mFormatDecided && !mPrerollOverflow) {
        // First decision of a VA instance: sendInputBuffer() reopens the decoder with threads
        // (this context has thread_count 1) and feeds it the preroll again.
        ALOGI("get_format: %s %dx%d (%s): %s; reopening for software decode with threads",
              avcodec_get_name(mCodecID), ctx->coded_width, ctx->coded_height,
              av_get_pix_fmt_name(ctx->sw_pix_fmt), why);
        mReopenReason = why;
        return sw;
    }
    ALOGI("get_format: software decode of %s %dx%d (%s): %s", avcodec_get_name(mCodecID),
          ctx->coded_width, ctx->coded_height, av_get_pix_fmt_name(ctx->sw_pix_fmt), why);
    mLoggedFormat = true;
    return sw;
}

c2_status_t C2FFMPEGVideoDecodeComponent::openCodec(bool allowHw) {
    const AVCodec* codec = avcodec_find_decoder(mCodecID);
    if (!codec) {
        ALOGE("openCodec: ffmpeg video decoder failed to find codec %d", mCodecID);
        return C2_NOT_FOUND;
    }
    if (!mCtx) {
        c2_status_t err = initDecoder();
        if (err != C2_OK) {
            return err;
        }
    }

    // Can't change extradata after opening the decoder.
    if (!mExtradata.empty() && !mCtx->extradata) {
        mCtx->extradata = (uint8_t*) av_mallocz(mExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!mCtx->extradata) {
            ALOGE("openCodec: oom for extradata");
            return C2_NO_MEMORY;
        }
        memcpy(mCtx->extradata, mExtradata.data(), mExtradata.size());
        mCtx->extradata_size = mExtradata.size();
    }
#if DEBUG_EXTRADATA
    ALOGD("openCodec: extradata_size = %d", mCtx->extradata_size);
#endif

    bool hw = false;
    if (allowHw) {
        if (!wt20::AcquireHwSession()) {
            ALOGW("openCodec: %d decoders already use the GPU", wt20::kMaxHwSessions);
        } else {
            mHwSession = true;
            int r = wt20::CreateVaapiDevice(&mHwDevice);
            if (r >= 0) {
                mCtx->hw_device_ctx = av_buffer_ref(mHwDevice);
            }
            if (r < 0 || !mCtx->hw_device_ctx) {
                ALOGE("openCodec: no VA device: %s", r < 0 ? av_err2str(r) : "out of memory");
                av_buffer_unref(&mHwDevice);
                wt20::ReleaseHwSession();
                mHwSession = false;
            } else {
                hw = true;
            }
        }
        if (!hw && !wt20::SwFallbackEnabled()) {
            ALOGE("openCodec: no hardware decoder and persist.vendor.wt20.hwdec.swfallback=false");
            closeCodec();
            return C2_NO_MEMORY;
        }
    }

    mCtx->opaque = this;
    mCtx->get_format = GetFormat;
    mLoggedFormat = false;
    mLoggedCpuCopy = false;
    mHwRefused = false;
    mHwErrors = 0;
    mInSend = false;
    mFormatInSend = false;
    mFormatDecided = false;
    mReopenReason = nullptr;
    mPrerollOverflow = false;
    mPrerollBytes = 0;
    mPreroll.clear();
    if (hw) {
        // hwaccel: one thread, exact profile match (no ALLOW_PROFILE_MISMATCH).
        mCtx->thread_count = 1;
        mCtx->extra_hw_frames = kExtraHwFrames;
        mCtx->hwaccel_flags &= ~AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    } else {
        mCtx->thread_count = base::GetIntProperty("debug.ffmpeg_codec2.threads", 0);
        if (mCodecID == AV_CODEC_ID_MPEG2VIDEO) {
            // Frame threads would add more delay than MPEG-2's fixed output delay (3).
            mCtx->thread_type = FF_THREAD_SLICE;
        }
    }

    int err = avcodec_open2(mCtx, codec, NULL);
    if (err < 0) {
        ALOGE("openCodec: ffmpeg video decoder failed to initialize%s. (%s)",
              hw ? " with VA-API" : "", av_err2str(err));
        closeCodec();
        if (hw && wt20::SwFallbackEnabled()) {
            return openCodec(false);
        }
        return C2_NO_INIT;
    }

    ALOGD("openCodec: open ffmpeg video decoder(%s) profile(%s) success, threads = %d, hw = %s",
          avcodec_get_name(mCtx->codec_id),
          avcodec_profile_name(mCtx->codec_id, mCtx->profile),
          mCtx->thread_count, hw ? "VA-API" : "no");
    return C2_OK;
}

// Frees the decoder context and this instance's VA device. The imported surfaces and the VPP
// context go first: they live on the device's VADisplay.
void C2FFMPEGVideoDecodeComponent::closeCodec() {
    if (mFrame) {
        av_frame_unref(mFrame);
    }
    if (mSwFrame) {
        av_frame_unref(mSwFrame);
    }
    mCopier.reset();
    if (mCtx) {
        avcodec_free_context(&mCtx);
    }
    av_buffer_unref(&mHwDevice);
    if (mHwSession) {
        wt20::ReleaseHwSession();
        mHwSession = false;
    }
    mCodecAlreadyOpened = false;
}

c2_status_t C2FFMPEGVideoDecodeComponent::openDecoder() {
    if (mCodecAlreadyOpened) {
        return C2_OK;
    }

    mExtradataReady = true;

    bool allowHw = !mForceSoftware && wt20::HwDecodeEnabled() && wt20::VaSupportsCodec(mCodecID);
    c2_status_t err = openCodec(allowHw);
    if (err != C2_OK) {
        return err;
    }
    mCodecAlreadyOpened = true;

    if (!mFrame) {
        mFrame = av_frame_alloc();
    }
    if (!mSwFrame) {
        mSwFrame = av_frame_alloc();
    }
    if (!mFrame || !mSwFrame) {
        ALOGE("openDecoder: oom for video frame");
        return C2_NO_MEMORY;
    }

    return C2_OK;
}

// Reopens this component's decoder without VA-API (and so with FFmpeg's threads), keeping the
// codec-specific data.
c2_status_t C2FFMPEGVideoDecodeComponent::reopenSoftware(const char* reason) {
    ALOGW("reopening %s without VA-API: %s", avcodec_get_name(mCodecID), reason);
    closeCodec();
    mForceSoftware = true;
    mSoftwareReason = reason;
    c2_status_t err = initDecoder();
    if (err == C2_OK) {
        err = openCodec(false);
    }
    if (err != C2_OK) {
        ALOGE("reopenSoftware: software decoder failed to open, err = %d", err);
        return err;
    }
    mCodecAlreadyOpened = true;
    return C2_OK;
}

// Too many VA errors: continue this stream in software, in the same component.
c2_status_t C2FFMPEGVideoDecodeComponent::fallbackToSoftware(const char* reason) {
    if (!wt20::SwFallbackEnabled()) {
        ALOGE("fallbackToSoftware: %s, and persist.vendor.wt20.hwdec.swfallback=false", reason);
        return C2_CORRUPTED;
    }
    return reopenSoftware(reason);
}

// Keeps a copy of a packet sent to a VA instance before its first get_format.
void C2FFMPEGVideoDecodeComponent::keepPreroll(const uint8_t* data, size_t size,
                                               int64_t timestamp) {
    if (mPrerollOverflow) {
        return;
    }
    if (mPreroll.size() >= kMaxPrerollPackets || mPrerollBytes + size > kMaxPrerollBytes) {
        ALOGW("keepPreroll: no picture after %zu packets / %zu bytes; a later software "
              "decision stays single-threaded", mPreroll.size(), mPrerollBytes);
        mPreroll.clear();
        mPrerollBytes = 0;
        mPrerollOverflow = true;
        return;
    }
    mPreroll.emplace_back(std::vector<uint8_t>(data, data + size), timestamp);
    mPrerollBytes += size;
}

// Sends the kept packets to the freshly opened software decoder. No frame exists before the
// first get_format, so the packets before the last one only carry parameter sets and the like
// and cannot make the decoder ask for its output first (EAGAIN).
int C2FFMPEGVideoDecodeComponent::replayPreroll(PacketQueue* packets) {
    int err = 0;
    for (auto& p : *packets) {
        mPacket->data = p.first.data();
        mPacket->size = p.first.size();
        mPacket->pts = p.second;
        mPacket->dts = AV_NOPTS_VALUE;
        err = avcodec_send_packet(mCtx, mPacket);
        av_packet_unref(mPacket);
        if (err == AVERROR(EAGAIN)) {
            ALOGW("replayPreroll: decoder wants output after %zu bytes; rest dropped",
                  p.first.size());
            break;
        }
        if (err < 0) {
            ALOGW("replayPreroll: %s", av_err2str(err));
        }
    }
    packets->clear();
    return err;
}

void C2FFMPEGVideoDecodeComponent::deInitDecoder() {
    ALOGD("%p deInitDecoder: %p", this, mCtx);
    closeCodec();
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mSwFrame) {
        av_frame_free(&mSwFrame);
        mSwFrame = NULL;
    }
    if (mPacket) {
        av_packet_free(&mPacket);
        mPacket = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
    mEOSSignalled = false;
    mExtradataReady = false;
    mExtradata.clear();
    mForceSoftware = false;
    mSoftwareReason = nullptr;
    mSwitchToSoftware = false;
    mHwRefused = false;
    mHwErrors = 0;
    mReopenReason = nullptr;
    mPreroll.clear();
    mPrerollBytes = 0;
    mPendingWorkQueue.clear();
}

c2_status_t C2FFMPEGVideoDecodeComponent::processCodecConfig(C2ReadView* inBuffer) {
#if DEBUG_EXTRADATA
    ALOGD("processCodecConfig: add = %u, current = %zu", inBuffer->capacity(), mExtradata.size());
#endif
    if (! mExtradataReady) {
        mExtradata.insert(mExtradata.end(), inBuffer->data(),
                          inBuffer->data() + inBuffer->capacity());
    }
    else {
        ALOGW("processCodecConfig: decoder is already opened, ignoring...");
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::noteDecodeError(int err, const char* where) {
    if (!mCtx || !mCtx->hw_device_ctx || !IsVaError(err)) {
        return;
    }
    mHwErrors++;
    ALOGW("%s: VA error %d of %d: %s", where, mHwErrors, kMaxHwErrors, av_err2str(err));
    if (mHwErrors >= kMaxHwErrors) {
        mSwitchToSoftware = true;
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::sendInputBuffer(
        C2ReadView *inBuffer, int64_t timestamp) {
    if (!mPacket) {
        mPacket = av_packet_alloc();
        if (!mPacket) {
            ALOGE("sendInputBuffer: oom for video packet");
            return C2_NO_MEMORY;
        }
    }

    const int size = inBuffer ? inBuffer->capacity() : 0;
    uint8_t* const data = inBuffer ? const_cast<uint8_t *>(inBuffer->data()) : NULL;

    // A VA instance runs FFmpeg with one thread. Until its first get_format (which runs inside
    // avcodec_send_packet: the decoder works synchronously with one thread) keep the packets,
    // so that a stream the hardware cannot take can be decoded again from the start by a
    // threaded software decoder instead of a single-threaded one.
    const bool keep = data && size > 0 && mCtx->hw_device_ctx && !mFormatDecided;
    if (keep) {
        keepPreroll(data, size, timestamp);
    }

    mPacket->data = data;
    mPacket->size = size;
    mPacket->pts = timestamp;
    mPacket->dts = AV_NOPTS_VALUE;

    mInSend = true;
    mFormatInSend = false;
    int err = avcodec_send_packet(mCtx, mPacket);
    mInSend = false;
    av_packet_unref(mPacket);

    if (keep && err == AVERROR(EAGAIN) && !mFormatInSend && !mPreroll.empty()) {
        // Not taken: it is sent (and kept) again after the frames are read.
        mPrerollBytes -= mPreroll.back().first.size();
        mPreroll.pop_back();
    }

    if (mFormatInSend && !mFormatDecided) {
        mFormatDecided = true;
        PacketQueue preroll;
        preroll.swap(mPreroll);
        mPrerollBytes = 0;
        const char* reopen = mReopenReason;
        mReopenReason = nullptr;
        if (reopen && keep && !preroll.empty()) {
            c2_status_t r = reopenSoftware(reopen);
            if (r != C2_OK) {
                return r;
            }
            preroll.pop_back();  // this packet: sent below, with the usual EAGAIN handling
            replayPreroll(&preroll);
            mPacket->data = data;
            mPacket->size = size;
            mPacket->pts = timestamp;
            mPacket->dts = AV_NOPTS_VALUE;
            err = avcodec_send_packet(mCtx, mPacket);
            av_packet_unref(mPacket);
        }
    }

    if (err == AVERROR(EAGAIN)) {
        // Frames must be read first, notify main decoding loop.
        return C2_BAD_STATE;
    }
    if (err < 0 && err != AVERROR_EOF) {
        ALOGE("sendInputBuffer: failed to send data (%d) to decoder: %s (%08x)",
              size, av_err2str(err), err);
        noteDecodeError(err, "send_packet");
        if (mHwRefused) {
            return C2_CORRUPTED;
        }
        // Otherwise don't send error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::receiveFrame(bool* hasPicture) {
    int err = avcodec_receive_frame(mCtx, mFrame);

    *hasPicture = false;
    if (err == 0) {
        *hasPicture = true;
    } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
        ALOGE("receiveFrame: failed to receive frame from decoder err = %d (%s)",
              err, av_err2str(err));
        noteDecodeError(err, "receive_frame");
        if (mHwRefused) {
            return C2_CORRUPTED;
        }
        // Don't report error to client.
    }

    return C2_OK;
}

// CPU path: software frames, and VA frames whose buffer cannot be imported into VA.
c2_status_t C2FFMPEGVideoDecodeComponent::copyToGraphicView(
        const AVFrame* frame, C2GraphicView* outBuffer) {
    const C2PlanarLayout layout = outBuffer->layout();
    uint8_t* const* planes = outBuffer->data();
    if (layout.type != C2PlanarLayout::TYPE_YUV || layout.numPlanes < 3 || !planes) {
        ALOGE("copyToGraphicView: not a YUV buffer (type %u, %u planes)",
              layout.type, layout.numPlanes);
        return C2_CORRUPTED;
    }
    const C2PlaneInfo& y = layout.planes[C2PlanarLayout::PLANE_Y];
    const C2PlaneInfo& u = layout.planes[C2PlanarLayout::PLANE_U];
    const C2PlaneInfo& v = layout.planes[C2PlanarLayout::PLANE_V];
    uint8_t* yData = planes[C2PlanarLayout::PLANE_Y];
    uint8_t* uData = planes[C2PlanarLayout::PLANE_U];
    uint8_t* vData = planes[C2PlanarLayout::PLANE_V];

    if (y.colInc != 1 || y.rowInc <= 0 || u.rowInc <= 0 || v.rowInc <= 0 ||
        y.allocatedDepth != 8 || u.allocatedDepth != 8 || v.allocatedDepth != 8 ||
        u.colSampling != 2 || u.rowSampling != 2 || v.colSampling != 2 || v.rowSampling != 2) {
        ALOGE("copyToGraphicView: unsupported 8-bit 4:2:0 layout");
        return C2_CORRUPTED;
    }

    uint8_t* data[4] = { yData, NULL, NULL, NULL };
    int linesize[4] = { y.rowInc, 0, 0, 0 };
    enum AVPixelFormat outpixfmt;
    if (u.colInc == 1 && v.colInc == 1) {
        outpixfmt = AV_PIX_FMT_YUV420P;
        data[1] = uData;
        data[2] = vData;
        linesize[1] = u.rowInc;
        linesize[2] = v.rowInc;
    } else if (u.colInc == 2 && v.colInc == 2 && u.rowInc == v.rowInc && vData == uData + 1) {
        outpixfmt = AV_PIX_FMT_NV12;
        data[1] = uData;
        linesize[1] = u.rowInc;
    } else if (u.colInc == 2 && v.colInc == 2 && u.rowInc == v.rowInc && uData == vData + 1) {
        outpixfmt = AV_PIX_FMT_NV21;
        data[1] = vData;
        linesize[1] = v.rowInc;
    } else {
        ALOGE("copyToGraphicView: unsupported chroma layout (colInc %d/%d)", u.colInc, v.colInc);
        return C2_CORRUPTED;
    }

    // NV12 into NV12 (the VA-API CPU copy): plain row copies. swscale's same-format path left
    // the CbCr plane of the gralloc buffer unwritten here (all zero: a green picture).
    if (frame->format == AV_PIX_FMT_NV12 && outpixfmt == AV_PIX_FMT_NV12) {
        const int rowBytes = (frame->width + 1) & ~1;
        for (int row = 0; row < frame->height; row++) {
            memcpy(data[0] + (size_t)row * linesize[0],
                   frame->data[0] + (size_t)row * frame->linesize[0], frame->width);
        }
        for (int row = 0; row < (frame->height + 1) / 2; row++) {
            memcpy(data[1] + (size_t)row * linesize[1],
                   frame->data[1] + (size_t)row * frame->linesize[1], rowBytes);
        }
        return C2_OK;
    }

    struct SwsContext* currentImgConvertCtx = mImgConvertCtx;
    mImgConvertCtx = sws_getCachedContext(currentImgConvertCtx,
           frame->width, frame->height, (AVPixelFormat)frame->format,
           frame->width, frame->height, outpixfmt,
           SWS_POINT, NULL, NULL, NULL);
    if (mImgConvertCtx && mImgConvertCtx != currentImgConvertCtx) {
        ALOGD("copyToGraphicView: created video converter - %s => %s",
              av_get_pix_fmt_name((AVPixelFormat)frame->format), av_get_pix_fmt_name(outpixfmt));
    } else if (! mImgConvertCtx) {
        ALOGE("copyToGraphicView: cannot initialize the conversion context");
        return C2_NO_MEMORY;
    }

    sws_scale(mImgConvertCtx, frame->data, frame->linesize,
              0, frame->height, data, linesize);

    return C2_OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    work->worklets.front()->output.flags =
        (C2FrameData::flags_t)(work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
    work->result = C2_OK;
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: drop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
}

static bool comparePendingWork(const PendingWork& w1, const PendingWork& w2) {
    return w1.second < w2.second;
}

void C2FFMPEGVideoDecodeComponent::pushPendingWork(const std::unique_ptr<C2Work>& work) {
    uint32_t outputDelay = mIntf->getOutputDelay();

    if (mPendingWorkQueue.size() >= outputDelay) {
        uint32_t newOutputDelay = outputDelay;
        std::vector<std::unique_ptr<C2Param>> configUpdate;

        switch (mCodecID) {
            case AV_CODEC_ID_H264:
                // Increase output delay step-wise.
                if (outputDelay >= 18u) {
                    newOutputDelay = 34u;
                } else if (outputDelay >= 8u) {
                    newOutputDelay = 18u;
                } else {
                    newOutputDelay = 8u;
                }
                break;
            default:
                // Other codecs use constant output delay.
                break;
        }

        if (newOutputDelay != outputDelay) {
            C2PortActualDelayTuning::output delay(newOutputDelay);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            int err;

            err = mIntf->config({ &delay }, C2_MAY_BLOCK, &failures);
            if (err == C2_OK) {
                ALOGD("WorkQueue: queue full, output delay set to %u", newOutputDelay);
                configUpdate.push_back(C2Param::Copy(delay));
            } else {
                ALOGE("WorkQueue: output delay update to %u failed err = %d",
                      newOutputDelay, err);
            }
        }

        auto fillEmptyWorkWithConfigUpdate = [&configUpdate](const std::unique_ptr<C2Work>& work) {
            fillEmptyWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
        };

        finish(mPendingWorkQueue.front().first, fillEmptyWorkWithConfigUpdate);
        mPendingWorkQueue.pop_front();
    }
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: push idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    mPendingWorkQueue.push_back(PendingWork(work->input.ordinal.frameIndex.peeku(),
                                            work->input.ordinal.timestamp.peeku()));
    std::sort(mPendingWorkQueue.begin(), mPendingWorkQueue.end(), comparePendingWork);
}

void C2FFMPEGVideoDecodeComponent::popPendingWork(const std::unique_ptr<C2Work>& work) {
    uint64_t index = work->input.ordinal.frameIndex.peeku();
    auto it = std::find_if(mPendingWorkQueue.begin(), mPendingWorkQueue.end(),
                           [index](const PendingWork& pWork) { return index == pWork.first; });

#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: pop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif

    if (it != mPendingWorkQueue.end()) {
        mPendingWorkQueue.erase(it);
    }
#if DEBUG_WORKQUEUE
    else {
        ALOGD("WorkQueue: pop work not found idx=%" PRIu64 ", ts=%" PRIu64,
              work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
    }
#endif
    prunePendingWorksUntil(work);
}

void C2FFMPEGVideoDecodeComponent::prunePendingWorksUntil(const std::unique_ptr<C2Work>& work) {
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: prune until idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    // Drop all works with a PTS earlier than provided argument.
    while (mPendingWorkQueue.size() > 0 &&
           mPendingWorkQueue.front().second < work->input.ordinal.timestamp.peeku()) {
        finish(mPendingWorkQueue.front().first, fillEmptyWork);
        mPendingWorkQueue.pop_front();
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onInit() {
    ALOGD("onInit");
    return initDecoder();
}

c2_status_t C2FFMPEGVideoDecodeComponent::onStop() {
    ALOGD("onStop");
    // SimpleC2Component::start() after stop() does not call onInit() again: leave a clean,
    // unopened decoder (and give the GPU session back).
    deInitDecoder();
    return initDecoder();
}

void C2FFMPEGVideoDecodeComponent::onReset() {
    ALOGD("onReset");
    deInitDecoder();
    initDecoder();
}

void C2FFMPEGVideoDecodeComponent::onRelease() {
    ALOGD("onRelease");
    deInitDecoder();
    if (mFFMPEGInitialized) {
        deInitFFmpeg();
        mFFMPEGInitialized = false;
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onFlush_sm() {
    ALOGD("onFlush_sm");
    if (mCtx && avcodec_is_open(mCtx)) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
        mEOSSignalled = false;
    }
    // SimpleC2Component hands the pending works back as flushed: forget them, or they would
    // count against the output delay after a seek.
    mPendingWorkQueue.clear();
    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::outputFrame(
    const std::unique_ptr<C2Work>& work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    c2_status_t err;
    std::vector<std::unique_ptr<C2Param>> configUpdate;

#if DEBUG_FRAMES
    ALOGD("outputFrame: pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - %d x %d (%x)",
          mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp, mFrame->width, mFrame->height, mFrame->format);
#endif

    // FFmpeg applies only the right/bottom crop to hwaccel frames (av_frame_apply_cropping):
    // the picture is [crop_left, width) x [crop_top, height) of the VA surface. Software
    // frames come cropped.
    const bool hwFrame = mFrame->format == AV_PIX_FMT_VAAPI;
    uint32_t cropLeft = 0, cropTop = 0;
    if (hwFrame && mFrame->crop_left < (size_t)mFrame->width &&
        mFrame->crop_top < (size_t)mFrame->height) {
        cropLeft = mFrame->crop_left;
        cropTop = mFrame->crop_top;
    }
    const uint32_t width = mFrame->width - cropLeft;
    const uint32_t height = mFrame->height - cropTop;

    if (width != mIntf->getWidth() || height != mIntf->getHeight()) {
        ALOGD("outputFrame: video params changed - %u x %u (%x)", width, height, mFrame->format);

        C2StreamPictureSizeInfo::output size(0u, width, height);
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        err = mIntf->config({ &size }, C2_MAY_BLOCK, &failures);
        if (err == OK) {
            configUpdate.push_back(C2Param::Copy(size));
        } else {
            ALOGE("outputFrame: config update failed err = %d", err);
            return C2_CORRUPTED;
        }
    }

    // Same format and usage for hardware and software frames: NV12 from minigbm, written by
    // the GPU (VPP) or by the CPU.
    const uint32_t allocWidth = AlignUp(width, kOutputWidthAlign);
    const uint32_t allocHeight = AlignUp(height, kOutputHeightAlign);
    const C2MemoryUsage memoryUsage(static_cast<uint64_t>(C2MemoryUsage::CPU_READ) |
                                    static_cast<uint64_t>(C2MemoryUsage::CPU_WRITE) |
                                    static_cast<uint64_t>(BufferUsage::VIDEO_DECODER));
    std::shared_ptr<C2GraphicBlock> block;

    err = pool->fetchGraphicBlock(allocWidth, allocHeight, HAL_PIXEL_FORMAT_YCBCR_420_888,
                                  memoryUsage, &block);

    if (err != C2_OK) {
        ALOGE("outputFrame: failed to fetch graphic block %u x %u (%x) err = %d",
              allocWidth, allocHeight, HAL_PIXEL_FORMAT_YCBCR_420_888, err);
        return C2_CORRUPTED;
    }

    c2_status_t copied = C2_OMITTED;
    if (hwFrame) {
        // One GPU copy from FFmpeg's VA surface into the gralloc buffer.
        const AVHWFramesContext* frames =
                reinterpret_cast<const AVHWFramesContext*>(mFrame->hw_frames_ctx->data);
        const AVVAAPIDeviceContext* va =
                static_cast<const AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
        VARectangle srcRect;
        srcRect.x = (int16_t)cropLeft;
        srcRect.y = (int16_t)cropTop;
        srcRect.width = (uint16_t)width;
        srcRect.height = (uint16_t)height;
        copied = mCopier.copy(va->display, (VASurfaceID)(uintptr_t)mFrame->data[3], srcRect,
                              *block);
        if (copied == C2_OK) {
            mHwErrors = 0;
        }
        // C2_CORRUPTED is a VPP fault, not a decode error: this frame takes the CPU copy
        // below, and the copier switches itself to CPU copies after a few in a row. VA
        // decode goes on either way.
    }

    if (copied != C2_OK) {
        const AVFrame* src = mFrame;
        if (hwFrame) {
            // CPU fallback: download the VA surface as NV12.
            av_frame_unref(mSwFrame);
            mSwFrame->format = AV_PIX_FMT_NV12;
            int ret = av_hwframe_transfer_data(mSwFrame, mFrame, 0);
            if (ret >= 0) {
                ret = av_frame_copy_props(mSwFrame, mFrame);
            }
            if (ret >= 0) {
                mSwFrame->crop_left = cropLeft;
                mSwFrame->crop_top = cropTop;
                mSwFrame->crop_right = 0;
                mSwFrame->crop_bottom = 0;
                ret = av_frame_apply_cropping(mSwFrame, AV_FRAME_CROP_UNALIGNED);
            }
            if (ret < 0) {
                ALOGE("outputFrame: cannot download the VA frame: %s; frame dropped",
                      av_err2str(ret));
                av_frame_unref(mSwFrame);
                noteDecodeError(AVERROR(EIO), "av_hwframe_transfer_data");
                return C2_OK;
            }
            mHwErrors = 0;  // the VA surface was decoded and read back fine
            if (!mLoggedCpuCopy) {
                ALOGI("outputFrame: VA frames copied by the CPU (%ux%u)", width, height);
                mLoggedCpuCopy = true;
            }
            src = mSwFrame;
        }

        C2GraphicView wView = block->map().get();

        err = wView.error();
        if (err != C2_OK) {
            ALOGE("outputFrame: graphic view map failed err = %d", err);
            return C2_CORRUPTED;
        }

        err = copyToGraphicView(src, &wView);
        if (hwFrame) {
            av_frame_unref(mSwFrame);
        }
        if (err != C2_OK) {
            return err;
        }
    }

    std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(std::move(block), C2Rect(width, height));

    buffer->setInfo(mIntf->getPixelFormatInfo());

    if (work && c2_cntr64_t(mFrame->best_effort_timestamp) == work->input.ordinal.frameIndex) {
        prunePendingWorksUntil(work);
        work->worklets.front()->output.configUpdate = std::move(configUpdate);
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
        work->result = C2_OK;
    } else {
        auto fillWork = [buffer, &configUpdate, this](const std::unique_ptr<C2Work>& work) {
            popPendingWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
            work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.buffers.push_back(buffer);
            work->worklets.front()->output.ordinal = work->input.ordinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
#if DEBUG_FRAMES
            ALOGD("outputFrame: work(finish) idx=%" PRIu64 ", processed=%u, result=%d",
                  work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
        };

        finish(mFrame->best_effort_timestamp, fillWork);
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::process(
    const std::unique_ptr<C2Work> &work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    size_t inSize = 0u;
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    C2ReadView rView = mDummyReadView;
    bool hasInputBuffer = false;

    if (! work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        hasInputBuffer = true;
    }

#if DEBUG_FRAMES
    ALOGD("process: input flags=%08x ts=%lu idx=%lu #buf=%lu[%lu] #conf=%lu #info=%lu",
          work->input.flags, work->input.ordinal.timestamp.peeku(), work->input.ordinal.frameIndex.peeku(),
          work->input.buffers.size(), inSize, work->input.configUpdate.size(), work->input.infoBuffers.size());
#endif

    if (mEOSSignalled) {
        ALOGE("process: ignoring work while EOS reached");
        work->workletsProcessed = 0u;
        work->result = C2_BAD_VALUE;
        return;
    }

    if (hasInputBuffer && rView.error()) {
        ALOGE("process: read view map failed err = %d", rView.error());
        work->workletsProcessed = 0u;
        work->result = rView.error();
        return;
    }

    // In all cases the work is marked as completed.
    //
    // There is not always a 1:1 mapping between input and output frames, in particular for
    // interlaced content. Keeping the corresponding worklets in the queue quickly fills it
    // in and stalls the decoder. But there's no obvious mechanism to determine, from
    // FFMPEG API, whether a given packet will produce an output frame and the worklet should
    // be kept around so it can be completed when the frame is produced.
    //
    // NOTE: This has an impact on the drain operation.

    work->result = C2_OK;
    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->workletsProcessed = 0u;

    if (inSize || (eos && mCodecAlreadyOpened)) {
        c2_status_t err = C2_OK;

        if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
            work->workletsProcessed = 1u;
            work->result = processCodecConfig(&rView);
            return;
        }

        if (! mCodecAlreadyOpened) {
            err = openDecoder();
            if (err != C2_OK) {
                work->workletsProcessed = 1u;
                work->result = err;
                return;
            }
        }

        bool inputConsumed = false;
        bool outputAvailable = true;
        bool hasPicture = false;
#if DEBUG_FRAMES
        int outputFrameCount = 0;
#endif

        while (!inputConsumed || outputAvailable) {
            if (!inputConsumed) {
                err = sendInputBuffer(&rView, work->input.ordinal.frameIndex.peekll());
                if (err == C2_OK) {
                    inputConsumed = true;
                    outputAvailable = true;
                    work->input.buffers.clear();
                } else if (err != C2_BAD_STATE) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }
            }

            if (outputAvailable) {
                hasPicture = false;
                err = receiveFrame(&hasPicture);
                if (err != C2_OK) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }

                if (hasPicture) {
                    err = outputFrame(work, pool);
                    if (err != C2_OK) {
                        work->workletsProcessed = 1u;
                        work->result = err;
                        return;
                    }
#if DEBUG_FRAMES
                    else {
                        outputFrameCount++;
                    }
#endif
                }
                else {
#if DEBUG_FRAMES
                    if (!outputFrameCount) {
                        ALOGD("process: no frame");
                    }
#endif
                    outputAvailable = false;
                }
            }
        }

        if (mSwitchToSoftware) {
            mSwitchToSoftware = false;
            err = fallbackToSoftware("too many consecutive VA errors");
            if (err != C2_OK) {
                work->workletsProcessed = 1u;
                work->result = C2_CORRUPTED;
                return;
            }
        }
    }
#if DEBUG_FRAMES
    else {
        ALOGD("process: empty work");
    }
#endif

    if (eos) {
        mEOSSignalled = true;
        work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        work->workletsProcessed = 1u;
    }

    if (work->workletsProcessed == 0u) {
        pushPendingWork(work);
    }

#if DEBUG_FRAMES
    ALOGD("process: work(end) idx=%" PRIu64 ", processed=%u, result=%d",
          work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
}

c2_status_t C2FFMPEGVideoDecodeComponent::drain(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool>& pool
) {
    ALOGD("drain: mode = %u", drainMode);

    if (drainMode == NO_DRAIN) {
        ALOGW("drain: NO_DRAIN is no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("drain: DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }
    if (! mCodecAlreadyOpened) {
        ALOGW("drain: codec not opened yet");
        return C2_OK;
    }

    bool hasPicture = false;
    c2_status_t err = C2_OK;

    err = sendInputBuffer(NULL, 0);
    while (err == C2_OK) {
        hasPicture = false;
        err = receiveFrame(&hasPicture);
        if (hasPicture) {
            // Ignore errors at this point, just drain the decoder.
            outputFrame(nullptr, pool);
        } else {
            err = C2_NOT_FOUND;
        }
    }

    return C2_OK;
}

} // namespace android
