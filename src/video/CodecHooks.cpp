#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <QDebug>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

// Global ELF symbol interposition for avcodec_open2.
// When KPipeWire initializes an FFmpeg video encoder, this hook intercepts the call
// to enforce true zero-latency encoding parameters:
// 1. max_b_frames = 0: Disables bi-directional B-frames. RDPGFX (MS-RDPEGFX AVC420)
//    has no presentation timestamp (PTS) reordering; B-frames cause out-of-order
//    frame delivery and introduce a 1-to-2 frame lookahead delay. That delay causes
//    the final frame of closed/minimized windows to remain trapped inside the GPU
//    encoder until subsequent user input occurs (lingering window ghosting).
// 2. bf = 0: Instructs FFmpeg encoder implementations to use only I- and P-frames.
// 3. async_depth = 1: Enforces immediate single-frame pipeline synchronization.
extern "C" Q_DECL_EXPORT int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options)
{
    static auto real_avcodec_open2 = reinterpret_cast<int (*)(AVCodecContext *, const AVCodec *, AVDictionary **)>(
        dlsym(RTLD_NEXT, "avcodec_open2"));
    if (!real_avcodec_open2) {
        return -1;
    }

    if (codec && avctx) {
        qInfo() << "CodecHooks: Intercepted avcodec_open2 for encoder:" << codec->name
                << "- enforcing max_b_frames=0, bf=0, async_depth=1";
        avctx->max_b_frames = 0;
        if (options) {
            av_dict_set(options, "bf", "0", 0);
            av_dict_set(options, "async_depth", "1", 0);
        }
    }

    return real_avcodec_open2(avctx, codec, options);
}
