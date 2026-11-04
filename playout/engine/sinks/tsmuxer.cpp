#include "tsmuxer.h"
#include <QDebug>

extern "C" {
#include <libavutil/opt.h>
}

namespace {
QString avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}
constexpr int kAvioBuf = 188 * 7 * 8;   // 7 TS packets per UDP-sized chunk, x8
}

TsMuxer::TsMuxer() = default;
TsMuxer::~TsMuxer() { close(); }

// ---------------------------------------------------------------------------
// TS options shared by the ASI path (this muxer) and FfmpegMuxSink (UDP/SRT/file)
// ---------------------------------------------------------------------------
QString TsMuxer::applyOptions(AVFormatContext* fmt, AVDictionary** opts, const QJsonObject& o, int videoIdx, int audioIdx) {
    QString warn;
    // Library default is zero mux delay, which makes the CBR scheduler flag every packet as late
    // ("dts < pcr"). The ffmpeg CLI uses 0.7 s.
    fmt->max_delay = int(o.value("muxDelayMs").toDouble(700.0) * 1000.0);
    const qint64 muxrate = qint64(o.value("muxrate").toDouble(0));
    if (muxrate > 0) av_dict_set(opts, "muxrate", QByteArray::number(muxrate), 0);
    av_dict_set(opts, "pcr_period", QByteArray::number(qBound(1, o.value("pcrPeriodMs").toInt(20), 100)), 0);
    av_dict_set(opts, "pat_period", QByteArray::number(qBound(0.02, o.value("patPeriodSec").toDouble(0.1), 1.0)), 0);
    av_dict_set(opts, "sdt_period", QByteArray::number(qBound(0.1, o.value("sdtPeriodSec").toDouble(0.5), 2.0)), 0);
    av_dict_set(opts, "mpegts_flags", "+resend_headers+pat_pmt_at_frames", 0);
    av_dict_set(opts, "mpegts_service_id", QByteArray::number(o.value("serviceId").toInt(1)), 0);
    av_dict_set(opts, "mpegts_service_type", o.value("serviceType").toString("digital_tv").toUtf8(), 0);
    av_dict_set(opts, "mpegts_pmt_start_pid", QByteArray::number(o.value("pmtPid").toInt(0x1000)), 0);
    av_dict_set(opts, "mpegts_start_pid", QByteArray::number(o.value("videoPid").toInt(0x100)), 0);
    av_dict_set(opts, "mpegts_transport_stream_id", QByteArray::number(o.value("tsId").toInt(1)), 0);
    av_dict_set(opts, "mpegts_original_network_id", QByteArray::number(o.value("networkId").toInt(1)), 0);
    if (o.contains("tablesVersion")) av_dict_set(opts, "tables_version", QByteArray::number(qBound(0, o.value("tablesVersion").toInt(), 31)), 0);
    if (o.contains("pesPayloadSize")) av_dict_set(opts, "pes_payload_size", QByteArray::number(qMax(188, o.value("pesPayloadSize").toInt(2930))), 0);
    av_dict_set(&fmt->metadata, "service_name", o.value("serviceName").toString("PLAY PLAY").toUtf8(), 0);
    av_dict_set(&fmt->metadata, "service_provider", o.value("provider").toString("PlayPlay").toUtf8(), 0);
    // Explicit PIDs ride on AVStream::id (16..0x1FFE); the PCR always travels on the video PID.
    if (videoIdx >= 0 && o.contains("videoPid")) fmt->streams[videoIdx]->id = o["videoPid"].toInt();
    if (audioIdx >= 0 && o.contains("audioPid")) fmt->streams[audioIdx]->id = o["audioPid"].toInt();
    if (o.contains("pcrPid") && o.value("pcrPid").toInt() != o.value("videoPid").toInt(0x100)) warn = QString("pcrPid %1 ignored: libavformat puts the PCR on the video PID %2").arg(o.value("pcrPid").toInt()).arg(o.value("videoPid").toInt(0x100));
    return warn;
}

int TsMuxer::writeThunk(void* opaque, const uint8_t* buf, int size) {
    auto* self = static_cast<TsMuxer*>(opaque);
    if (self->m_sink) self->m_sink(buf, size);
    self->m_bytes += size;
    return size;
}

bool TsMuxer::open(const AVCodecParameters* video, AVRational videoTimeBase,
                   const AVCodecParameters* audio, AVRational audioTimeBase,
                   const QJsonObject& o, ByteSink sink) {
    close();
    m_sink = std::move(sink);
    m_videoTb = videoTimeBase; m_audioTb = audioTimeBase;

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr, "mpegts", nullptr);
    if (ret < 0 || !m_fmt) { m_error = "mpegts muxer unavailable: " + avErr(ret); return false; }

    m_avioBuf = static_cast<uint8_t*>(av_malloc(kAvioBuf));
    m_avio = avio_alloc_context(m_avioBuf, kAvioBuf, 1, this, nullptr, &TsMuxer::writeThunk, nullptr);
    if (!m_avio) { m_error = "avio alloc failed"; close(); return false; }
    m_avio->max_packet_size = 188 * 7;   // flush in 1316-byte chunks: integral TS packets
    m_fmt->pb = m_avio;
    m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (video) {
        AVStream* st = avformat_new_stream(m_fmt, nullptr);
        avcodec_parameters_copy(st->codecpar, video);
        st->time_base = videoTimeBase; st->avg_frame_rate = av_inv_q(videoTimeBase); st->codecpar->codec_tag = 0;
        m_videoIdx = st->index;
    }
    if (audio) {
        AVStream* st = avformat_new_stream(m_fmt, nullptr);
        avcodec_parameters_copy(st->codecpar, audio);
        st->time_base = audioTimeBase; st->codecpar->codec_tag = 0;
        m_audioIdx = st->index;
    }

    m_muxrate = qint64(o.value("muxrate").toDouble(0));
    AVDictionary* opts = nullptr;
    const QString warn = applyOptions(m_fmt, &opts, o, m_videoIdx, m_audioIdx);
    if (!warn.isEmpty()) qWarning().noquote() << "[TsMuxer]" << warn;

    ret = avformat_write_header(m_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) { m_error = "write_header failed: " + avErr(ret); close(); return false; }
    return true;
}

void TsMuxer::close() {
    if (m_fmt) {
        if (m_fmt->pb) av_write_trailer(m_fmt);
        m_fmt->pb = nullptr;
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    if (m_avio) {
        // avio_context_free frees the context; the buffer belongs to us.
        av_freep(&m_avio->buffer);
        avio_context_free(&m_avio);
        m_avioBuf = nullptr;
    } else if (m_avioBuf) {
        av_free(m_avioBuf); m_avioBuf = nullptr;
    }
    m_videoIdx = m_audioIdx = -1;
}

bool TsMuxer::writePacket(const AVPacket* pkt, int streamKind) {
    if (!m_fmt) return false;
    const int idx = streamKind == 0 ? m_videoIdx : m_audioIdx;
    if (idx < 0) return false;
    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return false;
    copy->stream_index = idx;
    av_packet_rescale_ts(copy, streamKind == 0 ? m_videoTb : m_audioTb, m_fmt->streams[idx]->time_base);
    const int ret = av_interleaved_write_frame(m_fmt, copy);
    av_packet_free(&copy);
    if (ret < 0) { m_error = "write failed: " + avErr(ret); return false; }
    return true;
}
