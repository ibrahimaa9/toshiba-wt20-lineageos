/*
 * wt20_c2_vaapi_selftest (apply17c; device/pc/wt20/options.mk WT20_HW_VIDEO_DECODE)
 *
 * Proves the hardware decode path of the WT20 Codec2 HAL (external/ffmpeg_codec2) without the
 * framework: FFmpeg's VA-API hwaccel on the HD 5300 (i965), then the HAL's output step, one VPP
 * copy from FFmpeg's VA surface into an NV12 gralloc buffer imported with the legacy DRM_PRIME
 * path (i965 cannot import DRM_PRIME_2). Each stream is decoded three ways and every frame's
 * NV12 picture (visible size, packed rows: Y then interleaved CbCr) is hashed with MD5:
 *   sw   FFmpeg software decoder (yuv420p, interleaved to NV12 here; exact crop, see decode())
 *   hw   VA-API decode, av_hwframe_transfer_data to NV12 (the HAL's CPU fallback path)
 *   vpp  VA-API decode, VPP copy into an AHardwareBuffer (YCbCr_420_888 = NV12 on minigbm),
 *        read back with AHardwareBuffer_lockPlanes (the HAL's normal path). The buffers are
 *        allocated as the HAL's outputFrame() fetches them: width / height aligned to
 *        kHalAlignW / kHalAlignH (apply17c reads both from C2FFMPEGVideoDecodeComponent.cpp),
 *        usage CPU_READ | CPU_WRITE | VIDEO_DECODER; the VA surface is the visible size.
 * Pass criteria:
 *   H.264, VP8: decoding is bit exact by spec, so sw, hw and vpp must agree on every frame,
 *     and with the reference MD5s next to the stream (<stream>.md5: ffmpeg -pix_fmt nv12
 *     -f framehash -hash md5, one per line, system/cros-codecs test_data) when present.
 *   MPEG-2: not bit exact. The standard only bounds the IDCT error (IEEE 1180); FFmpeg's
 *     software path uses the C simple IDCT (no x86asm), i965 the MFX hardware IDCT. hw and vpp
 *     come from the same VA surface and must agree bit for bit; sw (and a reference, which is
 *     a software decode) is compared with hw by PSNR over the NV12 picture: every frame at
 *     least kMpeg2MinPsnr dB, and the same number of frames. The sw MD5s are printed for
 *     information only.
 *
 * Usage: wt20_c2_vaapi_selftest [-sw|-hw|-vpp] [-q] [-n frames] [-ref file.md5] [stream...]
 *   no stream: every vector in /vendor/etc/wt20_hwdec (installed by wt20.mk)
 *   -sw/-hw/-vpp: run only that path and print its MD5s (compare two runs by hand)
 *   -q: print only mismatches and the summary
 * Streams: IVF (DKIF, VP8), H.264 Annex B (.h264/.264/.avc), MPEG-2 video ES (.m2v/.mpg/.mpeg2)
 * Exit: 0 all checks pass, 1 a mismatch, 2 setup or decode error.
 * Runs as root from adb shell; sets LIBVA_DRIVER_NAME=i965 and LIBVA_DRIVERS_PATH=/vendor/lib64
 * unless already set.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <android/hardware_buffer.h>
#include <cros_gralloc/cros_gralloc_handle.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <vndk/hardware_buffer.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/md5.h>
#include <libavutil/pixdesc.h>
}

namespace {

constexpr const char* kRenderNode = "/dev/dri/renderD128";
constexpr const char* kVectorDir = "/vendor/etc/wt20_hwdec";
// minigbm's handle magic (external/minigbm-upstream/cros_gralloc/cros_gralloc_helpers.h)
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;
// android.hardware.graphics.common BufferUsage::VIDEO_DECODER (no AHARDWAREBUFFER_USAGE_ name)
constexpr uint64_t kUsageVideoDecoder = 1ULL << 22;
constexpr int kTargets = 3;  // gralloc buffers in rotation, like a small BufferQueue
// The HAL's output block alignment (C2FFMPEGVideoDecodeComponent.cpp outputFrame(), filled in
// by apply17c): the gralloc buffer is AlignUp(w, kHalAlignW) x AlignUp(h, kHalAlignH), the
// picture its visible top-left w x h
constexpr uint32_t kHalAlignW = 64;
constexpr uint32_t kHalAlignH = 16;
// MPEG-2 sw against hw: two IEEE 1180 IDCTs differ by +-1 on a few pixels and drift a little
// within a GOP (FFmpeg's simple IDCT against its int / faani ones on the installed MPEG-2
// vectors: 57 dB on the worst frame); a broken decode (wrong reference, lost slice, chroma
// swap) lands far below this
constexpr double kMpeg2MinPsnr = 40.0;
// Software pictures kept for the MPEG-2 PSNR check; frames beyond it only count
constexpr size_t kKeepBytes = 128u << 20;

bool gQuiet = false;

void fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

std::string averr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define VA_OK(expr)                                                              \
    ({                                                                           \
        VAStatus st_ = (expr);                                                   \
        if (st_ != VA_STATUS_SUCCESS) fail("%s: %s", #expr, vaErrorStr(st_));    \
        st_ == VA_STATUS_SUCCESS;                                                \
    })

// MD5 over an NV12 picture in the layout of ffmpeg's framehash (av_image_copy_to_buffer,
// align 1): w bytes x h rows of Y, then 2*ceil(w/2) bytes x ceil(h/2) rows of CbCr
class Nv12Md5 {
  public:
    Nv12Md5() : mCtx(av_md5_alloc()) { av_md5_init(mCtx); }
    ~Nv12Md5() { av_free(mCtx); }
    void rows(const uint8_t* p, ptrdiff_t stride, int bytes, int count) {
        for (int i = 0; i < count; i++) av_md5_update(mCtx, p + i * stride, bytes);
    }
    void row(const uint8_t* p, size_t bytes) { av_md5_update(mCtx, p, bytes); }
    std::string hex() {
        uint8_t d[16];
        char s[33];
        av_md5_final(mCtx, d);
        for (int i = 0; i < 16; i++) snprintf(s + 2 * i, 3, "%02x", d[i]);
        return s;
    }

  private:
    AVMD5* mCtx;
};

int chromaW(int w) { return (w + 1) / 2; }
int chromaH(int h) { return (h + 1) / 2; }
uint32_t alignUp(int v, uint32_t a) { return ((uint32_t)v + a - 1) / a * a; }

// ---------------------------------------------------------------------------------------------
// Input: the whole stream split into packets once, so every path sees the same packets

struct Stream {
    std::string path;
    AVCodecID codec = AV_CODEC_ID_NONE;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<std::string> ref;  // reference MD5s, may be empty
};

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    fclose(f);
    return true;
}

bool readRef(const std::string& path, std::vector<std::string>* out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.size() == 32) out->push_back(s);
    }
    fclose(f);
    return true;
}

bool loadStream(const std::string& path, const std::string& refPath, Stream* s) {
    std::vector<uint8_t> data;
    s->path = path;
    if (!readFile(path, &data) || data.empty()) {
        fail("cannot read %s", path.c_str());
        return false;
    }
    if (data.size() >= 32 && memcmp(data.data(), "DKIF", 4) == 0) {
        if (memcmp(data.data() + 8, "VP80", 4) != 0) {
            fail("%s: IVF fourcc %.4s, only VP80 is supported", path.c_str(),
                 (const char*)data.data() + 8);
            return false;
        }
        s->codec = AV_CODEC_ID_VP8;
        size_t pos = data[6] | data[7] << 8;  // header length
        while (pos + 12 <= data.size()) {
            uint32_t size = le32(&data[pos]);
            pos += 12;
            if (pos + size > data.size()) break;
            s->packets.emplace_back(data.begin() + pos, data.begin() + pos + size);
            pos += size;
        }
    } else {
        if (endsWith(path, ".h264") || endsWith(path, ".264") || endsWith(path, ".avc")) {
            s->codec = AV_CODEC_ID_H264;
        } else if (endsWith(path, ".m2v") || endsWith(path, ".mpg") || endsWith(path, ".mpeg2")) {
            s->codec = AV_CODEC_ID_MPEG2VIDEO;
        } else {
            fail("%s: unknown stream type (IVF, .h264, .m2v)", path.c_str());
            return false;
        }
        const AVCodec* dec = avcodec_find_decoder(s->codec);
        AVCodecParserContext* pc = av_parser_init(s->codec);
        AVCodecContext* ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
        if (!pc || !ctx) {
            fail("%s: no FFmpeg parser/decoder for %s", path.c_str(), avcodec_get_name(s->codec));
            av_parser_close(pc);
            avcodec_free_context(&ctx);
            return false;
        }
        // av_parser_parse2() may read up to AV_INPUT_BUFFER_PADDING_SIZE bytes past buf_size
        // (the h264 / mpegvideo start-code scanners do), and the last chunk ends at the end of
        // the file: zero-pad the buffer, parse only the real bytes
        const size_t n = data.size();
        data.resize(n + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        const uint8_t* p = data.data();
        size_t left = n;
        for (;;) {
            uint8_t* out = nullptr;
            int outSize = 0;
            int chunk = left > 4096 ? 4096 : (int)left;
            // once all bytes are consumed, NULL/0 calls flush the parser's last frame; stop only
            // when such a flush call returns nothing (not when the last real chunk does)
            const bool flushing = left == 0;
            int used = av_parser_parse2(pc, ctx, &out, &outSize, flushing ? nullptr : p, chunk,
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) break;
            p += used;
            left -= used;
            if (outSize > 0) s->packets.emplace_back(out, out + outSize);
            if (flushing && !outSize) break;  // flushed
        }
        av_parser_close(pc);
        avcodec_free_context(&ctx);
    }
    if (s->packets.empty()) {
        fail("%s: no packets", path.c_str());
        return false;
    }
    std::string r = refPath.empty() ? path + ".md5" : refPath;
    if (!readRef(r, &s->ref) && !refPath.empty()) {
        fail("cannot read %s", r.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// VA-API: one display for FFmpeg and for the VPP copy, opened the way the HAL probes it

struct Va {
    int fd = -1;
    VADisplay dpy = nullptr;
    AVBufferRef* dev = nullptr;
    VAConfigID vppCfg = VA_INVALID_ID;
    VAContextID vppCtx = VA_INVALID_ID;
};

bool hasEntrypoint(VADisplay dpy, VAProfile profile, VAEntrypoint want) {
    int n = vaMaxNumEntrypoints(dpy);
    std::vector<VAEntrypoint> eps(n > 0 ? n : 1);
    if (vaQueryConfigEntrypoints(dpy, profile, eps.data(), &n) != VA_STATUS_SUCCESS) return false;
    for (int i = 0; i < n; i++)
        if (eps[i] == want) return true;
    return false;
}

bool openVa(Va* va, bool needVpp) {
    setenv("LIBVA_DRIVER_NAME", "i965", 0);
    setenv("LIBVA_DRIVERS_PATH", "/vendor/lib64", 0);
    va->fd = open(kRenderNode, O_RDWR | O_CLOEXEC);
    if (va->fd < 0) {
        fail("open %s: %s", kRenderNode, strerror(errno));
        return false;
    }
    va->dpy = vaGetDisplayDRM(va->fd);
    int major = 0, minor = 0;
    if (!va->dpy || !VA_OK(vaInitialize(va->dpy, &major, &minor))) return false;
    printf("VA-API %d.%d: %s\n", major, minor, vaQueryVendorString(va->dpy));
    printf("VLD: H.264 High %s, Main %s, CBP %s | VP8 %s | MPEG-2 Main %s | VideoProc %s\n",
           hasEntrypoint(va->dpy, VAProfileH264High, VAEntrypointVLD) ? "yes" : "NO",
           hasEntrypoint(va->dpy, VAProfileH264Main, VAEntrypointVLD) ? "yes" : "NO",
           hasEntrypoint(va->dpy, VAProfileH264ConstrainedBaseline, VAEntrypointVLD) ? "yes" : "NO",
           hasEntrypoint(va->dpy, VAProfileVP8Version0_3, VAEntrypointVLD) ? "yes" : "NO",
           hasEntrypoint(va->dpy, VAProfileMPEG2Main, VAEntrypointVLD) ? "yes" : "NO",
           hasEntrypoint(va->dpy, VAProfileNone, VAEntrypointVideoProc) ? "yes" : "NO");

    va->dev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    if (!va->dev) {
        fail("av_hwdevice_ctx_alloc(VAAPI) failed: FFmpeg built without VA-API?");
        return false;
    }
    AVHWDeviceContext* devCtx = (AVHWDeviceContext*)va->dev->data;
    ((AVVAAPIDeviceContext*)devCtx->hwctx)->display = va->dpy;
    int err = av_hwdevice_ctx_init(va->dev);
    if (err < 0) {
        fail("av_hwdevice_ctx_init: %s", averr(err).c_str());
        return false;
    }
    if (needVpp) {
        if (!VA_OK(vaCreateConfig(va->dpy, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                  &va->vppCfg)) ||
            !VA_OK(vaCreateContext(va->dpy, va->vppCfg, 0, 0, VA_PROGRESSIVE, nullptr, 0,
                                   &va->vppCtx)))
            return false;
    }
    return true;
}

void closeVa(Va* va) {
    if (va->vppCtx != VA_INVALID_ID) vaDestroyContext(va->dpy, va->vppCtx);
    if (va->vppCfg != VA_INVALID_ID) vaDestroyConfig(va->dpy, va->vppCfg);
    av_buffer_unref(&va->dev);  // does not terminate a display it did not open
    if (va->dpy) vaTerminate(va->dpy);
    if (va->fd >= 0) close(va->fd);
    *va = Va();
}

// ---------------------------------------------------------------------------------------------
// The HAL's output step: NV12 gralloc buffers imported into VA once (cache keyed by the dmabuf
// inode), one VPP copy per frame, then a CPU read through the gralloc mapper

struct Target {
    AHardwareBuffer* ahb = nullptr;
    ino_t ino = 0;
};

class GrallocCopier {
  public:
    explicit GrallocCopier(Va* va) : mVa(va) {}
    ~GrallocCopier() { reset(); }

    // 1 = copied and hashed, 0 = skipped (layout i965 cannot import), -1 = error
    int copy(const AVFrame* f, std::string* md5) {
        // hwaccel frames keep their left/top crop in crop_left/crop_top (av_frame_apply_cropping
        // only removes the right/bottom part from width/height)
        int w = f->width - (int)f->crop_left, h = f->height - (int)f->crop_top;
        if (w != mW || h != mH) {
            reset();
            if (!allocate(w, h)) return -1;
        }
        if (mSkip) return 0;
        Target& t = mTargets[mNext];
        mNext = (mNext + 1) % kTargets;
        auto it = mSurfaces.find(t.ino);
        if (it == mSurfaces.end()) {
            VASurfaceID s;
            int r = import(t.ahb, &s);
            if (r <= 0) return r;
            it = mSurfaces.emplace(t.ino, s).first;
            mImports++;
        }
        VASurfaceID src = (VASurfaceID)(uintptr_t)f->data[3];
        VASurfaceID dst = it->second;
        VARectangle in = {(int16_t)f->crop_left, (int16_t)f->crop_top, (uint16_t)w, (uint16_t)h};
        VARectangle out = {0, 0, (uint16_t)w, (uint16_t)h};
        VAProcPipelineParameterBuffer p;
        memset(&p, 0, sizeof(p));
        p.surface = src;
        p.surface_region = &in;
        p.output_region = &out;
        p.output_background_color = 0xff000000;
        VABufferID buf = VA_INVALID_ID;
        if (!VA_OK(vaSyncSurface(mVa->dpy, src)) ||
            !VA_OK(vaCreateBuffer(mVa->dpy, mVa->vppCtx, VAProcPipelineParameterBufferType,
                                  sizeof(p), 1, &p, &buf)))
            return -1;
        bool ok = VA_OK(vaBeginPicture(mVa->dpy, mVa->vppCtx, dst)) &&
                  VA_OK(vaRenderPicture(mVa->dpy, mVa->vppCtx, &buf, 1)) &&
                  VA_OK(vaEndPicture(mVa->dpy, mVa->vppCtx));
        vaDestroyBuffer(mVa->dpy, buf);
        if (!ok || !VA_OK(vaSyncSurface(mVa->dpy, dst))) return -1;
        return hash(t.ahb, w, h, md5) ? 1 : -1;
    }

    int imports() const { return mImports; }

  private:
    bool allocate(int w, int h) {
        mW = w;
        mH = h;
        for (Target& t : mTargets) {
            AHardwareBuffer_Desc d = {};
            // the HAL's fetchGraphicBlock(AlignUp(w), AlignUp(h)); the picture is cropped from
            // its top-left, so a 1080-line stream checks the 1088-line buffer the HAL gets
            d.width = alignUp(w, kHalAlignW);
            d.height = alignUp(h, kHalAlignH);
            d.layers = 1;
            d.format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
            // the HAL's usage: CPU_READ | CPU_WRITE | VIDEO_DECODER. A surface consumer adds
            // GPU sampling / composer bits, which do not change gbm_mesa's NV12 layout (spoofed
            // format: linear, pitch ALIGN(width, 32) from VIDEO_DECODER alone)
            d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                      kUsageVideoDecoder;
            int err = AHardwareBuffer_allocate(&d, &t.ahb);
            if (err) {
                fail("AHardwareBuffer_allocate %ux%u NV12 (picture %dx%d): %d", d.width, d.height,
                     w, h, err);
                return false;
            }
            const native_handle_t* nh = AHardwareBuffer_getNativeHandle(t.ahb);
            struct stat st;
            if (!nh || nh->numFds < 1 || fstat(nh->data[0], &st)) {
                fail("gralloc buffer without a dmabuf fd");
                return false;
            }
            t.ino = st.st_ino;
        }
        return true;
    }

    // Reads the layout from minigbm's handle (never computed here) and imports it
    int import(AHardwareBuffer* ahb, VASurfaceID* out) {
        const native_handle_t* nh = AHardwareBuffer_getNativeHandle(ahb);
        size_t words = nh->numFds + nh->numInts;
        if (words * sizeof(int) < sizeof(cros_gralloc_handle) - sizeof(native_handle_t)) {
            fail("gralloc handle too small for a cros_gralloc_handle (%d fds, %d ints)",
                 nh->numFds, nh->numInts);
            return -1;
        }
        const cros_gralloc_handle* h = (const cros_gralloc_handle*)nh;
        if (h->magic != kCrosGrallocMagic) {
            fail("gralloc handle magic 0x%x, not minigbm", h->magic);
            return -1;
        }
        if (mLogLayout) {
            mLogLayout = false;
            printf("  gralloc %ux%u fourcc %.4s planes %u modifier 0x%llx: pitch %u/%u "
                   "offset %u/%u size %llu\n",
                   h->width, h->height, (const char*)&h->format, h->num_planes,
                   (unsigned long long)h->format_modifier, h->strides[0], h->strides[1],
                   h->offsets[0], h->offsets[1], (unsigned long long)h->total_size);
        }
        if (h->format != VA_FOURCC_NV12 || h->num_planes != 2) {
            fail("YCbCr_420_888 came out as %.4s with %u planes, expected NV12 / 2",
                 (const char*)&h->format, h->num_planes);
            return -1;
        }
        struct stat a, b;
        if (fstat(h->fds[0], &a) || fstat(h->fds[1], &b) || a.st_ino != b.st_ino) {
            fail("NV12 planes are in different dmabufs; the legacy import takes one");
            return -1;
        }
        if (h->format_modifier != 0 /* DRM_FORMAT_MOD_LINEAR */) {
            fail("NV12 buffer is not linear (modifier 0x%llx)",
                 (unsigned long long)h->format_modifier);
            return -1;
        }
        // i965_suface_external_memory() on Gen8: pitch % 64 == 0 (min_linear_wpitch),
        // offsets[1] / pitch >= height and % 4 == 0, pitches equal
        uint32_t pitch = h->strides[0];
        if (pitch % 64 || h->strides[1] != pitch || h->offsets[0] != 0 || h->offsets[1] % pitch ||
            h->offsets[1] / pitch < (uint32_t)mH || (h->offsets[1] / pitch) % 4) {
            printf("  vpp: SKIP %dx%d: pitch %u / chroma offset %u cannot be imported by i965 "
                   "(needs pitch %% 64 == 0); the HAL must use its CPU path for this size\n",
                   mW, mH, pitch, h->offsets[1]);
            mSkip = true;
            return 0;
        }
        VASurfaceAttribExternalBuffers ext;
        memset(&ext, 0, sizeof(ext));
        uintptr_t fd = h->fds[0];
        ext.pixel_format = VA_FOURCC_NV12;
        ext.width = mW;
        ext.height = mH;
        // the dmabuf's own size: minigbm's total_size also counts the gralloc metadata region
        // (reserved_region_size), which lives in a separate fd
        off_t end = lseek(h->fds[0], 0, SEEK_END);
        ext.data_size = end > 0 ? (uint32_t)end
                                : (uint32_t)(h->total_size - h->reserved_region_size);
        ext.num_planes = 2;
        ext.pitches[0] = ext.pitches[1] = pitch;
        ext.offsets[0] = h->offsets[0];
        ext.offsets[1] = h->offsets[1];
        ext.buffers = &fd;
        ext.num_buffers = 1;
        VASurfaceAttrib at[3];
        memset(at, 0, sizeof(at));
        at[0].type = VASurfaceAttribMemoryType;
        at[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        at[0].value.type = VAGenericValueTypeInteger;
        at[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
        at[1].type = VASurfaceAttribExternalBufferDescriptor;
        at[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        at[1].value.type = VAGenericValueTypePointer;
        at[1].value.value.p = &ext;
        at[2].type = VASurfaceAttribPixelFormat;
        at[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
        at[2].value.type = VAGenericValueTypeInteger;
        at[2].value.value.i = VA_FOURCC_NV12;
        if (!VA_OK(vaCreateSurfaces(mVa->dpy, VA_RT_FORMAT_YUV420, mW, mH, out, 1, at, 3)))
            return -1;
        return 1;
    }

    bool hash(AHardwareBuffer* ahb, int w, int h, std::string* md5) {
        AHardwareBuffer_Planes pl;
        int err = AHardwareBuffer_lockPlanes(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1,
                                             nullptr, &pl);
        if (err) {
            fail("AHardwareBuffer_lockPlanes: %d", err);
            return false;
        }
        bool ok = true;
        const uint8_t* y = (const uint8_t*)pl.planes[0].data;
        const uint8_t* cb = (const uint8_t*)pl.planes[1].data;
        const uint8_t* cr = (const uint8_t*)pl.planes[2].data;
        if (pl.planeCount != 3 || pl.planes[1].pixelStride != 2 || cr != cb + 1 ||
            pl.planes[0].rowStride != pl.planes[1].rowStride) {
            fail("mapper's planes are not NV12 (count %u, chroma step %u, Cr-Cb %td)",
                 pl.planeCount, pl.planes[1].pixelStride, cr - cb);
            ok = false;
        } else if (mCheckMapper) {
            // the HAL reads the layout from the mapper (getPlaneLayouts) and falls back to the
            // handle: both must describe the same buffer
            mCheckMapper = false;
            const cros_gralloc_handle* hd =
                    (const cros_gralloc_handle*)AHardwareBuffer_getNativeHandle(ahb);
            if (pl.planes[0].rowStride != hd->strides[0] ||
                (uint32_t)(cb - y) != hd->offsets[1] - hd->offsets[0]) {
                fail("mapper layout (pitch %u, chroma at +%td) differs from the handle "
                     "(pitch %u, chroma at +%u)",
                     pl.planes[0].rowStride, cb - y, hd->strides[0], hd->offsets[1]);
                ok = false;
            }
        }
        if (ok) {
            Nv12Md5 m;
            m.rows(y, pl.planes[0].rowStride, w, h);
            m.rows(cb, pl.planes[1].rowStride, 2 * chromaW(w), chromaH(h));
            *md5 = m.hex();
        }
        AHardwareBuffer_unlock(ahb, nullptr);
        return ok;
    }

    void reset() {
        for (auto& s : mSurfaces) vaDestroySurfaces(mVa->dpy, (VASurfaceID*)&s.second, 1);
        mSurfaces.clear();
        for (Target& t : mTargets) {
            if (t.ahb) AHardwareBuffer_release(t.ahb);
            t = Target();
        }
        mW = mH = 0;
        mNext = 0;
        mSkip = false;
    }

    Va* mVa;
    Target mTargets[kTargets];
    std::map<ino_t, VASurfaceID> mSurfaces;
    int mW = 0, mH = 0, mNext = 0, mImports = 0;
    bool mSkip = false, mLogLayout = true, mCheckMapper = true;
};

// ---------------------------------------------------------------------------------------------
// Decoding

enum Path { SW, HW, VPP, NPATHS };
const char* const kPathName[NPATHS] = {"sw", "hw", "vpp"};

using Picture = std::vector<uint8_t>;  // packed NV12 in the framehash layout (see Nv12Md5)

struct Result {
    std::vector<std::string> md5[NPATHS];
    bool ran[NPATHS] = {};
    bool error = false;
    bool vppSkipped = false;
    std::vector<Picture> swPics;  // MPEG-2: the first sw pictures (kKeepBytes), for the PSNR
    std::vector<double> psnr;     // MPEG-2: hw frame i against swPics[i], INFINITY if equal
    int maxDiff = 0;              // MPEG-2: largest |sw - hw| of any byte
};

enum AVPixelFormat getFormatVaapi(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    // the HAL's chooseFormat(): plain H.264 Baseline has no entry in FFmpeg's VA-API profile
    // map, so it is decoded with the High config through ALLOW_PROFILE_MISMATCH; every other
    // profile must match exactly
    if (ctx->codec_id == AV_CODEC_ID_H264 && ctx->profile == AV_PROFILE_H264_BASELINE)
        ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    else
        ctx->hwaccel_flags &= ~AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    fail("%s: the decoder does not offer VA-API for this stream (profile %s, %s)",
         avcodec_get_name(ctx->codec_id), avcodec_profile_name(ctx->codec_id, ctx->profile),
         av_get_pix_fmt_name(ctx->sw_pix_fmt));
    return AV_PIX_FMT_NONE;
}

// Packs the visible picture of a CPU frame as NV12; (x0, y0) is a crop the frame still carries
bool packSoftware(const AVFrame* f, int x0, int y0, Picture* out) {
    const int w = f->width - x0, h = f->height - y0, cw = chromaW(w), ch = chromaH(h);
    if (w <= 0 || h <= 0) {
        fail("frame %dx%d with crop %d,%d", f->width, f->height, x0, y0);
        return false;
    }
    if (f->format != AV_PIX_FMT_NV12 && f->format != AV_PIX_FMT_YUV420P &&
        f->format != AV_PIX_FMT_YUVJ420P) {
        fail("software output %s is not 8-bit 4:2:0", av_get_pix_fmt_name((AVPixelFormat)f->format));
        return false;
    }
    out->resize((size_t)w * h + (size_t)2 * cw * ch);
    uint8_t* d = out->data();
    for (int r = 0; r < h; r++, d += w)
        memcpy(d, f->data[0] + (ptrdiff_t)(y0 + r) * f->linesize[0] + x0, w);
    for (int r = 0; r < ch; r++, d += 2 * cw) {
        if (f->format == AV_PIX_FMT_NV12) {
            memcpy(d, f->data[1] + (ptrdiff_t)(y0 / 2 + r) * f->linesize[1] + x0 / 2 * 2, 2 * cw);
            continue;
        }
        const uint8_t* u = f->data[1] + (ptrdiff_t)(y0 / 2 + r) * f->linesize[1] + x0 / 2;
        const uint8_t* v = f->data[2] + (ptrdiff_t)(y0 / 2 + r) * f->linesize[2] + x0 / 2;
        for (int x = 0; x < cw; x++) {
            d[2 * x] = u[x];
            d[2 * x + 1] = v[x];
        }
    }
    return true;
}

std::string md5Of(const Picture& p) {
    Nv12Md5 m;
    m.row(p.data(), p.size());
    return m.hex();
}

// PSNR of b against a over every byte (Y and CbCr), INFINITY if equal, -1 if the sizes differ
double psnrOf(const Picture& a, const Picture& b, int* maxDiff) {
    if (a.size() != b.size() || a.empty()) return -1;
    uint64_t sse = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int d = abs((int)a[i] - (int)b[i]);
        sse += (uint64_t)(d * d);
        if (d > *maxDiff) *maxDiff = d;
    }
    return sse ? 10.0 * log10(255.0 * 255.0 * (double)a.size() / (double)sse) : INFINITY;
}

// One decode of the stream. hw: VA-API decode, hashes the hw and/or vpp outputs per frame.
// keepSw (software decode): keep the pictures in res->swPics for the hw decode's PSNR.
bool decode(const Stream& s, Va* va, bool hw, bool doHw, bool doVpp, bool keepSw, int maxFrames,
            Result* res) {
    const AVCodec* dec = avcodec_find_decoder(s.codec);
    AVCodecContext* ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    AVFrame* sw = av_frame_alloc();
    GrallocCopier copier(va);
    bool ok = ctx && pkt && f && sw;
    int frames = 0, vaFrames = 0;
    size_t kept = 0;
    bool keepFull = !keepSw;
    double t0 = now();
    if (!ok) fail("out of memory / no decoder for %s", avcodec_get_name(s.codec));
    if (ok) {
        ctx->thread_count = 1;
        if (hw) {
            ctx->hw_device_ctx = av_buffer_ref(va->dev);
            ctx->get_format = getFormatVaapi;
            // the HAL's settings: the level check is the HAL's job, never decode a profile
            // the driver does not list (H.264 Baseline aside: getFormatVaapi)
            ctx->hwaccel_flags &= ~AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
        } else {
            // The exact crop. Without UNALIGNED, av_frame_apply_cropping() rounds crop_left
            // down to keep the plane pointers 32-byte aligned and then zeroes crop_left
            // (libavutil/frame.c), so a left crop would leave extra columns that cannot be
            // told from the picture. Hardware frames keep crop_left / crop_top instead.
            ctx->flags |= AV_CODEC_FLAG_UNALIGNED;
        }
        int err = avcodec_open2(ctx, dec, nullptr);
        if (err < 0) {
            fail("avcodec_open2(%s): %s", dec->name, averr(err).c_str());
            ok = false;
        }
    }
    size_t next = 0;
    bool draining = false;
    while (ok && (maxFrames <= 0 || frames < maxFrames)) {
        int err = avcodec_receive_frame(ctx, f);
        if (err == AVERROR(EAGAIN)) {
            if (next < s.packets.size()) {
                const std::vector<uint8_t>& p = s.packets[next++];
                av_packet_unref(pkt);
                if (av_new_packet(pkt, p.size()) < 0) {
                    ok = false;
                    break;
                }
                memcpy(pkt->data, p.data(), p.size());
                err = avcodec_send_packet(ctx, pkt);
            } else if (!draining) {
                draining = true;
                err = avcodec_send_packet(ctx, nullptr);
            } else {
                break;  // drained (receive_frame should have returned EOF)
            }
            if (err < 0 && err != AVERROR_EOF) {
                fail("decode (%s, packet %zu): %s", hw ? "VA-API" : "software", next,
                     averr(err).c_str());
                ok = false;
            }
            continue;
        }
        if (err == AVERROR_EOF) break;
        if (err < 0) {
            fail("decode (%s, frame %d): %s", hw ? "VA-API" : "software", frames,
                 averr(err).c_str());
            ok = false;
            break;
        }
        Picture pic;
        std::string md5;
        if (!hw) {
            // crop_left / crop_top are 0 here (applied exactly, see above)
            ok = packSoftware(f, (int)f->crop_left, (int)f->crop_top, &pic);
            if (ok) {
                res->md5[SW].push_back(md5Of(pic));
                if (!keepFull && kept + pic.size() <= kKeepBytes) {
                    kept += pic.size();
                    res->swPics.push_back(std::move(pic));
                } else {
                    keepFull = true;  // in order: swPics[i] is frame i of the stream
                }
            }
        } else if (f->format != AV_PIX_FMT_VAAPI) {
            fail("frame %d came out as %s: FFmpeg left VA-API and decoded in software", frames,
                 av_get_pix_fmt_name((AVPixelFormat)f->format));
            ok = false;
        } else {
            vaFrames++;
            if (doHw) {
                av_frame_unref(sw);
                sw->format = AV_PIX_FMT_NV12;
                err = av_hwframe_transfer_data(sw, f, 0);
                if (err < 0) {
                    fail("av_hwframe_transfer_data: %s", averr(err).c_str());
                    ok = false;
                } else {
                    sw->width = f->width;
                    sw->height = f->height;
                    ok = packSoftware(sw, (int)f->crop_left, (int)f->crop_top, &pic);
                    if (ok) {
                        size_t i = res->md5[HW].size();
                        res->md5[HW].push_back(md5Of(pic));
                        if (i < res->swPics.size())
                            res->psnr.push_back(psnrOf(res->swPics[i], pic, &res->maxDiff));
                    }
                }
            }
            if (ok && doVpp && !res->vppSkipped) {
                int r = copier.copy(f, &md5);
                if (r < 0) ok = false;
                else if (r == 0) res->vppSkipped = true;
                else res->md5[VPP].push_back(md5);
            }
        }
        if (frames == 0 && ok)
            printf("  %s: %s %s level %d, %dx%d (coded %dx%d), %s\n", hw ? "VA-API" : "software",
                   dec->name, avcodec_profile_name(s.codec, ctx->profile) ?: "?", ctx->level,
                   f->width - (int)f->crop_left, f->height - (int)f->crop_top, ctx->coded_width,
                   ctx->coded_height,
                   av_get_pix_fmt_name(hw ? ctx->sw_pix_fmt : (AVPixelFormat)f->format));
        frames++;
        av_frame_unref(f);
    }
    double dt = now() - t0;
    if (ctx)
        printf("  %s: %d frames in %.2f s (%.0f fps)%s\n", hw ? "VA-API" : "software", frames, dt,
               dt > 0 ? frames / dt : 0.0,
               hw && doVpp ? (" incl. VPP copies, " + std::to_string(copier.imports()) +
                              " gralloc imports").c_str()
                           : "");
    if (hw && ok && vaFrames != frames) ok = false;
    av_frame_free(&sw);
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (!ok) res->error = true;
    return ok;
}

// ---------------------------------------------------------------------------------------------

// Returns 0 pass, 1 mismatch, 2 error
int runStream(const std::string& path, const std::string& refPath, Va* va, bool only[NPATHS],
              int maxFrames) {
    Stream s;
    printf("\n== %s\n", path.c_str());
    if (!loadStream(path, refPath, &s)) return 2;
    printf("  %s, %zu packets, %zu reference MD5s\n", avcodec_get_name(s.codec), s.packets.size(),
           s.ref.size());
    bool any = only[SW] || only[HW] || only[VPP];
    bool want[NPATHS];
    for (int p = 0; p < NPATHS; p++) want[p] = any ? only[p] : true;
    // H.264 and VP8 are bit exact by spec; MPEG-2 only within the IEEE 1180 IDCT tolerance
    const bool exact = s.codec != AV_CODEC_ID_MPEG2VIDEO;

    Result r;
    if (want[SW] && decode(s, va, false, false, false, !exact && want[HW], maxFrames, &r))
        r.ran[SW] = true;
    if ((want[HW] || want[VPP]) &&
        decode(s, va, true, want[HW], want[VPP], false, maxFrames, &r)) {
        r.ran[HW] = want[HW];
        r.ran[VPP] = want[VPP] && !r.vppSkipped;
    }
    if (r.error) return 2;

    // Columns compared bit for bit within their group: H.264 / VP8 every path and the
    // reference (group 0); MPEG-2 hw and vpp (group 0), sw and the reference (group 1, both
    // software decodes), and sw against hw by PSNR
    size_t refN = maxFrames > 0 && (size_t)maxFrames < s.ref.size() ? maxFrames : s.ref.size();
    const std::vector<std::string> ref(s.ref.begin(), s.ref.begin() + refN);
    struct Col {
        const char* name;
        const std::vector<std::string>* md5;
        int group;
    };
    std::vector<Col> cols;
    for (int p = 0; p < NPATHS; p++)
        if (r.ran[p]) cols.push_back({kPathName[p], &r.md5[p], exact || p != SW ? 0 : 1});
    if (!ref.empty()) cols.push_back({"ref", &ref, exact ? 0 : 1});
    const bool usePsnr = !exact && r.ran[SW] && r.ran[HW];
    if (cols.empty()) {
        printf("  SKIP: nothing to compare\n");
        return 0;
    }
    size_t n = 0;
    for (const Col& c : cols) n = std::max(n, c.md5->size());
    bool countMismatch = false;
    for (const Col& c : cols)
        if (c.md5->size() != n) countMismatch = true;
    int bad = 0, low = 0;
    double minPsnr = INFINITY;
    for (size_t i = 0; i < n; i++) {
        bool same = true;
        for (int g = 0; g < 2; g++) {
            const std::string* first = nullptr;
            for (const Col& c : cols) {
                if (c.group != g) continue;
                if (i >= c.md5->size()) {
                    same = false;
                } else if (!first) {
                    first = &(*c.md5)[i];
                } else if ((*c.md5)[i] != *first) {
                    same = false;
                }
            }
        }
        bool psnrLow = false;
        if (usePsnr && i < r.psnr.size()) {
            minPsnr = std::min(minPsnr, r.psnr[i]);
            psnrLow = r.psnr[i] < kMpeg2MinPsnr;
        }
        if (!same) bad++;
        if (psnrLow) low++;
        if (!same || psnrLow || !gQuiet) {
            printf("  %4zu", i);
            for (const Col& c : cols)
                printf(" %s=%s", c.name, i < c.md5->size() ? (*c.md5)[i].c_str() : "-");
            if (usePsnr) {
                if (i >= r.psnr.size()) printf(" psnr=-");
                else if (isinf(r.psnr[i])) printf(" psnr=same");
                else printf(" psnr=%.1f", r.psnr[i]);
            }
            printf("%s%s\n", same ? "" : "  MISMATCH", psnrLow ? "  LOW PSNR" : "");
        }
    }
    // "sw==hw==vpp==ref" (H.264 / VP8), "hw==vpp, sw==ref, sw~hw (PSNR >= 40 dB)" (MPEG-2)
    std::string what;
    for (int g = 0; g < 2; g++) {
        std::string grp;
        int members = 0;
        for (const Col& c : cols)
            if (c.group == g) {
                grp += (grp.empty() ? "" : "==") + std::string(c.name);
                members++;
            }
        if (members > 1 || (members == 1 && cols.size() == 1))
            what += (what.empty() ? "" : ", ") + grp;
    }
    if (usePsnr) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%ssw~hw (PSNR >= %.0f dB)", what.empty() ? "" : ", ",
                 kMpeg2MinPsnr);
        what += buf;
        if (r.psnr.size() < n)
            printf("  PSNR over the first %zu of %zu frames (kept %zu MB of sw pictures)\n",
                   r.psnr.size(), n, kKeepBytes >> 20);
        if (r.psnr.empty())
            printf("  sw/hw: no frame compared\n");
        else if (isinf(minPsnr))
            printf("  sw and hw are bit exact on every compared frame\n");
        else
            printf("  sw/hw: lowest PSNR %.1f dB, largest byte difference %d\n", minPsnr,
                   r.maxDiff);
    } else if (!exact && r.ran[SW]) {
        printf("  MPEG-2: sw is compared with hw only by PSNR, which needs both -sw and -hw\n");
    }
    if (r.vppSkipped) what += " (vpp skipped)";
    if (n == 0) {
        printf("  FAIL %s: no frames decoded\n", what.c_str());
        return 1;
    }
    if (bad || low || countMismatch) {
        printf("  FAIL %s: %d of %zu frames differ%s%s\n", what.c_str(), bad, n,
               low ? (", " + std::to_string(low) + " below the PSNR limit").c_str() : "",
               countMismatch ? ", frame counts differ" : "");
        for (const Col& c : cols) printf("    %s: %zu frames\n", c.name, c.md5->size());
        return 1;
    }
    printf("  PASS %s, %zu frames\n", what.c_str(), n);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool only[NPATHS] = {};
    int maxFrames = 0;
    std::string ref;
    std::vector<std::string> streams;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-sw") only[SW] = true;
        else if (a == "-hw") only[HW] = true;
        else if (a == "-vpp") only[VPP] = true;
        else if (a == "-q") gQuiet = true;
        else if (a == "-n" && i + 1 < argc) maxFrames = atoi(argv[++i]);
        else if (a == "-ref" && i + 1 < argc) ref = argv[++i];
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "Usage: %s [-sw|-hw|-vpp] [-q] [-n frames] [-ref file.md5] "
                    "[stream...]\n", argv[0]);
            return 2;
        } else streams.push_back(a);
    }
    if (streams.empty()) {
        // the vectors wt20.mk installs, in a fixed order. 320 wide: pitch == width; 176 and
        // 352 wide: pitch wider than the picture; 168x136 (coded 176x144): right / bottom
        // crop, a block larger than the picture both ways; plain Baseline: the HAL's
        // ALLOW_PROFILE_MISMATCH path
        static const char* const kVectors[] = {
                "test-25fps.h264",
                "test-25fps-interlaced.h264",
                "bbb_avc_176x144_300kbps_60fps.h264",
                "wt20_crop_168x136.h264",
                "wt20_baseline_176x144.h264",
                "test-25fps.vp8",
                "bbb_mpeg2_352x288_1mbps_60fps.m2v",
                "bbb_mpeg2_176x144_105kbps_25fps.m2v",
        };
        for (const char* v : kVectors) {
            std::string p = std::string(kVectorDir) + "/" + v;
            if (access(p.c_str(), R_OK) == 0) streams.push_back(p);
        }
        if (streams.empty()) {
            fail("no stream given and none in %s", kVectorDir);
            return 2;
        }
    }
    if (!ref.empty() && streams.size() != 1) {
        fail("-ref needs exactly one stream");
        return 2;
    }

    Va va;
    bool needVa = only[HW] || only[VPP] || !only[SW];
    bool needVpp = only[VPP] || !(only[SW] || only[HW]);
    if (needVa && !openVa(&va, needVpp)) {
        closeVa(&va);
        printf("\nRESULT: ERROR (VA-API setup)\n");
        return 2;
    }
    int worst = 0, passed = 0;
    for (const std::string& s : streams) {
        int r = runStream(s, ref, &va, only, maxFrames);
        if (r == 0) passed++;
        if (r > worst) worst = r;
    }
    closeVa(&va);
    printf("\nRESULT: %s (%d of %zu streams passed)\n",
           worst == 0 ? "PASS" : worst == 1 ? "FAIL" : "ERROR", passed, streams.size());
    return worst;
}
