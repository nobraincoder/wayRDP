#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <QDebug>
#include <atomic>
#include "video/CodecHooks.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

static std::atomic<bool> s_requestKeyframe{false};

extern "C" Q_DECL_EXPORT void CodecHooks_requestKeyframe(void)
{
    s_requestKeyframe.store(true, std::memory_order_release);
    qInfo() << "CodecHooks: Keyframe generation requested";
}

// Global ELF symbol interposition for avcodec_open2 and avcodec_send_frame.
// When KPipeWire initializes an FFmpeg video encoder, this hook intercepts the call
// to enforce true zero-latency encoding parameters:
// 1. max_b_frames = 0: Disables bi-directional B-frames. RDPGFX (MS-RDPEGFX AVC420)
//    has no presentation timestamp (PTS) reordering; B-frames cause out-of-order
//    frame delivery and introduce a 1-to-2 frame lookahead delay. That delay causes
//    the final frame of closed/minimized windows to remain trapped inside the GPU
//    encoder until subsequent user input occurs (lingering window ghosting).
// 2. bf = 0: Instructs FFmpeg encoder implementations to use only I- and P-frames.
// 3. async_depth = 1: Enforces immediate single-frame pipeline synchronization.
// 4. gop_size = 30 (configurable via RDP_GOP_SIZE): Enforces periodic IDR keyframes
//    every 0.5s at 60 FPS to bound reference chain drift and recover promptly from scene changes.
// 5. idr_interval = 0: Ensures every intra frame generated is an IDR keyframe.
extern "C" Q_DECL_EXPORT int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options)
{
    static auto real_avcodec_open2 = reinterpret_cast<int (*)(AVCodecContext *, const AVCodec *, AVDictionary **)>(
        dlsym(RTLD_NEXT, "avcodec_open2"));
    if (!real_avcodec_open2) {
        return -1;
    }

    if (codec && avctx) {
        int gopSize = 120;
        bool ok = false;
        int envGop = qEnvironmentVariable("RDP_GOP_SIZE").toInt(&ok);
        if (ok && envGop > 0) {
            gopSize = envGop;
        }

        qInfo() << "CodecHooks: Intercepted avcodec_open2 for encoder:" << codec->name
                << "- enforcing max_b_frames=0, bf=0, refs=1, async_depth=1, gop_size=" << gopSize;
        avctx->max_b_frames = 0;
        avctx->refs = 1;
        avctx->gop_size = gopSize;
        if (options) {
            av_dict_set(options, "bf", "0", 0);
            av_dict_set(options, "async_depth", "1", 0);
            av_dict_set(options, "idr_interval", "0", 0);
        }
    }

    return real_avcodec_open2(avctx, codec, options);
}

extern "C" Q_DECL_EXPORT int avcodec_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    static auto real_avcodec_send_frame = reinterpret_cast<int (*)(AVCodecContext *, const AVFrame *)>(
        dlsym(RTLD_NEXT, "avcodec_send_frame"));
    if (!real_avcodec_send_frame) {
        return -1;
    }

    if (frame && s_requestKeyframe.exchange(false, std::memory_order_acq_rel)) {
        AVFrame *mutable_frame = const_cast<AVFrame*>(frame);
        mutable_frame->pict_type = AV_PICTURE_TYPE_I;
#ifdef AV_FRAME_FLAG_KEY
        mutable_frame->flags |= AV_FRAME_FLAG_KEY;
#endif
        qInfo() << "CodecHooks: Injected keyframe flags into avcodec_send_frame";
    }

    return real_avcodec_send_frame(avctx, frame);
}
