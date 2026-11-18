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

#ifndef C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
#define C2_FFMPEG_VIDEO_DECODE_COMPONENT_H

#include <deque>
#include <utility>
#include <vector>
#include <SimpleC2Component.h>
#include "C2FFMPEGCommon.h"
#include "C2FFMPEGVaapi.h"
#include "C2FFMPEGVideoDecodeInterface.h"

namespace android {

typedef std::pair<uint64_t, uint64_t> PendingWork;
typedef std::deque<std::pair<std::vector<uint8_t>, int64_t>> PacketQueue;  // data, pts

class C2FFMPEGVideoDecodeComponent : public SimpleC2Component {
public:
    explicit C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf);
    virtual ~C2FFMPEGVideoDecodeComponent();

protected:
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) override;

private:
    c2_status_t initDecoder();
    c2_status_t openDecoder();
    c2_status_t openCodec(bool allowHw);
    void closeCodec();
    c2_status_t fallbackToSoftware(const char* reason);
    c2_status_t reopenSoftware(const char* reason);
    void keepPreroll(const uint8_t* data, size_t size, int64_t timestamp);
    int replayPreroll(PacketQueue* packets);
    void deInitDecoder();
    c2_status_t processCodecConfig(C2ReadView* inBuffer);
    c2_status_t sendInputBuffer(C2ReadView* inBuffer, int64_t timestamp);
    c2_status_t receiveFrame(bool* hasPicture);
    void noteDecodeError(int err, const char* where);
    c2_status_t copyToGraphicView(const AVFrame* frame, C2GraphicView* outBuffer);
    c2_status_t outputFrame(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool);

    static enum AVPixelFormat GetFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts);
    enum AVPixelFormat chooseFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts);

    void pushPendingWork(const std::unique_ptr<C2Work>& work);
    void popPendingWork(const std::unique_ptr<C2Work>& work);
    void prunePendingWorksUntil(const std::unique_ptr<C2Work>& work);

private:
    const C2FFMPEGComponentInfo* mInfo;
    std::shared_ptr<C2FFMPEGVideoDecodeInterface> mIntf;
    enum AVCodecID mCodecID;
    AVCodecContext* mCtx;
    struct SwsContext *mImgConvertCtx;
    AVFrame* mFrame;
    AVFrame* mSwFrame;        // CPU copy of a VA frame (av_hwframe_transfer_data)
    AVPacket* mPacket;
    bool mFFMPEGInitialized;
    bool mCodecAlreadyOpened;
    bool mExtradataReady;
    bool mEOSSignalled;
    std::deque<PendingWork> mPendingWorkQueue;

    // WT20 VA-API state
    std::vector<uint8_t> mExtradata;   // codec-specific data, kept for a software reopen
    AVBufferRef* mHwDevice;            // FFmpeg VAAPI device of this instance
    bool mHwSession;                   // holds one of wt20::kMaxHwSessions
    bool mForceSoftware;               // reopened without hwaccel (mSoftwareReason)
    const char* mSoftwareReason;
    bool mHwRefused;                   // swfallback=0 and the stream cannot use VA-API
    bool mSwitchToSoftware;            // too many VA errors: reopen after this work
    bool mLoggedFormat;                // get_format decision logged for this stream
    bool mLoggedCpuCopy;               // CPU output path logged for this stream
    int mHwErrors;                     // consecutive VA errors
    wt20::VaSurfaceCopier mCopier;

    // First get_format of a VA instance (opened with one thread): if it picks software, the
    // decoder is reopened with threads and fed mPreroll again (see sendInputBuffer).
    bool mInSend;                      // inside avcodec_send_packet()
    bool mFormatInSend;                // get_format ran during this avcodec_send_packet()
    bool mFormatDecided;               // the first get_format of this open is over
    const char* mReopenReason;         // non-null: that first get_format picked software
    bool mPrerollOverflow;             // too much data before the first get_format
    size_t mPrerollBytes;
    PacketQueue mPreroll;
};

} // namespace android

#endif // C2_FFMPEG_VIDEO_DECODE_COMPONENT_H
