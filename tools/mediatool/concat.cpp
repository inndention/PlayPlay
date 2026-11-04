// PlayPlayMediaTool graph runner: the stream-copy remux and the shared output writer (concat.h, DEP-05b).
#include "concat.h"
#include "ppcli.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

namespace mediatool {
namespace detail {

QString avErr(int r) { char b[AV_ERROR_MAX_STRING_SIZE] = {0}; av_strerror(r, b, sizeof(b)); return QString::fromUtf8(b); }

bool Interrupt::stop() {
    if (timedOut || cancelled) return true;
    if (cancel && *cancel && (*cancel)()) cancelled = true;
    else if (timeoutMs > 0 && clock.isValid() && clock.elapsed() >= timeoutMs) timedOut = true;
    return timedOut || cancelled;
}
int Interrupt::callback(void* p) { return static_cast<Interrupt*>(p)->stop() ? 1 : 0; }

namespace {
int64_t mapScore(const AVStream* st) {
    return 100000000LL * !!(st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS) + 5000000LL * !!(st->disposition & AV_DISPOSITION_DEFAULT);
}
}

int bestVideoStream(const AVFormatContext* fc) {
    int best = -1; int64_t bestScore = 0;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        int64_t score = int64_t(st->codecpar->width) * st->codecpar->height + mapScore(st);
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) score = 1;
        if (score > bestScore) { bestScore = score; best = int(i); }
    }
    return best;
}

int bestAudioStream(const AVFormatContext* fc) {
    int best = -1; int64_t bestScore = 0;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const int64_t score = st->codecpar->ch_layout.nb_channels + mapScore(st);
        if (score > bestScore) { bestScore = score; best = int(i); }
    }
    return best;
}

int openInput(const GraphInput& in, bool localOnly, Interrupt* irq, AVFormatContext** fc) {
    AVFormatContext* c = avformat_alloc_context();
    if (!c) return AVERROR(ENOMEM);
    c->interrupt_callback.callback = Interrupt::callback;
    c->interrupt_callback.opaque = irq;
    const AVInputFormat* fmt = nullptr;
    AVDictionary* fo = nullptr;
    av_dict_set(&fo, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    if (localOnly) av_dict_set(&fo, "protocol_whitelist", "file", 0);
    if (in.concat) { fmt = av_find_input_format("concat"); if (in.safe0) av_dict_set(&fo, "safe", "0", 0); }
    else if (in.loop || in.framerate > 0 || in.path.contains('%')) {
        // -loop and -framerate are image2 options; ffmpeg hands them to whatever demuxer probing picks (image2 for a
        // sequence or a .jpg, png_pipe for a lone .png, both take them). Naming image2 here keeps that one demuxer for all.
        fmt = av_find_input_format("image2");
        if (in.loop) av_dict_set(&fo, "loop", "1", 0);
        if (in.framerate > 0) av_dict_set(&fo, "framerate", QString::number(in.framerate, 'f', 6).toUtf8().constData(), 0);
    }
    int err = avformat_open_input(&c, in.path.toUtf8().constData(), fmt, &fo);   // frees the context on failure
    if (err >= 0) {
        av_dict_set(&fo, "scan_all_pmts", nullptr, 0);          // as ffmpeg: its own addition, never reported
        if (const AVDictionaryEntry* e = av_dict_iterate(fo, nullptr)) av_log(nullptr, AV_LOG_WARNING, "%s: option %s not used by the demuxer\n", in.path.toUtf8().constData(), e->key);
    }
    av_dict_free(&fo);
    if (err < 0) return err;
    if ((err = avformat_find_stream_info(c, nullptr)) < 0) { avformat_close_input(&c); return err; }
    *fc = c;
    return 0;
}

int64_t seekInput(AVFormatContext* fc, double seekSec, const QString& path) {
    int64_t ts = std::llround(seekSec * AV_TIME_BASE);
    if (fc->start_time != AV_NOPTS_VALUE) ts += fc->start_time;
    int64_t seekTs = ts;
    if (!(fc->iformat->flags & AVFMT_SEEK_TO_PTS)) {
        for (unsigned i = 0; i < fc->nb_streams; ++i)
            if (fc->streams[i]->codecpar->video_delay) { seekTs -= 3 * AV_TIME_BASE / 23; break; }   // ffmpeg_demux.c's dts heuristic
    }
    if (avformat_seek_file(fc, -1, INT64_MIN, seekTs, seekTs, 0) < 0)
        av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %.3f\n", path.toUtf8().constData(), double(ts) / AV_TIME_BASE);
    return ts;
}

}   // namespace detail

using detail::avErr;

// ---- MuxWriter --------------------------------------------------------------------------------------------------------
MuxWriter::MuxWriter() = default;
MuxWriter::~MuxWriter() {
    if (m_oc) {
        if (m_oc->pb && !(m_oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&m_oc->pb);
        avformat_free_context(m_oc);
    }
}

bool MuxWriter::open(const GraphOutput& out, bool overwrite, bool noOverwrite, QString* err, int* averror) {
    if (!overwrite && QFileInfo::exists(out.path)) {
        // ffmpeg -n, or no answer possible with -nostdin: "File 'x' already exists. Exiting."
        *err = QString("File '%1' already exists. Exiting.").arg(out.path); *averror = AVERROR(EEXIST);
        Q_UNUSED(noOverwrite); return false;
    }
    const char* fmt = out.container == "mp4" ? "mp4" : out.container == "mov" ? "mov" : out.container == "wav" ? "wav" : "image2";
    int r = avformat_alloc_output_context2(&m_oc, nullptr, fmt, out.path.toUtf8().constData());
    if (r < 0 || !m_oc) { *err = "cannot allocate the " + out.container + " muxer: " + avErr(r); *averror = r; return false; }
    if (!(m_oc->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open2(&m_oc->pb, out.path.toUtf8().constData(), AVIO_FLAG_WRITE, nullptr, nullptr);
        if (r < 0) { *err = QString("%1: %2").arg(out.path, avErr(r)); *averror = r; return false; }
    }
    if (out.faststart) av_opt_set(m_oc->priv_data, "movflags", "+faststart", 0);
    if (out.makeZeroTs) m_oc->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    return true;
}

AVStream* MuxWriter::addStream(const AVRational& timeBase) {
    AVStream* st = avformat_new_stream(m_oc, nullptr);
    if (!st) return nullptr;
    st->time_base = timeBase;
    m_packets << 0; m_lastDts << AV_NOPTS_VALUE;
    return st;
}

bool MuxWriter::writeHeader(QString* err, int* averror) {
    const int r = avformat_write_header(m_oc, nullptr);
    if (r < 0) { *err = "Could not write header: " + avErr(r); *averror = r; return false; }
    m_headerWritten = true;
    return true;
}

bool MuxWriter::writePacket(AVPacket* pkt, int streamIndex, const AVRational& timeBase, QString* err, int* averror) {
    AVStream* st = m_oc->streams[streamIndex];
    av_packet_rescale_ts(pkt, timeBase, st->time_base);
    pkt->stream_index = streamIndex;
    // fftools/ffmpeg_mux.c write_packet: what ffmpeg does to timestamps before the muxer sees them (a concat of segments
    // whose audio carries encoder priming overlaps by a packet, for instance)
    if (!(m_oc->oformat->flags & AVFMT_NOTIMESTAMPS)) {
        int64_t& last = m_lastDts[streamIndex];
        if (pkt->dts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE && pkt->dts > pkt->pts) {
            av_log(nullptr, AV_LOG_WARNING, "Invalid DTS/PTS combination in output stream 0:%d: dts %lld, pts %lld\n", streamIndex, (long long)pkt->dts, (long long)pkt->pts);
            pkt->pts = pkt->dts = pkt->pts + pkt->dts + last + 1 - std::min({pkt->pts, pkt->dts, last + 1}) - std::max({pkt->pts, pkt->dts, last + 1});
        }
        const AVMediaType t = st->codecpar->codec_type;
        if ((t == AVMEDIA_TYPE_AUDIO || t == AVMEDIA_TYPE_VIDEO || t == AVMEDIA_TYPE_SUBTITLE) && pkt->dts != AV_NOPTS_VALUE && last != AV_NOPTS_VALUE) {
            const int64_t max = last + !(m_oc->oformat->flags & AVFMT_TS_NONSTRICT);
            if (pkt->dts < max) {
                av_log(nullptr, max - pkt->dts > 2 || t == AVMEDIA_TYPE_VIDEO ? AV_LOG_WARNING : AV_LOG_DEBUG,
                       "Non-monotonic DTS in output stream 0:%d; previous: %lld, current: %lld; changing to %lld. This may result in incorrect timestamps in the output file.\n",
                       streamIndex, (long long)last, (long long)pkt->dts, (long long)max);
                if (pkt->pts >= pkt->dts) pkt->pts = std::max(pkt->pts, max);
                pkt->dts = max;
            }
        }
        if (pkt->dts != AV_NOPTS_VALUE) last = pkt->dts;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
        const int64_t endUs = av_rescale_q(pkt->pts + std::max<int64_t>(0, pkt->duration), st->time_base, AV_TIME_BASE_Q);
        m_outTimeUs = std::max(m_outTimeUs, endUs);
    }
    const int r = av_interleaved_write_frame(m_oc, pkt);
    if (r < 0) { *err = "Error muxing a packet: " + avErr(r); *averror = r; return false; }
    if (streamIndex < m_packets.size()) ++m_packets[streamIndex];
    return true;
}

bool MuxWriter::finish(QString* err, int* averror) {
    if (m_finished) return true;
    m_finished = true;
    if (m_headerWritten) {
        const int r = av_write_trailer(m_oc);
        if (r < 0) { *err = "Error writing the trailer: " + avErr(r); *averror = r; return false; }
    }
    if (m_oc->pb && !(m_oc->oformat->flags & AVFMT_NOFILE)) {
        avio_flush(m_oc->pb);
        avio_closep(&m_oc->pb);
    }
    m_finalBytes = QFileInfo(QString::fromUtf8(m_oc->url ? m_oc->url : "")).size();
    return true;
}

qint64 MuxWriter::bytesWritten() const {
    if (m_finished) return m_finalBytes;
    return m_oc && m_oc->pb ? qint64(avio_tell(m_oc->pb)) : 0;
}

// ---- the remux ---------------------------------------------------------------------------------------------------------
bool isCopyOnly(const GraphCommand& c) {
    if (!c.filterComplex.isEmpty() || c.outputs.size() != 1) return false;
    const GraphOutput& o = c.outputs.first();
    if (!o.vf.isEmpty() || !o.af.isEmpty()) return false;
    const QString v = o.videoEncoder(), a = o.audioEncoder();
    return (v.isEmpty() || v == "copy") && (a.isEmpty() || a == "copy") && !(v.isEmpty() && a.isEmpty());
}

namespace {

struct FmtCtx { AVFormatContext* p = nullptr; ~FmtCtx() { if (p) avformat_close_input(&p); } };
struct Packet { AVPacket* p = av_packet_alloc(); ~Packet() { av_packet_free(&p); } };

// ffmpeg_mux_init.c streamcopy_init: the input's tag stays when the container has no tag table, maps this tag to the
// same codec, or has no tag of its own for the codec; otherwise 0 and the muxer picks.
unsigned copyTag(const AVOutputFormat* of, const AVCodecParameters* par) {
    unsigned tmp = 0;
    if (!of->codec_tag || av_codec_get_id(of->codec_tag, par->codec_tag) == par->codec_id || !av_codec_get_tag2(of->codec_tag, par->codec_id, &tmp))
        return par->codec_tag;
    return 0;
}

QString describe(const AVCodecParameters* p) {
    const QString name = QString::fromLatin1(avcodec_get_name(p->codec_id));
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) return QString("video %1 %2x%3 (copy)").arg(name).arg(p->width).arg(p->height);
    char lay[64] = {0}; av_channel_layout_describe(&p->ch_layout, lay, sizeof(lay));
    return QString("audio %1 %2 %3 (copy)").arg(name).arg(p->sample_rate).arg(QString::fromLatin1(lay));
}

}   // namespace

RunResult remuxCopy(const GraphCommand& c, const RunOptions& o) {
    RunResult r; QElapsedTimer clock; clock.start();
    auto fail = [&](int err, const QString& what) { r.ok = false; r.error = what + (err ? ": " + avErr(err) : QString()); r.averror = err; r.elapsedSec = clock.elapsed() / 1000.0; return r; };
    if (!c.ok()) return fail(AVERROR(EINVAL), c.unsupported.isEmpty() ? c.error : c.unsupported);
    if (c.inputs.size() != 1 || c.outputs.size() != 1) return fail(AVERROR(EINVAL), "the remux takes one input and one output");
    const GraphInput& in = c.inputs.first(); const GraphOutput& out = c.outputs.first();
    r.out = out.path;

    detail::Interrupt irq; irq.clock.start(); irq.timeoutMs = o.timeoutMs; irq.cancel = &o.cancelled;
    auto stopped = [&]() { r.timedOut = irq.timedOut; r.cancelled = irq.cancelled; return fail(AVERROR_EXIT, irq.timedOut ? "timed out" : "cancelled"); };

    FmtCtx fc;
    int err = detail::openInput(in, o.localOnly, &irq, &fc.p);
    if (err < 0) return fail(err, in.path);
    if (irq.stop()) return stopped();

    // ffmpeg's automatic mapping: one video, one audio (a subtitle stream the container takes is not copied here)
    int vs = out.videoEncoder().isEmpty() ? -1 : detail::bestVideoStream(fc.p);
    int as = out.audioEncoder().isEmpty() ? -1 : detail::bestAudioStream(fc.p);
    if (vs < 0 && as < 0) return fail(AVERROR_STREAM_NOT_FOUND, "Output file does not contain any stream");
    for (unsigned i = 0; i < fc.p->nb_streams; ++i) if (int(i) != vs && int(i) != as) fc.p->streams[i]->discard = AVDISCARD_ALL;

    // -ss: the keyframe before t; timestamps rebased so t (or the file's start) is 0
    int64_t startUs = fc.p->start_time != AV_NOPTS_VALUE ? fc.p->start_time : 0;
    if (in.seekSec >= 0) startUs = detail::seekInput(fc.p, in.seekSec, in.path);
    const int64_t limitUs = in.durationSec >= 0 ? std::llround(in.durationSec * AV_TIME_BASE) : INT64_MAX;

    MuxWriter mux;
    QString e; int averr = 0;
    if (!mux.open(out, c.overwrite, c.noOverwrite, &e, &averr)) return fail(averr, e);
    QVector<int> outIndex(int(fc.p->nb_streams), -1);
    for (int idx : {vs, as}) {
        if (idx < 0) continue;
        const AVStream* ist = fc.p->streams[idx];
        AVStream* ost = mux.addStream(ist->time_base);
        if (!ost) return fail(AVERROR(ENOMEM), "cannot add an output stream");
        if ((err = avcodec_parameters_copy(ost->codecpar, ist->codecpar)) < 0) return fail(err, "cannot copy the stream parameters");
        ost->codecpar->codec_tag = copyTag(mux.context()->oformat, ist->codecpar);
        ost->avg_frame_rate = ist->avg_frame_rate;
        ost->r_frame_rate = ist->r_frame_rate;
        ost->sample_aspect_ratio = ist->sample_aspect_ratio;
        ost->disposition = ist->disposition;
        av_dict_copy(&ost->metadata, ist->metadata, AV_DICT_DONT_OVERWRITE);   // ffmpeg keeps the stream tags (the mp4 compressorname among them)
        outIndex[idx] = ost->index;
        r.streams << describe(ist->codecpar);
    }
    if (!mux.writeHeader(&e, &averr)) return fail(averr, e);

    Packet pkt;
    QVector<bool> done(int(fc.p->nb_streams), false);
    QElapsedTimer progressClock; progressClock.start();
    auto report = [&](bool end) {
        if (!o.progress) return;
        const double el = std::max(0.001, clock.elapsed() / 1000.0);
        const qint64 outUs = mux.outTimeUs();
        const double bitrate = outUs > 0 ? mux.bytesWritten() * 8.0 / (outUs / 1e6) / 1000.0 : -1;
        o.progress(progressBlock(vs >= 0 ? mux.packets(outIndex[vs]) : 0, vs >= 0 ? mux.packets(outIndex[vs]) / el : 0, bitrate, mux.bytesWritten(), outUs, 0, 0, outUs > 0 ? (outUs / 1e6) / el : 0, end));
        progressClock.restart();
    };
    for (;;) {
        if (irq.stop()) return stopped();
        err = av_read_frame(fc.p, pkt.p);
        if (err == AVERROR_EOF) break;
        if (err < 0) return fail(err, in.path);
        const int idx = pkt.p->stream_index;
        if (idx < 0 || idx >= outIndex.size() || outIndex[idx] < 0) { av_packet_unref(pkt.p); continue; }
        const AVStream* ist = fc.p->streams[idx];
        // rebase: ffmpeg's ts_offset = -(start_time + ss); the input -t ends the input at the first packet at or past it
        const int64_t off = av_rescale_q(startUs, AV_TIME_BASE_Q, ist->time_base);
        if (pkt.p->pts != AV_NOPTS_VALUE) pkt.p->pts -= off;
        if (pkt.p->dts != AV_NOPTS_VALUE) pkt.p->dts -= off;
        if (limitUs != INT64_MAX) {
            // ffmpeg_demux.c: each stream ends at its first packet with dts at or past -t (not written); the input when all did
            if (done[idx]) { av_packet_unref(pkt.p); continue; }
            const int64_t dts = pkt.p->dts != AV_NOPTS_VALUE ? pkt.p->dts : pkt.p->pts;
            if (dts != AV_NOPTS_VALUE && av_compare_ts(dts, ist->time_base, limitUs, AV_TIME_BASE_Q) >= 0) {
                av_packet_unref(pkt.p); done[idx] = true;
                bool all = true; for (int k : {vs, as}) if (k >= 0 && !done[k]) all = false;
                if (all) break;
                continue;
            }
        }
        if (!mux.writePacket(pkt.p, outIndex[idx], ist->time_base, &e, &averr)) { av_packet_unref(pkt.p); return fail(averr, e); }
        av_packet_unref(pkt.p);
        if (o.progress && progressClock.elapsed() >= o.progressIntervalMs) report(false);
    }
    if (!mux.finish(&e, &averr)) return fail(averr, e);
    report(true);
    r.ok = true;
    r.durationSec = mux.durationSec();
    r.videoFrames = vs >= 0 ? mux.packets(outIndex[vs]) : 0;
    r.bytes = mux.bytesWritten();
    r.elapsedSec = clock.elapsed() / 1000.0;
    return r;
}

} // namespace mediatool
