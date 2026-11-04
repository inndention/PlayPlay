#ifndef PLAYPLAY_MEDIAFRAME_H
#define PLAYPLAY_MEDIAFRAME_H

#include <QtGlobal>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/buffer.h>
}

/**
 * Shared, reference-counted AVFrame. Frames flow decoder -> mixer -> N sinks on
 * different threads; shared_ptr keeps the buffer alive until the last consumer
 * drops it, and AVFrame's own refcounted buffers make copies cheap.
 */
using FramePtr = std::shared_ptr<AVFrame>;

inline FramePtr makeFramePtr(AVFrame* f = nullptr) {
    if (!f) f = av_frame_alloc();
    return FramePtr(f, [](AVFrame* p) { av_frame_free(&p); });
}

/// Allocate a video frame with its own buffer (32-byte aligned planes).
inline FramePtr allocVideoFrame(int width, int height, AVPixelFormat fmt) {
    FramePtr f = makeFramePtr();
    f->width = width;
    f->height = height;
    f->format = fmt;
    if (av_frame_get_buffer(f.get(), 32) < 0) return nullptr;
    return f;
}

/**
 * FramePool - recycles BGRA video buffers so the hot path never calls malloc.
 * Frames obtained here are ordinary FramePtrs; when the last reference drops
 * the buffer returns to the pool instead of being freed.
 */
class FramePool {
public:
    FramePool(int width, int height, AVPixelFormat fmt = AV_PIX_FMT_BGRA)
        : m_w(width), m_h(height), m_fmt(fmt) {
        m_linesize = ((width * 4) + 63) & ~63;           // 64-byte aligned rows
        m_pool = av_buffer_pool_init(size_t(m_linesize) * height + 64, nullptr);
    }
    ~FramePool() { if (m_pool) av_buffer_pool_uninit(&m_pool); }
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;

    FramePtr get() {
        FramePtr f = makeFramePtr();
        f->buf[0] = av_buffer_pool_get(m_pool);
        if (!f->buf[0]) return nullptr;
        f->width = m_w; f->height = m_h; f->format = m_fmt;
        f->data[0] = f->buf[0]->data;
        f->linesize[0] = m_linesize;
        return f;
    }
    int width() const { return m_w; }
    int height() const { return m_h; }

private:
    int m_w, m_h, m_linesize;
    AVPixelFormat m_fmt;
    AVBufferPool* m_pool = nullptr;
};

/// Allocate a planar-float audio frame for `nbSamples` x `channels` @ `sampleRate`.
inline FramePtr allocAudioFrame(int nbSamples, int channels, int sampleRate,
                                AVSampleFormat fmt = AV_SAMPLE_FMT_FLTP) {
    FramePtr f = makeFramePtr();
    f->nb_samples = nbSamples;
    f->format = fmt;
    f->sample_rate = sampleRate;
    av_channel_layout_default(&f->ch_layout, channels);
    if (nbSamples > 0 && av_frame_get_buffer(f.get(), 0) < 0) return nullptr;
    return f;
}

/// Zero every sample plane of an audio frame.
inline void silenceAudioFrame(const FramePtr& f) {
    if (!f) return;
    const int planes = av_sample_fmt_is_planar(AVSampleFormat(f->format)) ? f->ch_layout.nb_channels : 1;
    const int bytes = av_samples_get_buffer_size(nullptr, planes == 1 ? f->ch_layout.nb_channels : 1,
                                                 f->nb_samples, AVSampleFormat(f->format), 0);
    for (int p = 0; p < planes && f->data[p]; ++p) {
        memset(f->data[p], 0, bytes);
    }
}

/**
 * One frame period of program output: the composited picture plus exactly the
 * audio samples that belong to that frame (1601/1602 cadence at 29.97).
 */
struct MediaFrame {
    qint64 frameNumber = 0;     // Output timeline frame index
    qint64 timestampNs = 0;     // Steady-clock time this frame is due
    FramePtr video;             // BGRA, output size
    FramePtr audio;             // FLTP, sampleRate, N channels, nb_samples for this frame
};

/// Video pixel format used throughout the mixer stage. Memory order B,G,R,A
/// matches QImage::Format_ARGB32 on little-endian, so QPainter can draw into
/// it without conversion.
constexpr AVPixelFormat kMixerPixelFormat = AV_PIX_FMT_BGRA;

#endif // PLAYPLAY_MEDIAFRAME_H
