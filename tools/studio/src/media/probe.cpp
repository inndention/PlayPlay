// Media probe on libav (ST-17). See probe.h.
#include "media/probe.h"

#include "media/avopen.h"

#include <QJsonValue>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

namespace studio::media {
namespace {

int evenAtLeast2(double v) {
    int x = int(std::lround(v));
    x -= (x & 1);
    return qMax(2, x);
}

// fftools get_rotation(): the display matrix says how far the picture is turned counter-clockwise; players turn it
// back clockwise by -theta, normalised into [0, 360) with the same 0.9 degree tolerance.
int rotationOf(const AVCodecParameters* par) {
    const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * 4) return 0;
    double theta = -std::round(av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data)));
    if (!std::isfinite(theta)) return 0;
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    return int(std::lround(theta)) % 360;
}

// The main stream of a kind; cover art (an attached picture in an mp3/m4a) is not a video stream for our purposes.
int bestStream(AVFormatContext* fc, AVMediaType type) {
    int best = av_find_best_stream(fc, type, -1, -1, nullptr, 0);
    if (best >= 0 && !(fc->streams[best]->disposition & AV_DISPOSITION_ATTACHED_PIC)) return best;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type == type && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) return int(i);
    }
    return -1;
}

double streamSeconds(const AVStream* st) {
    return st->duration != AV_NOPTS_VALUE && st->duration > 0 ? double(st->duration) * av_q2d(st->time_base) : 0.0;
}

// Demux every packet: the end of the longest stream, relative to its own first timestamp.
double scanDuration(AVFormatContext* fc, av::Deadline& dl, QString* error) {
    std::vector<int64_t> first(fc->nb_streams, AV_NOPTS_VALUE), last(fc->nb_streams, AV_NOPTS_VALUE);
    av::PacketPtr pkt = av::newPacket();
    if (!pkt) { *error = QStringLiteral("out of memory"); return 0; }
    for (qint64 n = 0;; ++n) {
        if ((n & 255) == 0 && dl.expired()) { *error = dl.reason(); return 0; }
        const int r = av_read_frame(fc, pkt.get());
        if (r == AVERROR_EOF) break;
        if (r < 0) { if (dl.expired()) { *error = dl.reason(); return 0; } break; }   // a damaged tail: keep what we read
        const unsigned s = unsigned(pkt->stream_index);
        const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (s < fc->nb_streams && ts != AV_NOPTS_VALUE) {
            if (first[s] == AV_NOPTS_VALUE || ts < first[s]) first[s] = ts;
            const int64_t end = ts + qMax<int64_t>(0, pkt->duration);
            if (last[s] == AV_NOPTS_VALUE || end > last[s]) last[s] = end;
        }
        av_packet_unref(pkt.get());
    }
    double d = 0;
    for (unsigned s = 0; s < fc->nb_streams; ++s)
        if (first[s] != AV_NOPTS_VALUE && last[s] != AV_NOPTS_VALUE) d = qMax(d, double(last[s] - first[s]) * av_q2d(fc->streams[s]->time_base));
    return d;
}

QString str(const char* s) { return s ? QString::fromLatin1(s) : QString(); }

} // namespace

QJsonObject MediaInfo::toJson() const {
    if (!ok) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    QJsonObject o{
        {QStringLiteral("ok"), true},
        {QStringLiteral("format"), format},
        {QStringLiteral("formatLongName"), formatLongName},
        {QStringLiteral("durationSec"), durationSec},
        {QStringLiteral("durationExact"), durationExact},
        {QStringLiteral("sizeBytes"), double(sizeBytes)},
        {QStringLiteral("bitRate"), double(bitRate)},
        {QStringLiteral("streams"), streams},
        {QStringLiteral("isImage"), isImage},
    };
    if (video.present) {
        o.insert(QStringLiteral("video"), QJsonObject{
            {QStringLiteral("index"), video.index}, {QStringLiteral("codec"), video.codec},
            {QStringLiteral("width"), video.width}, {QStringLiteral("height"), video.height},
            {QStringLiteral("displayWidth"), video.displayWidth}, {QStringLiteral("fps"), video.fps},
            {QStringLiteral("pixFmt"), video.pixFmt}, {QStringLiteral("rotation"), video.rotation},
            {QStringLiteral("frames"), double(video.frames)}, {QStringLiteral("bitRate"), double(video.bitRate)}});
    } else {
        o.insert(QStringLiteral("video"), QJsonValue::Null);
    }
    if (audio.present) {
        o.insert(QStringLiteral("audio"), QJsonObject{
            {QStringLiteral("index"), audio.index}, {QStringLiteral("codec"), audio.codec},
            {QStringLiteral("sampleRate"), audio.sampleRate}, {QStringLiteral("channels"), audio.channels},
            {QStringLiteral("layout"), audio.layout}, {QStringLiteral("sampleFmt"), audio.sampleFmt},
            {QStringLiteral("bitRate"), double(audio.bitRate)}});
    } else {
        o.insert(QStringLiteral("audio"), QJsonValue::Null);
    }
    return o;
}

MediaInfo probe(const QString& path, const ProbeOptions& opts) {
    MediaInfo m;
    av::Deadline dl(opts.timeoutMs, opts.cancel);
    QString err;
    std::unique_ptr<av::Input> in = av::openFile(path, &dl, &err);
    if (!in) { m.error = err; return m; }
    AVFormatContext* fc = in->fc();
    m.format = in->formatName();
    m.formatLongName = str(fc->iformat->long_name);
    m.sizeBytes = in->size();
    m.bitRate = fc->bit_rate > 0 ? fc->bit_rate : 0;
    m.streams = int(fc->nb_streams);
    m.isImage = m.format.endsWith(QLatin1String("_pipe")) || m.format == QLatin1String("image2pipe");

    const int vs = bestStream(fc, AVMEDIA_TYPE_VIDEO);
    if (vs >= 0) {
        AVStream* st = fc->streams[vs];
        const AVCodecParameters* par = st->codecpar;
        VideoInfo& v = m.video;
        v.present = true;
        v.index = vs;
        v.codec = str(avcodec_get_name(par->codec_id));
        v.width = par->width;
        v.height = par->height;
        const AVRational sar = av_guess_sample_aspect_ratio(fc, st, nullptr);
        v.displayWidth = par->width > 0 ? evenAtLeast2(par->width * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0)) : 0;
        const AVRational fr = av_guess_frame_rate(fc, st, nullptr);
        v.fps = !m.isImage && fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
        v.pixFmt = str(av_get_pix_fmt_name(AVPixelFormat(par->format)));
        v.rotation = rotationOf(par);
        v.frames = st->nb_frames > 0 ? st->nb_frames : 0;
        v.bitRate = par->bit_rate > 0 ? par->bit_rate : 0;
    }
    const int as = bestStream(fc, AVMEDIA_TYPE_AUDIO);
    if (as >= 0) {
        const AVCodecParameters* par = fc->streams[as]->codecpar;
        AudioInfo& a = m.audio;
        a.present = true;
        a.index = as;
        a.codec = str(avcodec_get_name(par->codec_id));
        a.sampleRate = par->sample_rate;
        a.channels = par->ch_layout.nb_channels;
        if (par->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
            char b[128] = {0};
            if (av_channel_layout_describe(&par->ch_layout, b, sizeof(b)) > 0) a.layout = QString::fromLatin1(b);
        }
        a.sampleFmt = str(av_get_sample_fmt_name(AVSampleFormat(par->format)));
        a.bitRate = par->bit_rate > 0 ? par->bit_rate : 0;
    }

    if (fc->duration != AV_NOPTS_VALUE && fc->duration > 0) m.durationSec = double(fc->duration) / AV_TIME_BASE;
    else if (vs >= 0 && streamSeconds(fc->streams[vs]) > 0) m.durationSec = streamSeconds(fc->streams[vs]);
    else if (as >= 0) m.durationSec = streamSeconds(fc->streams[as]);
    if (m.isImage) m.durationSec = 0;
    if (opts.exactDuration && !m.isImage) {
        const double d = scanDuration(fc, dl, &err);
        if (!err.isEmpty()) { m.error = err; return m; }
        if (d > 0) { m.durationSec = d; m.durationExact = true; }
    }
    m.ok = true;
    return m;
}

QFuture<MediaInfo> probeAsync(const QString& path, const ProbeOptions& opts, QThreadPool* pool) {
    return QtConcurrent::run(pool ? pool : QThreadPool::globalInstance(), [path, opts] { return probe(path, opts); });
}

} // namespace studio::media
