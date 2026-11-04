// PlayPlayAvKit probe: fftools/ffprobe.c (open_input_file, show_streams, show_format, the JSON text formatter) for the
// -show_format -show_streams subset, in-process. See avprobe.h for the contract and what is left out.
#include "avprobe.h"

#include <QJsonArray>
#include <QJsonValue>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace avkit {
namespace {

QString avErr(int r) { char b[AV_ERROR_MAX_STRING_SIZE] = {0}; av_strerror(r, b, sizeof(b)); return QString::fromUtf8(b); }

// ---------------------------------------------------------------------------------------------
// Ordered JSON: ffprobe prints keys in a fixed order (tags in file order); a QJsonObject would sort them away.
// ---------------------------------------------------------------------------------------------
struct Node {
    enum Kind { Int, Str, Obj, Arr } kind = Obj;
    QByteArray key;
    qint64 i = 0;
    QByteArray s;
    std::vector<Node> kids;

    void num(const char* k, qint64 v) { Node n; n.kind = Int; n.key = k; n.i = v; kids.push_back(std::move(n)); }
    void str(const QByteArray& k, const QByteArray& v) { Node n; n.kind = Str; n.key = k; n.s = v; kids.push_back(std::move(n)); }
    void child(const char* k, Node&& n) { n.key = k; kids.push_back(std::move(n)); }
};

QJsonValue toJson(const Node& n) {
    switch (n.kind) {
    case Node::Int: return QJsonValue(n.i);
    case Node::Str: return QJsonValue(QString::fromUtf8(n.s));
    case Node::Arr: { QJsonArray a; for (const Node& k : n.kids) a.append(toJson(k)); return a; }
    case Node::Obj: default: { QJsonObject o; for (const Node& k : n.kids) o.insert(QString::fromUtf8(k.key), toJson(k)); return o; }
    }
}

// tf_json.c json_escape_str: " \ \b \f \n \r \t escaped, other bytes < 32 as \u00XX, everything else raw (UTF-8 kept).
void escapeTo(QByteArray& out, const QByteArray& s) {
    for (const char ch : s) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 32) { char b[8]; std::snprintf(b, sizeof(b), "\\u00%02x", static_cast<unsigned char>(ch)); out += b; }
            else out += ch;
        }
    }
}

// tf_json.c layout: 4 spaces per level, items joined by ",\n", a container closes on its own line; an empty container
// prints as "{\n\n<indent>}" (the formatter always writes the item_start_end newline pair).
void writeTo(QByteArray& out, const Node& n, int level) {
    switch (n.kind) {
    case Node::Int: out += QByteArray::number(n.i); return;
    case Node::Str: out += '"'; escapeTo(out, n.s); out += '"'; return;
    case Node::Obj: case Node::Arr: {
        out += n.kind == Node::Obj ? "{\n" : "[\n";
        for (size_t k = 0; k < n.kids.size(); ++k) {
            if (k) out += ",\n";
            out += QByteArray((level + 1) * 4, ' ');
            if (n.kind == Node::Obj) { out += '"'; escapeTo(out, n.kids[k].key); out += "\": "; }
            writeTo(out, n.kids[k], level + 1);
        }
        out += '\n'; out += QByteArray(level * 4, ' '); out += n.kind == Node::Obj ? '}' : ']';
        return;
    }
    }
}

// ---------------------------------------------------------------------------------------------
// ffprobe value formatting
// ---------------------------------------------------------------------------------------------
QByteArray fixed6(double d) { char b[64]; std::snprintf(b, sizeof(b), "%f", d); return b; }                     // value_string, seconds
QByteArray q(AVRational r, char sep) { return QByteArray::number(r.num) + sep + QByteArray::number(r.den); }    // print_q
QByteArray hex(const char* fmt, unsigned v) { char b[32]; std::snprintf(b, sizeof(b), fmt, v); return b; }

// print_str_validate (tags, filename): every invalid UTF-8 sequence becomes one U+FFFD, decoded with no
// AV_UTF8_FLAG_ACCEPT_* flags, so an encoded surrogate is one invalid sequence (checked against ffprobe 9 by the test).
QByteArray validUtf8(const char* src) {
    QByteArray out;
    if (!src) return out;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
    const uint8_t* end = p + std::strlen(src);
    while (*p) {
        const uint8_t* p0 = p; int32_t code = 0;
        if (av_utf8_decode(&code, &p, end, 0) >= 0) out.append(reinterpret_cast<const char*>(p0), int(p - p0));
        else out.append("\xEF\xBF\xBD");
        if (p == p0) ++p;   // never stall (av_utf8_decode always advances, but be defensive)
    }
    return out;
}

void putTime(Node& n, const char* k, int64_t ts, AVRational tb) {   // print_time: AV_NOPTS_VALUE -> N/A (left out in JSON)
    if (ts != AV_NOPTS_VALUE) n.str(k, fixed6(av_q2d(tb) * double(ts)));
}
void putTs(Node& n, const char* k, int64_t ts) { if (ts != AV_NOPTS_VALUE) n.num(k, ts); }   // print_ts

Node tagsNode(const AVDictionary* dict) {
    Node t;
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_iterate(dict, e))) t.str(validUtf8(e->key), validUtf8(e->value));
    return t;
}

void putExported(Node& n, void* obj) {   // print_private_data: options flagged AV_OPT_FLAG_EXPORT, as strings
    const AVOption* opt = nullptr;
    while ((opt = av_opt_next(obj, opt))) {
        if (!(opt->flags & AV_OPT_FLAG_EXPORT)) continue;
        uint8_t* str = nullptr;
        if (av_opt_get(obj, opt->name, 0, &str) >= 0) { n.str(opt->name, reinterpret_cast<const char*>(str)); av_free(str); }
    }
}

Node sideDataNode(const AVPacketSideData& sd) {   // print_pkt_side_data, the types listed in avprobe.h
    Node e;
    const char* name = av_packet_side_data_name(sd.type);
    e.str("side_data_type", name ? name : "unknown");
    if (sd.type == AV_PKT_DATA_DISPLAYMATRIX && sd.size >= 9 * 4) {
        // writer_print_integers(" %11d", 3 columns, 4 bytes, offset +1), then the rotation truncated to an integer
        QByteArray m = "\n";
        for (int row = 0; row < 3; ++row) {
            char b[32]; std::snprintf(b, sizeof(b), "%08x: ", row); m += b;
            for (int c = 0; c < 3; ++c) { int32_t v; std::memcpy(&v, sd.data + 4 * (row * 3 + c), 4); std::snprintf(b, sizeof(b), " %11d", v); m += b; }
            m += '\n';
        }
        e.str("displaymatrix", m);
        double rot = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd.data));
        if (rot != rot) rot = 0;   // NaN
        e.num("rotation", qint64(rot));
    } else if (sd.type == AV_PKT_DATA_CPB_PROPERTIES && sd.size >= sizeof(AVCPBProperties)) {
        const auto* p = reinterpret_cast<const AVCPBProperties*>(sd.data);
        e.num("max_bitrate", p->max_bitrate); e.num("min_bitrate", p->min_bitrate); e.num("avg_bitrate", p->avg_bitrate);
        e.num("buffer_size", p->buffer_size); e.num("vbv_delay", qint64(p->vbv_delay));
    } else if (sd.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA && sd.size >= sizeof(AVMasteringDisplayMetadata)) {
        const auto* m = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd.data);
        if (m->has_primaries) {
            e.str("red_x", q(m->display_primaries[0][0], '/')); e.str("red_y", q(m->display_primaries[0][1], '/'));
            e.str("green_x", q(m->display_primaries[1][0], '/')); e.str("green_y", q(m->display_primaries[1][1], '/'));
            e.str("blue_x", q(m->display_primaries[2][0], '/')); e.str("blue_y", q(m->display_primaries[2][1], '/'));
            e.str("white_point_x", q(m->white_point[0], '/')); e.str("white_point_y", q(m->white_point[1], '/'));
        }
        if (m->has_luminance) { e.str("min_luminance", q(m->min_luminance, '/')); e.str("max_luminance", q(m->max_luminance, '/')); }
    } else if (sd.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL && sd.size >= sizeof(AVContentLightMetadata)) {
        const auto* c = reinterpret_cast<const AVContentLightMetadata*>(sd.data);
        e.num("max_content", c->MaxCLL); e.num("max_average", c->MaxFALL);
    } else if (sd.type == AV_PKT_DATA_AUDIO_SERVICE_TYPE && sd.size >= sizeof(int)) {
        int t; std::memcpy(&t, sd.data, sizeof(int)); e.num("service_type", t);
    }
    return e;
}

const char* fieldOrderName(AVFieldOrder f) {
    switch (f) {
    case AV_FIELD_PROGRESSIVE: return "progressive";
    case AV_FIELD_TT: return "tt";
    case AV_FIELD_BB: return "bb";
    case AV_FIELD_TB: return "tb";
    case AV_FIELD_BT: return "bt";
    default: return nullptr;   // "unknown" is optional: left out
    }
}

Node streamNode(AVFormatContext* fc, AVStream* st, AVCodecContext* dec) {   // show_stream
    const AVCodecParameters* par = st->codecpar;
    Node s;
    s.num("index", st->index);
    if (const AVCodecDescriptor* cd = avcodec_descriptor_get(par->codec_id)) {
        s.str("codec_name", cd->name);
        s.str("codec_long_name", cd->long_name ? cd->long_name : "unknown");
    }
    if (const char* p = avcodec_profile_name(par->codec_id, par->profile)) s.str("profile", p);
    else if (par->profile != AV_PROFILE_UNKNOWN) s.str("profile", QByteArray::number(par->profile));
    if (const char* t = av_get_media_type_string(par->codec_type)) s.str("codec_type", t);
    char tag[AV_FOURCC_MAX_STRING_SIZE] = {0};
    s.str("codec_tag_string", av_fourcc_make_string(tag, par->codec_tag));
    s.str("codec_tag", hex("0x%04x", par->codec_tag));
    {
        AVBPrint bp; av_bprint_init(&bp, 1, AV_BPRINT_SIZE_UNLIMITED);
        if (av_mime_codec_str(par, st->avg_frame_rate, &bp) >= 0 && av_bprint_is_complete(&bp)) s.str("mime_codec_string", QByteArray(bp.str, int(bp.len)));
        av_bprint_finalize(&bp, nullptr);
    }
    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO: {
        s.num("width", par->width); s.num("height", par->height);
        if (dec) { s.num("coded_width", dec->coded_width); s.num("coded_height", dec->coded_height); }
        s.num("has_b_frames", par->video_delay);
        const AVRational sar = av_guess_sample_aspect_ratio(fc, st, nullptr);
        if (sar.num) {
            AVRational dar;
            av_reduce(&dar.num, &dar.den, int64_t(par->width) * sar.num, int64_t(par->height) * sar.den, 1024 * 1024);
            s.str("sample_aspect_ratio", q(sar, ':')); s.str("display_aspect_ratio", q(dar, ':'));
        }
        if (const char* pf = av_get_pix_fmt_name(AVPixelFormat(par->format))) s.str("pix_fmt", pf);
        s.num("level", par->level);
        // print_color_range / _space / _trc, print_primaries, print_chroma_location: "unknown" (unspecified) is optional
        if (const char* v = av_color_range_name(par->color_range); v && par->color_range != AVCOL_RANGE_UNSPECIFIED) s.str("color_range", v);
        if (const char* v = av_color_space_name(par->color_space); v && par->color_space != AVCOL_SPC_UNSPECIFIED) s.str("color_space", v);
        if (const char* v = av_color_transfer_name(par->color_trc); v && par->color_trc != AVCOL_TRC_UNSPECIFIED) s.str("color_transfer", v);
        if (const char* v = av_color_primaries_name(par->color_primaries); v && par->color_primaries != AVCOL_PRI_UNSPECIFIED) s.str("color_primaries", v);
        if (const char* v = av_chroma_location_name(par->chroma_location); v && par->chroma_location != AVCHROMA_LOC_UNSPECIFIED) s.str("chroma_location", v);
        if (const char* f = fieldOrderName(par->field_order)) s.str("field_order", f);
        break;
    }
    case AVMEDIA_TYPE_AUDIO: {
        if (const char* sf = av_get_sample_fmt_name(AVSampleFormat(par->format))) s.str("sample_fmt", sf);
        s.str("sample_rate", QByteArray::number(par->sample_rate));
        s.num("channels", par->ch_layout.nb_channels);
        if (par->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
            char b[128] = {0}; av_channel_layout_describe(&par->ch_layout, b, sizeof(b)); s.str("channel_layout", b);
        }
        s.num("bits_per_sample", av_get_bits_per_sample(par->codec_id));
        s.num("initial_padding", par->initial_padding);
        break;
    }
    case AVMEDIA_TYPE_SUBTITLE:
        if (par->width) s.num("width", par->width);
        if (par->height) s.num("height", par->height);
        break;
    default: break;
    }
    if (dec && dec->codec && dec->codec->priv_class && dec->priv_data) putExported(s, dec->priv_data);
    if (fc->iformat->priv_class && fc->priv_data) putExported(s, fc->priv_data);
    if (fc->iformat->flags & AVFMT_SHOW_IDS) s.str("id", hex("0x%x", unsigned(st->id)));
    s.str("r_frame_rate", q(st->r_frame_rate, '/'));
    s.str("avg_frame_rate", q(st->avg_frame_rate, '/'));
    s.str("time_base", q(st->time_base, '/'));
    putTs(s, "start_pts", st->start_time);
    putTime(s, "start_time", st->start_time, st->time_base);
    putTs(s, "duration_ts", st->duration);
    putTime(s, "duration", st->duration, st->time_base);
    if (par->bit_rate > 0) s.str("bit_rate", QByteArray::number(qint64(par->bit_rate)));
    if (dec && dec->rc_max_rate > 0) s.str("max_bit_rate", QByteArray::number(qint64(dec->rc_max_rate)));
    const int rawBits = dec ? dec->bits_per_raw_sample : par->bits_per_raw_sample;
    if (rawBits > 0) s.str("bits_per_raw_sample", QByteArray::number(rawBits));
    if (st->nb_frames) s.str("nb_frames", QByteArray::number(qint64(st->nb_frames)));
    if (par->extradata_size > 0) s.num("extradata_size", par->extradata_size);
    Node disp;   // print_dispositions: every named bit of the linked libavformat, 0/1
    for (int b = 0; b < 32; ++b) if (const char* n = av_disposition_to_string(int(1u << b))) disp.num(n, (unsigned(st->disposition) >> b) & 1u);
    s.child("disposition", std::move(disp));
    if (st->metadata) s.child("tags", tagsNode(st->metadata));
    if (par->nb_coded_side_data > 0) {
        Node list; list.kind = Node::Arr;
        for (int k = 0; k < par->nb_coded_side_data; ++k) list.kids.push_back(sideDataNode(par->coded_side_data[k]));
        s.child("side_data_list", std::move(list));
    }
    return s;
}

Node formatNode(AVFormatContext* fc, const QByteArray& printName) {   // show_format
    Node f;
    f.str("filename", printName);
    f.num("nb_streams", fc->nb_streams);
    f.num("nb_programs", fc->nb_programs);
    f.num("nb_stream_groups", fc->nb_stream_groups);
    f.str("format_name", fc->iformat->name);
    if (fc->iformat->long_name) f.str("format_long_name", fc->iformat->long_name);
    putTime(f, "start_time", fc->start_time, AV_TIME_BASE_Q);
    putTime(f, "duration", fc->duration, AV_TIME_BASE_Q);
    const int64_t size = fc->pb ? avio_size(fc->pb) : -1;
    if (size >= 0) f.str("size", QByteArray::number(qint64(size)));
    if (fc->bit_rate > 0) f.str("bit_rate", QByteArray::number(qint64(fc->bit_rate)));
    f.num("probe_score", fc->probe_score);
    if (fc->metadata) f.child("tags", tagsNode(fc->metadata));
    return f;
}

// ---------------------------------------------------------------------------------------------
// Opening with a deadline / cancel flag, local files only
// ---------------------------------------------------------------------------------------------
struct Interrupt {
    std::chrono::steady_clock::time_point deadline{};
    bool hasDeadline = false;
    const std::atomic<bool>* cancel = nullptr;
    bool timedOut = false, cancelled = false;
};

int interruptCb(void* opaque) {
    auto* s = static_cast<Interrupt*>(opaque);
    if (s->cancel && s->cancel->load(std::memory_order_relaxed)) { s->cancelled = true; return 1; }
    if (s->hasDeadline && std::chrono::steady_clock::now() >= s->deadline) { s->timedOut = true; return 1; }
    return 0;
}

struct FormatCloser { void operator()(AVFormatContext* fc) const { if (fc) avformat_close_input(&fc); } };
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;

/// avformat_open_input with ffprobe's scan_all_pmts=1, the file-only whitelist and the interrupt callback.
FormatPtr openInput(const QString& path, bool localOnly, Interrupt* irq, int* averror) {
    AVFormatContext* fc = avformat_alloc_context();
    if (!fc) { *averror = AVERROR(ENOMEM); return nullptr; }
    fc->interrupt_callback.callback = interruptCb;
    fc->interrupt_callback.opaque = irq;
    AVDictionary* fo = nullptr;
    av_dict_set(&fo, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    if (localOnly) av_dict_set(&fo, "protocol_whitelist", "file", 0);
    const QByteArray u = path.toUtf8();
    *averror = avformat_open_input(&fc, u.constData(), nullptr, &fo);   // frees fc on failure
    av_dict_free(&fo);
    if (*averror < 0) return nullptr;
    return FormatPtr(fc);
}

void arm(Interrupt& irq, int timeoutMs, const std::atomic<bool>* cancel) {
    irq.cancel = cancel;
    irq.hasDeadline = timeoutMs > 0;
    if (irq.hasDeadline) irq.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
}

} // namespace

ProbeResult probe(const QString& path, const ProbeOptions& opts) {
    ProbeResult r;
    r.text = "{\n\n}\n";      // what ffprobe prints when it cannot open the input
    Interrupt irq; arm(irq, opts.timeoutMs, opts.cancel);
    auto failed = [&](int err, const QString& what) {
        r.averror = err; r.timedOut = irq.timedOut; r.cancelled = irq.cancelled;
        r.error = irq.timedOut ? QString("timeout after %1 ms (%2 %3)").arg(opts.timeoutMs).arg(what, path)
                : irq.cancelled ? QString("cancelled (%1 %2)").arg(what, path)
                                : QString("%1 %2: %3").arg(what, path, avErr(err));
        return r;
    };
    if (path.isEmpty()) return failed(AVERROR(EINVAL), "cannot open");
    int err = 0;
    FormatPtr fc = openInput(path, opts.localOnly, &irq, &err);
    if (!fc) return failed(err, "cannot open");
    if ((err = avformat_find_stream_info(fc.get(), nullptr)) < 0) return failed(err, "no stream info for");
    if (irq.timedOut || irq.cancelled) return failed(AVERROR_EXIT, "probing");

    // bind a decoder to each stream, as ffprobe does (no frames decoded; threads stay at libavcodec's default of 1)
    struct DecFree { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
    std::vector<std::unique_ptr<AVCodecContext, DecFree>> decs(fc->nb_streams);
    if (opts.openDecoders && opts.showStreams) {
        for (unsigned k = 0; k < fc->nb_streams; ++k) {
            AVStream* st = fc->streams[k];
            if (st->codecpar->codec_id == AV_CODEC_ID_PROBE) continue;
            const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!codec) continue;
            std::unique_ptr<AVCodecContext, DecFree> d(avcodec_alloc_context3(codec));
            if (!d || avcodec_parameters_to_context(d.get(), st->codecpar) < 0) continue;
            d->pkt_timebase = st->time_base;
            // ffprobe exits when a decoder does not open; here the stream is just printed without decoder fields
            if (avcodec_open2(d.get(), codec, nullptr) < 0) continue;
            decs[k] = std::move(d);
        }
    }

    Node root;
    if (opts.showStreams) {
        Node streams; streams.kind = Node::Arr;
        for (unsigned k = 0; k < fc->nb_streams; ++k) streams.kids.push_back(streamNode(fc.get(), fc->streams[k], decs[k].get()));
        root.child("streams", std::move(streams));
    }
    if (opts.showFormat) root.child("format", formatNode(fc.get(), validUtf8((opts.printName.isEmpty() ? path : opts.printName).toUtf8().constData())));
    r.ok = true;
    r.json = toJson(root).toObject();
    r.text.clear(); writeTo(r.text, root, 0); r.text += '\n';
    return r;
}

QJsonObject probeJson(const QString& path, const ProbeOptions& opts, QString* error) {
    ProbeResult r = probe(path, opts);
    if (error) *error = r.error;
    return r.ok ? r.json : QJsonObject();
}

double durationOnly(const QString& path, int timeoutMs, QString* error) {
    Interrupt irq; arm(irq, timeoutMs, nullptr);
    int err = 0;
    FormatPtr fc = openInput(path, true, &irq, &err);
    if (!fc) { if (error) *error = irq.timedOut ? QString("timeout after %1 ms").arg(timeoutMs) : "cannot open " + path + ": " + avErr(err); return 0.0; }
    double d = fc->duration > 0 ? double(fc->duration) / AV_TIME_BASE : 0.0;
    if (d <= 0.0 && avformat_find_stream_info(fc.get(), nullptr) >= 0 && fc->duration > 0) d = double(fc->duration) / AV_TIME_BASE;
    if (error) *error = d > 0.0 ? QString() : irq.timedOut ? QString("timeout after %1 ms").arg(timeoutMs) : QString("no duration in " + path);
    return d;
}

QByteArray formatDurationValue(const ProbeResult& r) {
    const QJsonValue d = r.json.value("format").toObject().value("duration");
    return d.isString() ? d.toString().toUtf8() : QByteArray("N/A");
}

} // namespace avkit
