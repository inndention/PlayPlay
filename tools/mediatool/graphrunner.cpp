// PlayPlayMediaTool graph runner: see graphrunner.h for the contract and the ffmpeg behaviour it follows (DEP-05b).
#include "graphrunner.h"
#include "concat.h"
#include "ppcli.h"
#include "probecli.h"      // oneShotTimeoutMs is not used here: a render has no default deadline, like ffmpeg

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
}

namespace mediatool {

using detail::avErr;

QByteArray progressBlock(qint64 frames, double fps, double bitrateKbps, qint64 totalSize, qint64 outTimeUs, int dup, int drop, double speed, bool end) {
    // fftools/ffmpeg_mux.c print_report with -progress: one key=value per line, "progress=" last
    QByteArray b;
    b += "frame=" + QByteArray::number(frames) + '\n';
    b += "fps=" + QByteArray::number(fps, 'f', 2) + '\n';
    b += "bitrate=" + (bitrateKbps < 0 ? QByteArray("N/A") : QByteArray::number(bitrateKbps, 'f', 1) + "kbits/s") + '\n';
    b += "total_size=" + QByteArray::number(totalSize) + '\n';
    if (outTimeUs < 0) { b += "out_time_us=N/A\nout_time_ms=N/A\nout_time=N/A\n"; }
    else {
        b += "out_time_us=" + QByteArray::number(outTimeUs) + '\n';
        b += "out_time_ms=" + QByteArray::number(outTimeUs) + '\n';      // ffmpeg prints microseconds under both names
        const qint64 s = outTimeUs / 1000000, us = outTimeUs % 1000000;
        b += QString("out_time=%1:%2:%3.%4\n").arg(s / 3600, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')).arg(us, 6, 10, QChar('0')).toLatin1();
    }
    b += "dup_frames=" + QByteArray::number(dup) + '\n';
    b += "drop_frames=" + QByteArray::number(drop) + '\n';
    b += "speed=" + (speed <= 0 ? QByteArray("N/A") : QByteArray::number(speed, 'f', 3) + 'x') + '\n';
    b += end ? "progress=end\n" : "progress=continue\n";
    return b;
}

namespace {

struct Frame { AVFrame* p = av_frame_alloc(); ~Frame() { av_frame_free(&p); } };
struct Packet { AVPacket* p = av_packet_alloc(); ~Packet() { av_packet_free(&p); } };

// ---- autorotate (fftools/ffmpeg_filter.c configure_input_video_filter, cmdutils.c get_rotation): the display matrix of
// the first decoded frame (decoders attach the stream's, the JPEG decoder an EXIF orientation), else the stream's.
QStringList rotationFilters(const AVFrame* f, const AVStream* st) {
    const int32_t* m = nullptr;
    if (const AVFrameSideData* sd = av_frame_side_data_get(f->side_data, f->nb_side_data, AV_FRAME_DATA_DISPLAYMATRIX); sd && sd->size >= 9 * sizeof(int32_t))
        m = reinterpret_cast<const int32_t*>(sd->data);
    else if (const AVPacketSideData* pd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX); pd && pd->size >= 9 * sizeof(int32_t))
        m = reinterpret_cast<const int32_t*>(pd->data);
    if (!m) return {};
    double theta = -std::round(av_display_rotation_get(m));
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    QStringList out;
    if (!std::isfinite(theta)) return out;
    if (std::fabs(theta - 90) < 1.0) out << (m[3] > 0 ? "transpose=cclock_flip" : "transpose=clock");
    else if (std::fabs(theta - 180) < 1.0) { if (m[0] < 0) out << "hflip"; if (m[4] < 0) out << "vflip"; }
    else if (std::fabs(theta - 270) < 1.0) out << (m[3] < 0 ? "transpose=clock_flip" : "transpose=cclock");
    else if (std::fabs(theta) > 1.0) out << "rotate=" + QString::number(theta * 3.14159265358979323846 / 180.0, 'f', 6);
    else if (m[4] < 0) out << "vflip";
    return out;
}

struct InStream {
    int index = -1;
    AVStream* st = nullptr;
    AVCodecContext* dec = nullptr;
    QVector<AVFilterContext*> srcs;       // one buffersrc per use of this stream in the graph
    QVector<bool> srcClosed;              // the graph no longer takes frames there (trim finished)
    int copyTo = -1;                      // output stream index when the packets are copied
    bool eofSent = false;                 // EOF pushed into the srcs
    bool copyDone = false;                // the copy stream reached the output -t (or the input -t)
    bool inputDone = false;               // the input -t: a packet with dts at or past it was seen (ffmpeg_demux.c)
    int64_t trimEnd = AV_NOPTS_VALUE;     // the input -t in the stream time base: frames at or past it are dropped / cut
    QVector<AVFrame*> pending;            // decoded before the graph existed
    int64_t lastEnd = AV_NOPTS_VALUE;     // last packet pts + duration (stream_loop offset), stream time base
    int64_t loopOffset = 0;               // stream time base
    int64_t trimStart = AV_NOPTS_VALUE;   // -ss: frames before this pts (after rebasing: 0) are dropped / cut
    bool open() const { for (bool c : srcClosed) if (!c) return true; return false; }
    AVMediaType type() const { return st->codecpar->codec_type; }
};

struct Input {
    const GraphInput* spec = nullptr;
    int index = -1;
    AVFormatContext* fc = nullptr;
    QVector<InStream> streams;            // the used ones only
    bool eof = false;                     // nothing more to read (and the loop, if any, is over)
    int64_t startUs = 0;                  // rebase: start_time (+ -ss)
    int64_t limitUs = INT64_MAX;          // -t
    QVector<AVPacket*> pendingCopy;       // copy packets read before the header
    ~Input() {
        for (InStream& s : streams) { avcodec_free_context(&s.dec); for (AVFrame* f : s.pending) av_frame_free(&f); }
        for (AVPacket* p : pendingCopy) av_packet_free(&p);
        if (fc) avformat_close_input(&fc);
    }
    InStream* stream(int idx) { for (InStream& s : streams) if (s.index == idx) return &s; return nullptr; }
    bool allClosed() const { for (const InStream& s : streams) if (s.copyTo < 0 ? s.open() : !s.copyDone) return false; return true; }
    bool allInputDone() const { for (const InStream& s : streams) if (!s.inputDone) return false; return true; }
};

struct OutStream {
    AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
    AVFilterContext* sink = nullptr;
    AVCodecContext* enc = nullptr;
    int index = -1;                       // muxer stream index
    AVRational tb{1, 1};                  // packets come in this time base (the encoder's, or the copied stream's)
    bool copy = false;
    bool sinkEof = false, flushed = false;
    // video sync (ffmpeg_filter.c video_sync_process)
    bool cfr = true;
    int64_t nextPts = 0;
    AVFrame* lastFrame = nullptr;
    qint64 framesOut = 0;                 // frames sent to the encoder
    qint64 samplesOut = 0;
    int dup = 0, drop = 0;
    int frameLimit = -1;                  // -frames:v
    ~OutStream() { avcodec_free_context(&enc); av_frame_free(&lastFrame); }
    bool done() const { return copy || flushed; }
};

class Runner {
public:
    Runner(const GraphCommand& c, const RunOptions& o) : m_c(c), m_o(o), m_out(c.outputs.first()) {}
    ~Runner() {
        for (Input* in : m_inputs) delete in;
        for (OutStream* os : m_outs) delete os;
        avfilter_inout_free(&m_graphIns); avfilter_inout_free(&m_graphOuts);
        avfilter_graph_free(&m_graph);
    }
    RunResult run();

private:
    const GraphCommand& m_c;
    const RunOptions& m_o;
    const GraphOutput& m_out;
    RunResult m_r;
    detail::Interrupt m_irq;
    QElapsedTimer m_clock, m_progressClock;
    QVector<Input*> m_inputs;
    QVector<OutStream*> m_outs;
    AVFilterGraph* m_graph = nullptr;
    AVFilterInOut* m_graphIns = nullptr;  // the open input pads of the user's graph
    AVFilterInOut* m_graphOuts = nullptr; // its open output pads (labelled)
    bool m_graphReady = false;
    MuxWriter m_mux;
    bool m_headerWritten = false;
    Packet m_pkt; Frame m_frame;
    QString m_err; int m_averr = 0;

    bool fail(int err, const QString& what) {
        if (!m_r.error.isEmpty()) return false;
        m_r.error = what + (err && err != AVERROR_EXIT ? ": " + avErr(err) : QString()); m_r.averror = err; return false;
    }
    bool stopped() { if (!m_irq.stop()) return false; m_r.timedOut = m_irq.timedOut; m_r.cancelled = m_irq.cancelled; return fail(AVERROR_EXIT, m_irq.timedOut ? "timed out" : "cancelled"); }

    bool openInputs();
    bool parseComplexGraph();
    bool resolveStreams();
    bool openDecoders();
    bool firstFrames();
    bool buildGraph();
    bool openEncoders();
    bool openOutput();
    bool feedLoop();
    bool finish();

    bool readOne(Input& in);                                     // one packet (or EOF handling) of an input
    bool handlePacket(Input& in, InStream& s, AVPacket* pkt);    // rebased packet: copy or decode
    bool decode(Input& in, InStream& s, AVPacket* pkt);          // pkt null = drain
    bool deliver(Input& in, InStream& s, AVFrame* f);            // a decoded frame: trim, queue or push
    bool push(InStream& s, AVFrame* f);                          // into every open src
    bool inputEof(Input& in);
    bool reapSinks();
    bool encodeFrame(OutStream& os, AVFrame* f);                 // null = flush
    bool videoOut(OutStream& os, AVFrame* f);
    bool writeCopy(Input& in, InStream& s, AVPacket* pkt);
    void report(bool end);
    AVFilterContext* makeFilter(const char* name, const QString& args, const QString& instance);
    bool chainFilters(const QStringList& specs, AVFilterContext** last, int* lastPad, const QString& tag);
    bool attachSink(OutStream& os, AVFilterContext* from, int pad);
    InStream* streamForLabel(const QString& label, AVMediaType padType, QString* why);
};

// ---- inputs ----------------------------------------------------------------------------------------------------------
bool Runner::openInputs() {
    for (int i = 0; i < m_c.inputs.size(); ++i) {
        Input* in = new Input; in->spec = &m_c.inputs[i]; in->index = i;
        m_inputs << in;
        const int err = detail::openInput(*in->spec, m_o.localOnly, &m_irq, &in->fc);
        if (err < 0) return fail(err, in->spec->path);
        if (stopped()) return false;
        in->startUs = in->fc->start_time != AV_NOPTS_VALUE ? in->fc->start_time : 0;
        if (in->spec->seekSec >= 0) in->startUs = detail::seekInput(in->fc, in->spec->seekSec, in->spec->path);
        if (in->spec->durationSec >= 0) in->limitUs = std::llround(in->spec->durationSec * AV_TIME_BASE);
        for (unsigned k = 0; k < in->fc->nb_streams; ++k) in->fc->streams[k]->discard = AVDISCARD_ALL;   // used streams re-enabled below
    }
    return true;
}

bool Runner::parseComplexGraph() {
    m_graph = avfilter_graph_alloc();
    if (!m_graph) return fail(AVERROR(ENOMEM), "cannot allocate the filtergraph");
    if (m_c.filterComplex.isEmpty()) return true;
    const int err = avfilter_graph_parse_ptr(m_graph, m_c.filterComplex.toUtf8().constData(), &m_graphIns, &m_graphOuts, nullptr);
    if (err < 0) return fail(err, "-filter_complex");
    return true;
}

// The stream a graph label stands for: [k:v] / [k:a] the first stream of that type in input k, [k] the first stream of
// the pad's type (ffmpeg_filter.c fg_complex_bind_input). The automatic mapping of -vf / -af uses bestVideoStream /
// bestAudioStream instead (see resolveStreams).
InStream* Runner::streamForLabel(const QString& label, AVMediaType padType, QString* why) {
    const int colon = label.indexOf(':');
    bool ok = false; const int k = label.left(colon < 0 ? label.size() : colon).toInt(&ok);
    if (!ok || k < 0 || k >= m_inputs.size()) { *why = "Invalid file index " + label; return nullptr; }
    AVMediaType type = padType;
    if (colon >= 0) type = label.mid(colon + 1) == "a" ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
    Input& in = *m_inputs[k];
    for (unsigned i = 0; i < in.fc->nb_streams; ++i) {
        if (in.fc->streams[i]->codecpar->codec_type != type) continue;
        if (InStream* s = in.stream(int(i))) return s;
        InStream s; s.index = int(i); s.st = in.fc->streams[i];
        in.streams << s; return &in.streams.last();
    }
    *why = QString("Stream specifier ':%1' in filtergraph description matches no streams of input %2").arg(type == AVMEDIA_TYPE_AUDIO ? "a" : "v").arg(k);
    return nullptr;
}

bool Runner::resolveStreams() {
    QString why;
    if (!m_c.filterComplex.isEmpty()) {
        for (AVFilterInOut* io = m_graphIns; io; io = io->next) {
            const AVMediaType t = avfilter_pad_get_type(io->filter_ctx->input_pads, io->pad_idx);
            InStream* s = streamForLabel(QString::fromUtf8(io->name ? io->name : ""), t, &why);
            if (!s) return fail(AVERROR(EINVAL), why);
            s->srcs << nullptr; s->srcClosed << false;     // created in buildGraph, once the first frame is known
        }
        if (m_graphIns == nullptr) return fail(AVERROR(EINVAL), "-filter_complex reads no input");
        // every mapped label must be an open output of the graph
        for (const QString& m : m_out.maps) {
            bool found = false;
            for (AVFilterInOut* io = m_graphOuts; io; io = io->next) if (io->name && m == QString::fromUtf8(io->name)) found = true;
            if (!found) return fail(AVERROR(EINVAL), "Output with label '" + m + "' does not exist in any defined filter graph, or was already used elsewhere.");
        }
        return true;
    }
    // the simple graph: ffmpeg's automatic mapping across the inputs, best picture / most channels
    const bool wantV = !m_out.videoEncoder().isEmpty(), wantA = !m_out.audioEncoder().isEmpty();
    Input* bestVIn = nullptr; int bestV = -1; int64_t bestVScore = 0;
    Input* bestAIn = nullptr; int bestA = -1; int64_t bestAScore = 0;
    for (Input* in : m_inputs) {
        if (wantV) { const int v = detail::bestVideoStream(in->fc); if (v >= 0) { const int64_t sc = int64_t(in->fc->streams[v]->codecpar->width) * in->fc->streams[v]->codecpar->height; if (!bestVIn || sc > bestVScore) { bestVIn = in; bestV = v; bestVScore = sc; } } }
        if (wantA) { const int a = detail::bestAudioStream(in->fc); if (a >= 0) { const int64_t sc = in->fc->streams[a]->codecpar->ch_layout.nb_channels; if (!bestAIn || sc > bestAScore) { bestAIn = in; bestA = a; bestAScore = sc; } } }
    }
    if (!bestVIn && !bestAIn) return fail(AVERROR_STREAM_NOT_FOUND, "Output file does not contain any stream");
    auto add = [](Input* in, int idx, bool copy) { InStream s; s.index = idx; s.st = in->fc->streams[idx]; if (copy) s.copyTo = 0; else { s.srcs << nullptr; s.srcClosed << false; } in->streams << s; };
    if (bestVIn) add(bestVIn, bestV, m_out.videoEncoder() == "copy");
    if (bestAIn) add(bestAIn, bestA, m_out.audioEncoder() == "copy");
    return true;
}

bool Runner::openDecoders() {
    for (Input* in : m_inputs) {
        for (InStream& s : in->streams) {
            s.st->discard = AVDISCARD_DEFAULT;
            if (s.copyTo >= 0) continue;
            const AVCodec* codec = avcodec_find_decoder(s.st->codecpar->codec_id);
            if (!codec) return fail(AVERROR_DECODER_NOT_FOUND, QString("no decoder for %1 in %2").arg(QString::fromLatin1(avcodec_get_name(s.st->codecpar->codec_id)), in->spec->path));
            s.dec = avcodec_alloc_context3(codec);
            int err = s.dec ? avcodec_parameters_to_context(s.dec, s.st->codecpar) : AVERROR(ENOMEM);
            if (err < 0) return fail(err, "decoder for " + in->spec->path);
            s.dec->pkt_timebase = s.st->time_base;
            s.dec->thread_count = std::max(1, m_o.decoderThreads);
            if ((err = avcodec_open2(s.dec, codec, nullptr)) < 0) return fail(err, "cannot open the decoder for " + in->spec->path);
            if (in->spec->seekSec >= 0) s.trimStart = 0;
            if (in->limitUs != INT64_MAX) s.trimEnd = av_rescale_q(in->limitUs, AV_TIME_BASE_Q, s.st->time_base);
        }
    }
    return true;
}

// ffmpeg configures the graph from the first decoded frame of every input: read each input until all its decoded
// streams delivered one (copy packets and extra frames wait in the queues).
bool Runner::firstFrames() {
    for (Input* in : m_inputs) {
        auto need = [&]() { for (const InStream& s : in->streams) if (s.copyTo < 0 && s.pending.isEmpty() && !s.eofSent) return true; return false; };
        while (need()) {
            if (stopped()) return false;
            if (in->eof) break;
            if (!readOne(*in)) return false;
        }
        for (const InStream& s : in->streams)
            if (s.copyTo < 0 && s.pending.isEmpty()) return fail(AVERROR_INVALIDDATA, QString("Cannot determine format of input stream %1:%2 after EOF").arg(in->index).arg(s.index));
    }
    return true;
}

// ---- the graph ---------------------------------------------------------------------------------------------------------
AVFilterContext* Runner::makeFilter(const char* name, const QString& args, const QString& instance) {
    const AVFilter* f = avfilter_get_by_name(name);
    if (!f) { fail(AVERROR_FILTER_NOT_FOUND, QString("filter '%1'").arg(name)); return nullptr; }
    AVFilterContext* ctx = nullptr;
    const int err = avfilter_graph_create_filter(&ctx, f, instance.toUtf8().constData(), args.isEmpty() ? nullptr : args.toUtf8().constData(), nullptr, m_graph);
    if (err < 0) { fail(err, QString("cannot create the %1 filter").arg(name)); return nullptr; }
    return ctx;
}

bool Runner::chainFilters(const QStringList& specs, AVFilterContext** last, int* lastPad, const QString& tag) {
    for (int i = 0; i < specs.size(); ++i) {
        const QString spec = specs[i];
        const int eq = spec.indexOf('=');
        const QString name = eq < 0 ? spec : spec.left(eq), args = eq < 0 ? QString() : spec.mid(eq + 1);
        AVFilterContext* f = makeFilter(name.toUtf8().constData(), args, tag + "_" + name + QString::number(i));
        if (!f) return false;
        const int err = avfilter_link(*last, *lastPad, f, 0);
        if (err < 0) return fail(err, "cannot link " + name);
        *last = f; *lastPad = 0;
    }
    return true;
}

// ffmpeg's output filters (ffmpeg_filter.c configure_output_{video,audio}_filter): format / aformat with what the
// encoder takes (-pix_fmt, -ar, -ac), then trim / atrim for the output -t, then the sink.
bool Runner::attachSink(OutStream& os, AVFilterContext* from, int pad) {
    const QString tag = os.type == AVMEDIA_TYPE_VIDEO ? "v" : "a";
    QStringList specs;
    if (os.type == AVMEDIA_TYPE_VIDEO) {
        const QString enc = m_out.videoEncoder();
        if (!m_out.pixFmt.isEmpty()) specs << "format=pix_fmts=" + m_out.pixFmt + (m_out.colorRange.isEmpty() ? QString() : ":color_ranges=" + m_out.colorRange);
        else {
            const AVCodec* codec = avcodec_find_encoder_by_name(enc.toUtf8().constData());
            const void* cfg = nullptr; int n = 0;
            if (codec && avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) >= 0 && cfg && n > 0) {
                QStringList fmts; const auto* list = static_cast<const AVPixelFormat*>(cfg);
                for (int i = 0; i < n; ++i) fmts << QString::fromLatin1(av_get_pix_fmt_name(list[i]));
                specs << "format=pix_fmts=" + fmts.join('|');
            }
        }
        if (m_out.durationSec >= 0) specs << "trim=duration=" + QString::number(m_out.durationSec, 'f', 6);
    } else {
        const QString enc = m_out.audioEncoder();
        const AVCodec* codec = avcodec_find_encoder_by_name(enc.toUtf8().constData());
        QStringList parts;
        const void* cfg = nullptr; int n = 0;
        if (codec && avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n) >= 0 && cfg && n > 0) {
            QStringList fmts; const auto* list = static_cast<const AVSampleFormat*>(cfg);
            for (int i = 0; i < n; ++i) fmts << QString::fromLatin1(av_get_sample_fmt_name(list[i]));
            parts << "sample_fmts=" + fmts.join('|');
        }
        if (m_out.sampleRate > 0) parts << "sample_rates=" + QString::number(m_out.sampleRate);
        if (m_out.channels > 0) {
            AVChannelLayout l{}; av_channel_layout_default(&l, m_out.channels);
            char buf[64] = {0}; av_channel_layout_describe(&l, buf, sizeof(buf)); av_channel_layout_uninit(&l);
            parts << "channel_layouts=" + QString::fromLatin1(buf);
        }
        if (!parts.isEmpty()) specs << "aformat=" + parts.join(':');
        if (m_out.durationSec >= 0) specs << "atrim=duration=" + QString::number(m_out.durationSec, 'f', 6);
    }
    AVFilterContext* last = from; int lastPad = pad;
    if (!chainFilters(specs, &last, &lastPad, "out_" + tag)) return false;
    os.sink = makeFilter(os.type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink", QString(), "sink_" + tag + QString::number(os.index));
    if (!os.sink) return false;
    const int err = avfilter_link(last, lastPad, os.sink, 0);
    if (err < 0) return fail(err, "cannot link the sink");
    return true;
}

bool Runner::buildGraph() {
    // the sources, from the first frame of each used stream (ffmpeg_filter.c configure_input_{video,audio}_filter)
    for (Input* in : m_inputs) {
        for (InStream& s : in->streams) {
            if (s.copyTo >= 0) continue;
            const AVFrame* f = s.pending.first();
            for (int k = 0; k < s.srcs.size(); ++k) {
                const QString tag = QString("in_%1_%2_%3").arg(in->index).arg(s.index).arg(k);
                AVFilterContext* src = nullptr;
                if (s.type() == AVMEDIA_TYPE_VIDEO) {
                    src = avfilter_graph_alloc_filter(m_graph, avfilter_get_by_name("buffer"), tag.toUtf8().constData());
                    if (!src) return fail(AVERROR(ENOMEM), "cannot allocate the buffer source");
                    AVBufferSrcParameters* p = av_buffersrc_parameters_alloc();
                    p->format = f->format; p->width = f->width; p->height = f->height;
                    p->time_base = s.st->time_base; p->sample_aspect_ratio = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : AVRational{0, 1};
                    p->frame_rate = av_guess_frame_rate(in->fc, s.st, nullptr);
                    p->color_space = f->colorspace; p->color_range = f->color_range;
                    const int err = av_buffersrc_parameters_set(src, p); av_free(p);
                    if (err < 0) return fail(err, "buffer source parameters");
                } else {
                    src = avfilter_graph_alloc_filter(m_graph, avfilter_get_by_name("abuffer"), tag.toUtf8().constData());
                    if (!src) return fail(AVERROR(ENOMEM), "cannot allocate the audio buffer source");
                    AVBufferSrcParameters* p = av_buffersrc_parameters_alloc();
                    p->format = f->format; p->sample_rate = f->sample_rate; p->time_base = s.st->time_base;
                    av_channel_layout_copy(&p->ch_layout, &f->ch_layout);
                    const int err = av_buffersrc_parameters_set(src, p); av_channel_layout_uninit(&p->ch_layout); av_free(p);
                    if (err < 0) return fail(err, "audio buffer source parameters");
                }
                if (const int err = avfilter_init_str(src, nullptr); err < 0) return fail(err, "cannot initialise the buffer source");
                s.srcs[k] = src;
            }
        }
    }
    // link the sources into the user's graph (complex) or build the simple one, with autorotate first
    auto rotateChain = [&](Input& in, InStream& s, int k, AVFilterContext** last, int* pad) {
        *last = s.srcs[k]; *pad = 0;
        if (s.type() != AVMEDIA_TYPE_VIDEO) return true;
        const QStringList rot = rotationFilters(s.pending.first(), s.st);
        return chainFilters(rot, last, pad, QString("rot_%1_%2_%3").arg(in.index).arg(s.index).arg(k));
    };
    if (!m_c.filterComplex.isEmpty()) {
        // the n-th open input pad of the graph is the n-th src of its stream, in resolveStreams' order
        QHash<InStream*, int> used;
        for (AVFilterInOut* io = m_graphIns; io; io = io->next) {
            QString why;
            InStream* s = streamForLabel(QString::fromUtf8(io->name ? io->name : ""), avfilter_pad_get_type(io->filter_ctx->input_pads, io->pad_idx), &why);
            if (!s) return fail(AVERROR(EINVAL), why);
            Input* in = nullptr; for (Input* i : m_inputs) for (InStream& t : i->streams) if (&t == s) in = i;
            const int k = used[s]++;
            AVFilterContext* last = nullptr; int pad = 0;
            if (!rotateChain(*in, *s, k, &last, &pad)) return false;
            const int err = avfilter_link(last, pad, io->filter_ctx, io->pad_idx);
            if (err < 0) return fail(err, QString("cannot connect [%1] to %2").arg(QString::fromUtf8(io->name ? io->name : "?"), QString::fromUtf8(io->filter_ctx->name)));
        }
        for (const QString& m : m_out.maps) {
            AVFilterInOut* io = m_graphOuts; for (; io; io = io->next) if (io->name && m == QString::fromUtf8(io->name)) break;
            OutStream* os = new OutStream; os->type = avfilter_pad_get_type(io->filter_ctx->output_pads, io->pad_idx); os->index = m_outs.size();
            m_outs << os;
            if (!attachSink(*os, io->filter_ctx, io->pad_idx)) return false;
        }
    } else {
        for (Input* in : m_inputs) {
            for (InStream& s : in->streams) {
                OutStream* os = new OutStream; os->type = s.type(); os->index = m_outs.size();
                m_outs << os;
                if (s.copyTo >= 0) { os->copy = true; os->tb = s.st->time_base; s.copyTo = os->index; continue; }
                AVFilterContext* last = nullptr; int pad = 0;
                if (!rotateChain(*in, s, 0, &last, &pad)) return false;
                const QString chain = s.type() == AVMEDIA_TYPE_VIDEO ? m_out.vf : m_out.af;
                if (!chain.isEmpty()) {
                    AVFilterInOut* ins = nullptr; AVFilterInOut* outs = nullptr;
                    int err = avfilter_graph_parse_ptr(m_graph, chain.toUtf8().constData(), &ins, &outs, nullptr);
                    if (err < 0 || !ins || !outs) { avfilter_inout_free(&ins); avfilter_inout_free(&outs); return fail(err < 0 ? err : AVERROR(EINVAL), s.type() == AVMEDIA_TYPE_VIDEO ? "-vf" : "-af"); }
                    err = avfilter_link(last, pad, ins->filter_ctx, ins->pad_idx);
                    if (err >= 0) { last = outs->filter_ctx; pad = outs->pad_idx; }
                    avfilter_inout_free(&ins); avfilter_inout_free(&outs);
                    if (err < 0) return fail(err, "cannot connect the simple graph");
                }
                if (!attachSink(*os, last, pad)) return false;
            }
        }
    }
    // the output stream order ffmpeg uses: video first, then audio (the -map order of the spot graphs is [v] [a] already)
    const int err = avfilter_graph_config(m_graph, nullptr);
    if (err < 0) return fail(err, "Error configuring the filtergraph");
    m_graphReady = true;
    return true;
}

// ---- encoders and the output ------------------------------------------------------------------------------------------
bool Runner::openEncoders() {
    const bool vfrMuxer = m_out.container == "mp4" || m_out.container == "mov";   // AVFMT_VARIABLE_FPS: vfr unless -r
    for (OutStream* os : m_outs) {
        if (os->copy) continue;
        const QString name = os->type == AVMEDIA_TYPE_VIDEO ? m_out.videoEncoder() : m_out.audioEncoder();
        const AVCodec* codec = avcodec_find_encoder_by_name(name.toUtf8().constData());
        if (!codec) return fail(AVERROR_ENCODER_NOT_FOUND, "Unknown encoder '" + name + "'");
        os->enc = avcodec_alloc_context3(codec);
        if (!os->enc) return fail(AVERROR(ENOMEM), "encoder " + name);
        AVCodecContext* e = os->enc;
        AVDictionary* opts = nullptr;
        if (os->type == AVMEDIA_TYPE_VIDEO) {
            e->width = av_buffersink_get_w(os->sink); e->height = av_buffersink_get_h(os->sink);
            e->pix_fmt = AVPixelFormat(av_buffersink_get_format(os->sink));
            e->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(os->sink);
            e->colorspace = av_buffersink_get_colorspace(os->sink);
            e->color_range = av_buffersink_get_color_range(os->sink);
            if (!m_out.colorRange.isEmpty()) e->color_range = AVColorRange(av_color_range_from_name(m_out.colorRange.toUtf8().constData()));
            AVRational fr = av_buffersink_get_frame_rate(os->sink);
            if (!m_out.frameRate.isEmpty()) { av_parse_video_rate(&fr, m_out.frameRate.toUtf8().constData()); os->cfr = true; }
            else os->cfr = !vfrMuxer;
            e->framerate = fr;
            e->time_base = fr.num > 0 && fr.den > 0 ? av_inv_q(fr) : av_buffersink_get_time_base(os->sink);
            if (m_out.gop >= 0) e->gop_size = m_out.gop;
            e->thread_count = 0;                                      // ffmpeg's -threads auto
            if (name == "libx264") {
                if (!m_out.preset.isEmpty()) av_dict_set(&opts, "preset", m_out.preset.toUtf8().constData(), 0);
                if (m_out.crf >= 0) av_dict_set(&opts, "crf", QString::number(m_out.crf).toUtf8().constData(), 0);
            } else if (name == "mjpeg" && m_out.qscale > 0) {
                e->flags |= AV_CODEC_FLAG_QSCALE; e->global_quality = int(FF_QP2LAMBDA * m_out.qscale);
            }
            os->frameLimit = m_out.frames;
        } else {
            e->sample_fmt = AVSampleFormat(av_buffersink_get_format(os->sink));
            e->sample_rate = av_buffersink_get_sample_rate(os->sink);
            av_buffersink_get_ch_layout(os->sink, &e->ch_layout);
            e->time_base = AVRational{1, e->sample_rate};
            if (m_out.audioBitrate > 0) e->bit_rate = m_out.audioBitrate;
            e->thread_count = 0;
        }
        if (m_mux.context()->oformat->flags & AVFMT_GLOBALHEADER) e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int err = avcodec_open2(e, codec, &opts);
        av_dict_free(&opts);
        if (err < 0) return fail(err, "Error while opening encoder for output stream #0:" + QString::number(os->index));
        os->tb = e->time_base;
        if (os->type == AVMEDIA_TYPE_AUDIO && !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) && e->frame_size > 0)
            av_buffersink_set_frame_size(os->sink, unsigned(e->frame_size));
    }
    return true;
}

bool Runner::openOutput() {
    // the muxer first (the encoders need its global-header flag), then its streams
    if (!m_mux.open(m_out, m_c.overwrite, m_c.noOverwrite, &m_err, &m_averr)) return fail(m_averr, m_err);
    if (!openEncoders()) return false;
    for (OutStream* os : m_outs) {
        AVStream* st = m_mux.addStream(os->tb);
        if (!st) return fail(AVERROR(ENOMEM), "cannot add an output stream");
        if (os->copy) {
            InStream* src = nullptr; const AVStream* ist = nullptr;
            for (Input* in : m_inputs) for (InStream& s : in->streams) if (s.copyTo == os->index) { src = &s; ist = s.st; }
            if (!src) return fail(AVERROR(EINVAL), "copy stream without a source");
            int err = avcodec_parameters_copy(st->codecpar, ist->codecpar);
            if (err < 0) return fail(err, "cannot copy the stream parameters");
            unsigned tmp = 0; const AVOutputFormat* of = m_mux.context()->oformat;
            if (!(!of->codec_tag || av_codec_get_id(of->codec_tag, ist->codecpar->codec_tag) == ist->codecpar->codec_id || !av_codec_get_tag2(of->codec_tag, ist->codecpar->codec_id, &tmp)))
                st->codecpar->codec_tag = 0;
            st->avg_frame_rate = ist->avg_frame_rate; st->r_frame_rate = ist->r_frame_rate; st->sample_aspect_ratio = ist->sample_aspect_ratio; st->disposition = ist->disposition;
            av_dict_copy(&st->metadata, ist->metadata, AV_DICT_DONT_OVERWRITE);   // ffmpeg keeps the stream tags (the mp4 compressorname among them)
            const QString name = QString::fromLatin1(avcodec_get_name(ist->codecpar->codec_id));
            m_r.streams << (os->type == AVMEDIA_TYPE_VIDEO ? QString("video %1 %2x%3 (copy)").arg(name).arg(ist->codecpar->width).arg(ist->codecpar->height) : QString("audio %1 %2 (copy)").arg(name).arg(ist->codecpar->sample_rate));
        } else {
            const int err = avcodec_parameters_from_context(st->codecpar, os->enc);
            if (err < 0) return fail(err, "encoder parameters");
            if (os->type == AVMEDIA_TYPE_VIDEO) { st->avg_frame_rate = os->enc->framerate; st->sample_aspect_ratio = os->enc->sample_aspect_ratio; }
            av_dict_set(&st->metadata, "encoder", (QByteArray(LIBAVCODEC_IDENT) + ' ' + os->enc->codec->name).constData(), 0);   // as ffmpeg_mux_init.c tags it
            if (os->type == AVMEDIA_TYPE_VIDEO)
                m_r.streams << QString("video %1 %2x%3 %4 %5/%6").arg(os->enc->codec->name).arg(os->enc->width).arg(os->enc->height).arg(QString::fromLatin1(av_get_pix_fmt_name(os->enc->pix_fmt))).arg(os->enc->framerate.num).arg(os->enc->framerate.den);
            else {
                char lay[64] = {0}; av_channel_layout_describe(&os->enc->ch_layout, lay, sizeof(lay));
                m_r.streams << QString("audio %1 %2 %3").arg(os->enc->codec->name).arg(os->enc->sample_rate).arg(QString::fromLatin1(lay));
            }
        }
    }
    if (!m_mux.writeHeader(&m_err, &m_averr)) return fail(m_averr, m_err);
    m_headerWritten = true;
    // copy packets that arrived while the first frames were being found
    for (Input* in : m_inputs) {
        for (AVPacket* p : in->pendingCopy) {
            InStream* s = in->stream(p->stream_index);
            const bool ok = s && writeCopy(*in, *s, p);
            av_packet_free(&p);
            if (!ok) { in->pendingCopy.clear(); return false; }
        }
        in->pendingCopy.clear();
    }
    return true;
}

// ---- reading, decoding, feeding --------------------------------------------------------------------------------------
bool Runner::writeCopy(Input& in, InStream& s, AVPacket* pkt) {
    if (s.copyDone) return true;
    if (m_out.durationSec >= 0 && pkt->pts != AV_NOPTS_VALUE && av_compare_ts(pkt->pts, s.st->time_base, std::llround(m_out.durationSec * AV_TIME_BASE), AV_TIME_BASE_Q) >= 0) { s.copyDone = true; return true; }
    if (!m_mux.writePacket(pkt, s.copyTo, s.st->time_base, &m_err, &m_averr)) return fail(m_averr, m_err);
    Q_UNUSED(in);
    return true;
}

bool Runner::readOne(Input& in) {
    if (in.eof) return true;
    const int err = av_read_frame(in.fc, m_pkt.p);
    if (err == AVERROR_EOF || (err < 0 && avio_feof(in.fc->pb))) return inputEof(in);
    if (err < 0) return fail(err, in.spec->path);
    InStream* s = in.stream(m_pkt.p->stream_index);
    if (!s) { av_packet_unref(m_pkt.p); return true; }
    // ffmpeg_demux.c ts_fixup: rebase by start_time (+ -ss), add the stream_loop offset, end the input at -t
    const int64_t off = av_rescale_q(in.startUs, AV_TIME_BASE_Q, s->st->time_base) - s->loopOffset;
    if (m_pkt.p->pts != AV_NOPTS_VALUE) m_pkt.p->pts -= off;
    if (m_pkt.p->dts != AV_NOPTS_VALUE) m_pkt.p->dts -= off;
    // the input -t ends each stream at its first packet with dts at or past it (that packet is not used); the input
    // itself ends once every used stream got there (ffmpeg_demux.c input_packet_process: ds->finished)
    if (s->inputDone) { av_packet_unref(m_pkt.p); return true; }
    const int64_t dts = m_pkt.p->dts != AV_NOPTS_VALUE ? m_pkt.p->dts : m_pkt.p->pts;
    if (in.limitUs != INT64_MAX && dts != AV_NOPTS_VALUE && av_compare_ts(dts, s->st->time_base, in.limitUs, AV_TIME_BASE_Q) >= 0) {
        av_packet_unref(m_pkt.p);
        s->inputDone = true; if (s->copyTo >= 0) s->copyDone = true;
        return in.allInputDone() ? inputEof(in) : true;
    }
    const int64_t ts = m_pkt.p->pts != AV_NOPTS_VALUE ? m_pkt.p->pts : m_pkt.p->dts;
    if (ts != AV_NOPTS_VALUE) s->lastEnd = std::max(s->lastEnd == AV_NOPTS_VALUE ? INT64_MIN : s->lastEnd, ts + std::max<int64_t>(0, m_pkt.p->duration));
    const bool ok = handlePacket(in, *s, m_pkt.p);
    av_packet_unref(m_pkt.p);
    return ok;
}

bool Runner::handlePacket(Input& in, InStream& s, AVPacket* pkt) {
    if (s.copyTo >= 0) {
        if (!m_headerWritten) { AVPacket* keep = av_packet_clone(pkt); if (!keep) return fail(AVERROR(ENOMEM), "packet"); in.pendingCopy << keep; return true; }
        return writeCopy(in, s, pkt);
    }
    return decode(in, s, pkt);
}

bool Runner::decode(Input& in, InStream& s, AVPacket* pkt) {
    int err = avcodec_send_packet(s.dec, pkt);
    if (err < 0 && err != AVERROR_EOF && err != AVERROR(EAGAIN)) {
        if (pkt) { av_log(nullptr, AV_LOG_WARNING, "%s: decode error, packet dropped: %s\n", in.spec->path.toUtf8().constData(), avErr(err).toUtf8().constData()); return true; }
        return fail(err, "decoder");
    }
    for (;;) {
        err = avcodec_receive_frame(s.dec, m_frame.p);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) return fail(err, "decoder");
        if (m_frame.p->pts == AV_NOPTS_VALUE) m_frame.p->pts = m_frame.p->best_effort_timestamp;
        const bool ok = deliver(in, s, m_frame.p);
        av_frame_unref(m_frame.p);
        if (!ok) return false;
    }
    return true;
}

// The -ss trim (ffmpeg's accurate seek inserts trim / atrim at the input): video frames before 0 are dropped, an audio
// frame straddling 0 loses its leading samples. Then the frame is queued (graph not built yet) or pushed.
bool Runner::deliver(Input& in, InStream& s, AVFrame* f) {
    Q_UNUSED(in);
    if (s.trimStart != AV_NOPTS_VALUE && f->pts != AV_NOPTS_VALUE && f->pts < s.trimStart) {
        if (s.type() == AVMEDIA_TYPE_VIDEO) return true;
        const int64_t skip = av_rescale_q(s.trimStart - f->pts, s.st->time_base, AVRational{1, f->sample_rate});
        if (skip >= f->nb_samples) return true;
        Frame cut;
        cut.p->format = f->format; cut.p->sample_rate = f->sample_rate; cut.p->nb_samples = int(f->nb_samples - skip);
        av_channel_layout_copy(&cut.p->ch_layout, &f->ch_layout);
        if (av_frame_get_buffer(cut.p, 0) < 0 || av_frame_copy_props(cut.p, f) < 0) return fail(AVERROR(ENOMEM), "audio trim");
        av_samples_copy(cut.p->data, f->data, 0, int(skip), cut.p->nb_samples, f->ch_layout.nb_channels, AVSampleFormat(f->format));
        cut.p->pts = s.trimStart;
        return deliver(in, s, cut.p);
    }
    if (s.trimEnd != AV_NOPTS_VALUE && f->pts != AV_NOPTS_VALUE) {
        // ffmpeg's input trim end (trim=end / atrim=end at the input filter): frames past -t are dropped, the audio
        // frame crossing it loses its tail
        if (f->pts >= s.trimEnd) return true;
        if (s.type() == AVMEDIA_TYPE_AUDIO) {
            const int64_t keep = av_rescale_q(s.trimEnd - f->pts, s.st->time_base, AVRational{1, f->sample_rate});
            if (keep <= 0) return true;
            if (keep < f->nb_samples) {
                Frame cut; if (av_frame_ref(cut.p, f) < 0) return fail(AVERROR(ENOMEM), "audio trim");
                cut.p->nb_samples = int(keep);
                if (!m_graphReady) { AVFrame* k = av_frame_clone(cut.p); if (!k) return fail(AVERROR(ENOMEM), "frame"); s.pending << k; return true; }
                return push(s, cut.p);
            }
        }
    }
    if (!m_graphReady) { AVFrame* keep = av_frame_clone(f); if (!keep) return fail(AVERROR(ENOMEM), "frame"); s.pending << keep; return true; }
    return push(s, f);
}

bool Runner::push(InStream& s, AVFrame* f) {
    for (int k = 0; k < s.srcs.size(); ++k) {
        if (s.srcClosed[k]) continue;
        const int err = av_buffersrc_add_frame_flags(s.srcs[k], f, AV_BUFFERSRC_FLAG_KEEP_REF | AV_BUFFERSRC_FLAG_PUSH);
        if (err == AVERROR_EOF || (err == AVERROR(EINVAL) && !f)) { s.srcClosed[k] = true; continue; }   // the graph closed this input (trim done)
        if (err < 0) return fail(err, "Error while feeding the filtergraph");
    }
    return true;
}

// Demuxer EOF: -stream_loop -1 re-reads from the start with ffmpeg's timestamp offset (the longest stream read so far,
// decoders drained and flushed first); otherwise the decoders are drained and EOF goes into the sources.
bool Runner::inputEof(Input& in) {
    for (InStream& s : in.streams) if (s.copyTo < 0 && !decode(in, s, nullptr)) return false;
    if (in.spec->streamLoop) {
        int64_t durUs = 0;
        for (const InStream& s : in.streams) if (s.lastEnd != AV_NOPTS_VALUE) durUs = std::max(durUs, av_rescale_q(s.lastEnd - s.loopOffset, s.st->time_base, AV_TIME_BASE_Q));
        const int64_t start = in.fc->start_time != AV_NOPTS_VALUE ? in.fc->start_time : 0;
        const int err = avformat_seek_file(in.fc, -1, INT64_MIN, start, start, 0);
        if (err >= 0 && durUs > 0) {
            for (InStream& s : in.streams) { s.loopOffset += av_rescale_q(durUs, AV_TIME_BASE_Q, s.st->time_base); if (s.dec) avcodec_flush_buffers(s.dec); }
            return true;
        }
        av_log(nullptr, AV_LOG_WARNING, "%s: cannot loop the input (seek failed or nothing read)\n", in.spec->path.toUtf8().constData());
    }
    in.eof = true;
    for (InStream& s : in.streams) {
        if (s.copyTo >= 0) { s.copyDone = true; continue; }
        if (s.eofSent) continue;
        s.eofSent = true;
        if (m_graphReady && !push(s, nullptr)) return false;
    }
    return true;
}

// ---- the sinks and the encoders --------------------------------------------------------------------------------------
bool Runner::reapSinks() {
    for (OutStream* os : m_outs) {
        if (os->copy || os->flushed) continue;
        for (;;) {
            const int err = av_buffersink_get_frame_flags(os->sink, m_frame.p, 0);
            if (err == AVERROR(EAGAIN)) break;
            if (err == AVERROR_EOF) { os->sinkEof = true; if (!encodeFrame(*os, nullptr)) return false; break; }
            if (err < 0) return fail(err, "Error reading from the filtergraph");
            const bool ok = encodeFrame(*os, m_frame.p);
            av_frame_unref(m_frame.p);
            if (!ok) return false;
            if (os->flushed) break;
        }
    }
    return true;
}

// ffmpeg_filter.c video_sync_process (cfr: duplicate / drop to the output rate; vfr: round to the encoder grid, drop a
// frame that lands on the previous one), then the encoder.
bool Runner::videoOut(OutStream& os, AVFrame* f) {
    const AVRational sinkTb = av_buffersink_get_time_base(os.sink);
    const double syncIpts = f->pts == AV_NOPTS_VALUE ? double(os.nextPts) : av_q2d(sinkTb) / av_q2d(os.tb) * double(f->pts);
    double duration = f->duration > 0 ? av_q2d(sinkTb) / av_q2d(os.tb) * double(f->duration) : 1.0;
    double delta0 = syncIpts - double(os.nextPts), delta = delta0 + duration;
    int64_t nbFrames = 1, nb0 = 0;
    if (delta0 < 0 && delta > 0) { duration += delta0; delta0 = 0; }         // a frame a little late: shifted onto the grid
    if (os.cfr) {
        if (delta < -1.1) nbFrames = 0;
        else if (delta > 1.1) { nbFrames = std::llround(delta); if (delta0 > 1.1) nb0 = std::llround(delta0 - 0.6); }
    } else {
        if (delta <= -0.6) nbFrames = 0;
        else if (delta > 0.6) os.nextPts = std::llround(syncIpts);
    }
    if (nbFrames == 0) { ++os.drop; return true; }
    if (nbFrames > 1) os.dup += int(nbFrames - 1);
    for (int64_t i = 0; i < nbFrames; ++i) {
        if (os.frameLimit >= 0 && os.framesOut >= os.frameLimit) break;
        AVFrame* src = (i < nb0 && os.lastFrame) ? os.lastFrame : f;
        Frame copy; if (av_frame_ref(copy.p, src) < 0) return fail(AVERROR(ENOMEM), "frame");
        copy.p->pts = os.nextPts++; copy.p->duration = 1;
        copy.p->pict_type = AV_PICTURE_TYPE_NONE;
        int err = avcodec_send_frame(os.enc, copy.p);
        if (err < 0) return fail(err, "Error submitting a video frame to the encoder");
        ++os.framesOut;
        for (;;) {
            err = avcodec_receive_packet(os.enc, m_pkt.p);
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
            if (err < 0) return fail(err, "video encoder");
            const bool ok = m_mux.writePacket(m_pkt.p, os.index, os.tb, &m_err, &m_averr);
            av_packet_unref(m_pkt.p);
            if (!ok) return fail(m_averr, m_err);
        }
    }
    if (!os.lastFrame) os.lastFrame = av_frame_alloc();
    av_frame_unref(os.lastFrame); av_frame_ref(os.lastFrame, f);
    if (os.frameLimit >= 0 && os.framesOut >= os.frameLimit && !os.flushed) return encodeFrame(os, nullptr);
    return true;
}

bool Runner::encodeFrame(OutStream& os, AVFrame* f) {
    if (os.flushed) return true;
    if (f && os.type == AVMEDIA_TYPE_VIDEO) return videoOut(os, f);
    if (f) {
        // audio: the sink delivers encoder-sized frames; pts in 1/sample_rate
        if (f->pts != AV_NOPTS_VALUE) f->pts = av_rescale_q(f->pts, av_buffersink_get_time_base(os.sink), os.tb);
        os.samplesOut += f->nb_samples;
    }
    int err = avcodec_send_frame(os.enc, f);
    if (err < 0 && !(err == AVERROR_EOF && !f)) return fail(err, "Error submitting a frame to the encoder");
    if (f && os.type == AVMEDIA_TYPE_VIDEO) ++os.framesOut;
    for (;;) {
        err = avcodec_receive_packet(os.enc, m_pkt.p);
        if (err == AVERROR(EAGAIN)) break;
        if (err == AVERROR_EOF) { os.flushed = true; break; }
        if (err < 0) return fail(err, "encoder");
        const bool ok = m_mux.writePacket(m_pkt.p, os.index, os.tb, &m_err, &m_averr);
        av_packet_unref(m_pkt.p);
        if (!ok) return fail(m_averr, m_err);
    }
    if (!f && !os.flushed) os.flushed = true;
    return true;
}

// ---- the loop ---------------------------------------------------------------------------------------------------------
void Runner::report(bool end) {
    if (!m_o.progress) return;
    const double el = std::max(0.001, m_clock.elapsed() / 1000.0);
    qint64 frames = 0; int dup = 0, drop = 0;
    for (OutStream* os : m_outs) if (os->type == AVMEDIA_TYPE_VIDEO) { frames += os->copy ? m_mux.packets(os->index) : os->framesOut; dup += os->dup; drop += os->drop; }
    const qint64 outUs = m_mux.outTimeUs();
    const double bitrate = outUs > 0 ? m_mux.bytesWritten() * 8.0 / (outUs / 1e6) / 1000.0 : -1;
    m_o.progress(progressBlock(frames, frames / el, bitrate, m_mux.bytesWritten(), outUs, dup, drop, outUs > 0 ? (outUs / 1e6) / el : 0, end));
    m_progressClock.restart();
}

bool Runner::feedLoop() {
    // what was decoded before the graph existed
    for (Input* in : m_inputs)
        for (InStream& s : in->streams) {
            for (AVFrame* f : s.pending) { const bool ok = push(s, f); av_frame_free(&f); if (!ok) { s.pending.clear(); return false; } }
            s.pending.clear();
            if (s.eofSent && !push(s, nullptr)) return false;
        }
    int idle = 0, blindReads = 0;        // reads nobody asked for, since the last frame out of a sink
    for (;;) {
        if (stopped()) return false;
        bool allDone = true; for (OutStream* os : m_outs) if (!os->done()) allDone = false;
        if (allDone) break;
        const int ret = avfilter_graph_request_oldest(m_graph);
        if (ret == AVERROR_EOF) { if (!reapSinks()) return false; for (OutStream* os : m_outs) if (!os->copy && !os->flushed && !encodeFrame(*os, nullptr)) return false; break; }
        if (ret >= 0 || ret == AVERROR(EAGAIN)) { if (!reapSinks()) return false; }
        else return fail(ret, "Error while filtering");
        if (ret >= 0) { idle = 0; blindReads = 0; continue; }
        // EAGAIN: the input whose sources failed the most requests gets the next packet (fftools transcode_from_filter)
        Input* best = nullptr; unsigned bestN = 0;
        for (Input* in : m_inputs) {
            if (in->eof) continue;
            unsigned n = 0;
            for (InStream& s : in->streams) for (int k = 0; k < s.srcs.size(); ++k) if (!s.srcClosed[k]) n = std::max(n, av_buffersrc_get_nb_failed_requests(s.srcs[k]));
            if (n > bestN || (!best && n > 0)) { best = in; bestN = n; }
        }
        if (!best) {
            for (Input* in : m_inputs) if (!in->eof && !in->allClosed()) { best = in; break; }   // nothing asked: keep the stream moving
            if (best && ++blindReads > 4096) return fail(AVERROR(EDEADLK), "the filtergraph takes no input and delivers no frame");
            if (!best) {
                for (Input* in : m_inputs) if (!in->eof) { in->eof = true; for (InStream& s : in->streams) if (s.copyTo < 0 && !s.eofSent) { s.eofSent = true; if (!push(s, nullptr)) return false; } }
                if (++idle > 64) return fail(AVERROR(EDEADLK), "the filtergraph asks for no input and delivers no frame");
                continue;
            }
        }
        idle = 0;
        if (!readOne(*best)) return false;
        if (best->allClosed() && !best->eof) {
            // every use of this input is finished (its trims closed): stop reading a looping still / sequence
            best->eof = true;
            for (InStream& s : best->streams) if (s.copyTo < 0 && !s.eofSent) { s.eofSent = true; if (!push(s, nullptr)) return false; }
        }
        if (m_o.progress && m_progressClock.elapsed() >= m_o.progressIntervalMs) report(false);
    }
    return true;
}

bool Runner::finish() {
    if (!m_mux.finish(&m_err, &m_averr)) return fail(m_averr, m_err);
    return true;
}

RunResult Runner::run() {
    m_clock.start(); m_progressClock.start();
    m_irq.clock.start(); m_irq.timeoutMs = m_o.timeoutMs; m_irq.cancel = &m_o.cancelled;
    m_r.out = m_out.path;
    const bool ok = openInputs() && parseComplexGraph() && resolveStreams() && openDecoders() && firstFrames() && buildGraph() && openOutput() && feedLoop() && finish();
    m_r.ok = ok && m_r.error.isEmpty();
    if (m_r.ok) {
        report(true);
        m_r.durationSec = m_mux.durationSec();
        m_r.bytes = m_mux.bytesWritten();
        for (OutStream* os : m_outs) {
            if (os->type == AVMEDIA_TYPE_VIDEO) { m_r.videoFrames += m_mux.packets(os->index); m_r.dupFrames += os->dup; m_r.dropFrames += os->drop; }
            else m_r.audioSamples += os->samplesOut;
        }
    }
    m_r.elapsedSec = m_clock.elapsed() / 1000.0;
    return m_r;
}

}   // namespace

RunResult runGraph(const GraphCommand& c, const RunOptions& o) {
    if (!c.ok()) { RunResult r; r.error = c.unsupported.isEmpty() ? c.error : c.unsupported; r.averror = AVERROR(EINVAL); return r; }
    if (c.outputs.size() != 1) { RunResult r; r.error = "one output file"; r.averror = AVERROR(EINVAL); return r; }
    if (isCopyOnly(c)) return remuxCopy(c, o);
    Runner run(c, o);
    return run.run();
}

QJsonObject runJson(const RunResult& r) {
    QJsonObject j;
    if (!r.ok) {
        j["ok"] = false; j["error"] = r.error; j["averror"] = r.averror;
        if (r.timedOut) j["timeout"] = true;
        if (r.cancelled) j["cancelled"] = true;
        return j;
    }
    j["ok"] = true; j["out"] = QDir::toNativeSeparators(r.out); j["duration"] = r.durationSec; j["frames"] = double(r.videoFrames); j["samples"] = double(r.audioSamples);
    j["bytes"] = double(r.bytes); j["dup"] = r.dupFrames; j["drop"] = r.dropFrames; j["streams"] = QJsonArray::fromStringList(r.streams); j["elapsed"] = r.elapsedSec;
    return j;
}

int graphRunCli(const QStringList& args) {
    const GraphCommand c = parseGraphArgs(args);
    if (!c.unsupported.isEmpty()) return ppcli::unsupported(c.unsupported);
    if (!c.error.isEmpty()) return ppcli::fail(c.error);
    av_log_set_level(c.logLevel);
    RunOptions o;
    bool ok = false; const int t = qEnvironmentVariableIntValue("PLAYPLAY_MEDIATOOL_RENDER_TIMEOUT_MS", &ok);
    if (ok && t > 0) o.timeoutMs = t;
    if (c.progressPipe) o.progress = [](const QByteArray& b) { std::fwrite(b.constData(), 1, size_t(b.size()), stdout); std::fflush(stdout); };
    const RunResult r = runGraph(c, o);
    if (!r.ok) av_log(nullptr, AV_LOG_ERROR, "%s\n", r.error.toUtf8().constData());
    return ppcli::finish(runJson(r));
}

} // namespace mediatool
