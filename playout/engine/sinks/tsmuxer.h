#ifndef PLAYPLAY_TSMUXER_H
#define PLAYPLAY_TSMUXER_H

#include <QString>
#include <QJsonObject>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

/**
 * TsMuxer — an MPEG-TS multiplexer that hands you bytes.
 *
 * Wraps libavformat's mpegts muxer with a custom AVIO context so the output
 * goes to a callback instead of a URL. Used by hardware sinks (DekTec ASI)
 * and any transport that is not an FFmpeg protocol. Constant-rate mode with
 * null-packet stuffing when `muxrate` is set, so the byte stream itself is
 * the ASI clock.
 *
 * Options (JSON): muxrate (bps), pcrPeriodMs, patPeriodSec, sdtPeriodSec, serviceId, serviceName,
 * provider, serviceType (digital_tv|...), pmtPid, videoPid, audioPid, pcrPid (must equal videoPid:
 * libavformat carries the PCR on the video PID), tsId, networkId, tablesVersion, pesPayloadSize, muxDelayMs.
 */
class TsMuxer {
public:
    using ByteSink = std::function<void(const uint8_t* data, int size)>;

    TsMuxer();
    ~TsMuxer();

    /// Shared with FfmpegMuxSink: apply the TS options above to an mpegts AVFormatContext (muxer
    /// options into `opts`, metadata + stream PIDs on the context). Returns a warning text or empty.
    static QString applyOptions(AVFormatContext* fmt, AVDictionary** opts, const QJsonObject& o, int videoIdx, int audioIdx);

    bool open(const AVCodecParameters* video, AVRational videoTimeBase,
              const AVCodecParameters* audio, AVRational audioTimeBase,
              const QJsonObject& options, ByteSink sink);
    void close();
    bool isOpen() const { return m_fmt != nullptr; }

    /// Thread of the caller; the muxer is not internally synchronised.
    bool writePacket(const AVPacket* pkt, int streamKind);   // 0 video, 1 audio
    QString lastError() const { return m_error; }
    qint64 bytesOut() const { return m_bytes; }
    qint64 muxrate() const { return m_muxrate; }

private:
    AVFormatContext* m_fmt = nullptr;
    AVIOContext* m_avio = nullptr;
    uint8_t* m_avioBuf = nullptr;
    ByteSink m_sink;
    int m_videoIdx = -1, m_audioIdx = -1;
    AVRational m_videoTb{1, 30}, m_audioTb{1, 48000};
    qint64 m_bytes = 0;
    qint64 m_muxrate = 0;
    QString m_error;

    static int writeThunk(void* opaque, const uint8_t* buf, int size);
};

#endif // PLAYPLAY_TSMUXER_H
