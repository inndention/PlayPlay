#include "ffmpegmuxsink.h"
#include "tsmuxer.h"
#include "../../supervisor/livestream.h"
#include <QDebug>
#include <QFileInfo>
#include <QUrl>

extern "C" {
#include <libavutil/opt.h>
}

namespace {
QString avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}
} // namespace

FfmpegMuxSink::FfmpegMuxSink(const QString& name, const QString& url, const QJsonObject& options, QObject* parent)
    : PacketSink(name, parent), m_url(url), m_opts(options)
{
}

FfmpegMuxSink::~FfmpegMuxSink() {
    stop();
}

QString FfmpegMuxSink::guessFormatName() const {
    const QString u = m_url.toLower();
    if (u.startsWith("udp://") || u.startsWith("srt://") || u.startsWith("rist://")) return "mpegts";
    if (u.startsWith("rtp://")) return "rtp_mpegts";
    if (u.startsWith("rtmp")) return "flv";
    if (u.endsWith(".ts") || u.endsWith(".m2ts")) return "mpegts";
    return QString(); // let avformat guess from the extension
}

bool FfmpegMuxSink::start(const AVCodecParameters* video, AVRational videoTimeBase,
                          const AVCodecParameters* audio, AVRational audioTimeBase) {
    if (m_running.load()) return true;
    m_videoTb = videoTimeBase;
    m_audioTb = audioTimeBase;

    QString path = m_url;
    if (path.startsWith("file://")) path = QUrl(path).toLocalFile();
    const QString fmtName = guessFormatName();

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr,
        fmtName.isEmpty() ? nullptr : fmtName.toUtf8().constData(), path.toUtf8().constData());
    if (ret < 0 || !m_fmt) {
        setError("cannot create muxer for " + livestream::redactUrl(m_url) + ": " + avErr(ret));
        return false;
    }

    // Streams
    if (video) {
        AVStream* st = avformat_new_stream(m_fmt, nullptr);
        avcodec_parameters_copy(st->codecpar, video);
        st->time_base = videoTimeBase;
        st->avg_frame_rate = av_inv_q(videoTimeBase);
        st->codecpar->codec_tag = 0;
        m_videoIdx = st->index;
    }
    if (audio) {
        AVStream* st = avformat_new_stream(m_fmt, nullptr);
        avcodec_parameters_copy(st->codecpar, audio);
        st->time_base = audioTimeBase;
        st->codecpar->codec_tag = 0;
        m_audioIdx = st->index;
    }

    // Muxer options
    AVDictionary* opts = nullptr;
    const bool isTs = (m_fmt->oformat && QString(m_fmt->oformat->name) == "mpegts");
    if (isTs) {
        // muxrate, PCR/PAT/SDT periods, service id/name/provider/type, PIDs, tables version: one place for ASI + IP.
        const QString warn = TsMuxer::applyOptions(m_fmt, &opts, m_opts, m_videoIdx, m_audioIdx);
        if (!warn.isEmpty()) qWarning().noquote() << "[MuxSink]" << m_name << warn;
    }
    if (QString(m_fmt->oformat->name) == "flv") {
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
    }

    // Protocol options via the URL for udp/srt (avio_open2 honours them in opts too)
    AVDictionary* ioOpts = nullptr;
    const QString lower = m_url.toLower();
    if (lower.startsWith("udp://")) {
        av_dict_set(&ioOpts, "pkt_size", QByteArray::number(m_opts.value("pktSize").toInt(1316)), 0);
        const qint64 muxrate = qint64(m_opts.value("muxrate").toDouble(0));
        // Paced UDP: FFmpeg's udp sender thread drains a circular buffer at `bitrate`; the TS muxer writes
        // in bursts at exactly muxrate, so give the pace 8 % headroom and a deep FIFO or the buffer
        // overruns (udp_write then fails with ENOMEM, "Cannot allocate memory").
        // No FFmpeg-side pacing (`bitrate`): its sender thread sleeps per packet and Windows timer
        // granularity caps that at about 1 Mb/s. The encoder runs on the frame clock, so packets leave in
        // per-frame bursts that any TS receiver (and tsp's merge plugin) absorbs; just enlarge the socket buffer.
        Q_UNUSED(muxrate);
        av_dict_set(&ioOpts, "buffer_size", "8388608", 0);
    } else if (lower.startsWith("srt://")) {
        av_dict_set(&ioOpts, "pkt_size", "1316", 0);
        av_dict_set(&ioOpts, "latency", QByteArray::number(m_opts.value("latencyMs").toInt(120) * 1000), 0);
        av_dict_set(&ioOpts, "transtype", "live", 0);
    }

    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&m_fmt->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE, nullptr, &ioOpts);
        av_dict_free(&ioOpts);
        if (ret < 0) {
            setError("cannot open " + livestream::redactUrl(m_url) + ": " + avErr(ret));
            av_dict_free(&opts);
            closeFormat();
            return false;
        }
    }

    ret = avformat_write_header(m_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        setError("write_header failed for " + livestream::redactUrl(m_url) + ": " + avErr(ret));
        closeFormat();
        return false;
    }
    m_headerWritten = true;

    m_stop = false;
    m_running = true;
    { QMutexLocker l(&m_statMutex); m_stats.connected = true; }
    m_writer = new Writer(this);
    m_writer->start(QThread::HighPriority);
    qInfo().noquote() << QString("[MuxSink] %1 -> %2 (%3)").arg(m_name, livestream::redactUrl(m_url), m_fmt->oformat->name);
    return true;
}

void FfmpegMuxSink::stop() {
    if (!m_running.load() && !m_writer) return;
    m_stop = true;
    { QMutexLocker l(&m_qmutex); m_qcond.wakeAll(); }
    if (m_writer) { if (m_drainOnStop) m_writer->wait(); else m_writer->wait(5000); delete m_writer; m_writer = nullptr; }
    {
        QMutexLocker l(&m_qmutex);
        for (AVPacket* p : m_queue) av_packet_free(&p);
        m_queue.clear();
    }
    closeFormat();
    m_running = false;
    QMutexLocker l(&m_statMutex); m_stats.connected = false;
}

void FfmpegMuxSink::closeFormat() {
    if (m_fmt) {
        if (m_headerWritten) av_write_trailer(m_fmt);
        if (!(m_fmt->oformat->flags & AVFMT_NOFILE) && m_fmt->pb) avio_closep(&m_fmt->pb);
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    m_headerWritten = false;
    m_videoIdx = m_audioIdx = -1;
}

void FfmpegMuxSink::writePacket(const AVPacket* pkt, int streamKind) {
    if (!m_running.load()) return;
    const int idx = streamKind == 0 ? m_videoIdx : m_audioIdx;
    if (idx < 0) return;
    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return;
    copy->stream_index = idx;
    // Rescale here (encoder time base -> stream time base) so the writer thread
    // only has to call the muxer.
    av_packet_rescale_ts(copy, streamKind == 0 ? m_videoTb : m_audioTb, m_fmt->streams[idx]->time_base);

    QMutexLocker l(&m_qmutex);
    if (int(m_queue.size()) >= kMaxQueue) {
        AVPacket* old = m_queue.front(); m_queue.pop_front();
        av_packet_free(&old);
        QMutexLocker s(&m_statMutex); m_stats.dropped++;
    }
    m_queue.push_back(copy);
    { QMutexLocker s(&m_statMutex); m_stats.framesIn++; }
    m_qcond.wakeOne();
}

void FfmpegMuxSink::writeLoop() {
    while (true) {
        AVPacket* pkt = nullptr;
        {
            QMutexLocker l(&m_qmutex);
            while (m_queue.empty() && !m_stop.load()) m_qcond.wait(&m_qmutex, 100);
            if (m_stop.load() && (!m_drainOnStop || m_queue.empty())) break;
            if (m_queue.empty()) continue;
            pkt = m_queue.front();
            m_queue.pop_front();
        }
        const int size = pkt->size;
        int ret = av_interleaved_write_frame(m_fmt, pkt);   // takes ownership of the data ref
        av_packet_free(&pkt);
        if (ret < 0) {
            setError("write failed: " + avErr(ret));
            QMutexLocker s(&m_statMutex); m_stats.dropped++;
        } else {
            QMutexLocker s(&m_statMutex); m_stats.framesOut++; m_stats.bytesOut += size;
        }
    }
}
