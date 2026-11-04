// PlayPlayMediaTool `thumb`: see thumbnailer.h for the contract and the ffmpeg behaviour it follows.
#include "thumbnailer.h"

#include "probecli.h"      // parseLogLevel, isNonFileInput, oneShotTimeoutMs
#include "ppcli.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace mediatool {
namespace {

QString avErr(int r) { char b[AV_ERROR_MAX_STRING_SIZE] = {0}; av_strerror(r, b, sizeof(b)); return QString::fromUtf8(b); }

// Deadline + cancel hook, polled by libav's I/O (AVIOInterruptCB) and by the decode loop between packets.
struct Irq {
    std::chrono::steady_clock::time_point deadline;
    bool hasDeadline = false;
    const std::function<bool()>* cancel = nullptr;
    bool timedOut = false, cancelled = false;
    bool stop() {
        if (timedOut || cancelled) return true;
        if (cancel && *cancel && (*cancel)()) cancelled = true;
        else if (hasDeadline && std::chrono::steady_clock::now() >= deadline) timedOut = true;
        return timedOut || cancelled;
    }
};
int irqCb(void* p) { return static_cast<Irq*>(p)->stop() ? 1 : 0; }

struct FmtCtx { AVFormatContext* p = nullptr; ~FmtCtx() { if (p) avformat_close_input(&p); } };
struct CodecCtx { AVCodecContext* p = nullptr; ~CodecCtx() { avcodec_free_context(&p); } };
struct Frame { AVFrame* p = av_frame_alloc(); ~Frame() { av_frame_free(&p); } };
struct Packet { AVPacket* p = av_packet_alloc(); ~Packet() { av_packet_free(&p); } };
struct Scaler { SwsContext* p = nullptr; ~Scaler() { sws_free_context(&p); } };

// ffmpeg_mux_init.c map_auto_video: the largest picture wins, +1e8 for a stream that delivered packets, +5e6 for the
// default disposition; an attached picture scores 1 (image2 is not an APIC muxer), so it is used only when alone.
int pickVideoStream(const AVFormatContext* fc) {
    int best = -1; int64_t bestScore = 0;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        int64_t score = int64_t(st->codecpar->width) * st->codecpar->height
                      + 100000000LL * !!(st->event_flags & AVSTREAM_EVENT_FLAG_NEW_PACKETS)
                      + 5000000LL * !!(st->disposition & AV_DISPOSITION_DEFAULT);
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) score = 1;
        if (score > bestScore) { bestScore = score; best = int(i); }
    }
    return best;
}

AVPixelFormat plainTwin(AVPixelFormat f) {   // a YUVJ format = the plain one at full range (same memory layout)
    switch (f) {
    case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P: return AV_PIX_FMT_YUV411P;
    default: return f;
    }
}

// The encoder's best pixel format for this source, as format negotiation picks it (yuv420p -> yuv420p, rgba -> yuv444p),
// with a YUVJ pick replaced by its plain twin when the encoder lists it (the pixels are the same at full range).
AVPixelFormat encoderFormat(const AVCodecContext* enc, const AVCodec* codec, AVPixelFormat src) {
    const void* cfg = nullptr; int n = 0;
    if (avcodec_get_supported_config(enc, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) < 0 || !cfg || n <= 0) return AV_PIX_FMT_YUVJ420P;
    const auto* list = static_cast<const AVPixelFormat*>(cfg);
    const AVPixelFormat best = avcodec_find_best_pix_fmt_of_list(list, src, 0, nullptr);
    const AVPixelFormat twin = plainTwin(best);
    if (twin != best) for (int i = 0; i < n; ++i) if (list[i] == twin) return twin;
    return best == AV_PIX_FMT_NONE ? list[0] : best;
}

bool isYuv(AVPixelFormat f) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
    return d && !(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components >= 3;
}

// ---- autorotate: fftools/ffmpeg_filter.c configure_input_video_filter (on unless -noautorotate, which is not taken)
struct Orientation {
    bool transpose = false; int dir = 0;      // vf_transpose dir: 0 cclock_flip, 1 clock, 2 cclock, 3 clock_flip
    bool hflip = false, vflip = false;
    bool odd = false;                         // an angle ffmpeg turns with the rotate filter, or a degenerate matrix
    double theta = 0;
    bool any() const { return transpose || hflip || vflip; }
    QString filters() const {
        static const char* const dirs[] = {"cclock_flip", "clock", "cclock", "clock_flip"};
        if (transpose) return QStringLiteral("transpose=") + QLatin1String(dirs[dir & 3]);
        return QString(hflip ? "hflip" : "") + (hflip && vflip ? "," : "") + (vflip ? "vflip" : "");
    }
};

// The display matrix ffmpeg's input filter sees: the decoded frame's (decoders attach the stream's matrix, and the JPEG
// decoder a file's EXIF orientation, to every frame), else the stream's own. get_rotation() (cmdutils.c) makes it
// theta = -round(rotation) folded into [0, 360); then ffmpeg's branches, in its order.
Orientation orientationOf(const AVFrame* f, const AVStream* st) {
    Orientation o;
    const int32_t* m = nullptr;
    if (const AVFrameSideData* sd = av_frame_side_data_get(f->side_data, f->nb_side_data, AV_FRAME_DATA_DISPLAYMATRIX); sd && sd->size >= 9 * sizeof(int32_t))
        m = reinterpret_cast<const int32_t*>(sd->data);
    else if (const AVPacketSideData* pd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
             pd && pd->size >= 9 * sizeof(int32_t))
        m = reinterpret_cast<const int32_t*>(pd->data);
    if (!m) return o;
    double theta = -std::round(av_display_rotation_get(m));
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    o.theta = theta;
    if (!std::isfinite(theta)) o.odd = true;                                   // no rotation defined: not guessed at
    else if (std::fabs(theta - 90) < 1.0) { o.transpose = true; o.dir = m[3] > 0 ? 0 : 1; }
    else if (std::fabs(theta - 180) < 1.0) { o.hflip = m[0] < 0; o.vflip = m[4] < 0; }
    else if (std::fabs(theta - 270) < 1.0) { o.transpose = true; o.dir = m[3] < 0 ? 3 : 2; }
    else if (std::fabs(theta) > 1.0) o.odd = true;
    else o.vflip = m[4] < 0;
    return o;
}

// The formats vf_transpose (vf_hflip / vf_vflip for a flip) take as they are. For any other one ffmpeg's format
// negotiation converts the picture first (a 4:2:2 picture turned 90 degrees ends up 4:4:4), which is not reproduced.
bool orientKeepsFormat(AVPixelFormat f, bool transpose) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BAYER))) return false;
    if (d->log2_chroma_w != d->log2_chroma_h) return !transpose && d->comp[0].plane != d->comp[1].plane;   // flips: planar only
    return true;
}

// One plane, pixels of N bytes. vf_transpose: out(x, y) = in(row x, column y), the rows read bottom-up for dir & 1 and
// written bottom-up for dir & 2; the flips mirror columns / rows. A lossless copy, so the scaler sees ffmpeg's picture.
template <size_t N>
void remapPlane(const uint8_t* src, ptrdiff_t sls, int iw, int ih, uint8_t* dst, ptrdiff_t dls, int ow, int oh, const Orientation& g) {
    for (int y = 0; y < oh; ++y) {
        uint8_t* d = dst + y * dls;
        if (g.transpose) {
            const uint8_t* s = src + size_t((g.dir & 2) ? iw - 1 - y : y) * N;
            for (int x = 0; x < ow; ++x) std::memcpy(d + size_t(x) * N, s + ((g.dir & 1) ? ih - 1 - x : x) * sls, N);
        } else {
            const uint8_t* s = src + (g.vflip ? ih - 1 - y : y) * sls;
            if (!g.hflip) std::memcpy(d, s, size_t(ow) * N);
            else for (int x = 0; x < ow; ++x) std::memcpy(d + size_t(x) * N, s + size_t(iw - 1 - x) * N, N);
        }
    }
}

// The autorotated copy of `in` (a format orientKeepsFormat() accepts), with the applied display matrix dropped.
int orientFrame(const AVFrame* in, AVFrame* out, const Orientation& g) {
    const AVPixelFormat fmt = AVPixelFormat(in->format);
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    out->format = in->format;
    out->width = g.transpose ? in->height : in->width;
    out->height = g.transpose ? in->width : in->height;
    int err = av_frame_get_buffer(out, 0);
    if (err >= 0) err = av_frame_copy_props(out, in);
    if (err < 0) return err;
    av_frame_remove_side_data(out, AV_FRAME_DATA_DISPLAYMATRIX);
    int steps[4] = {0, 0, 0, 0};
    av_image_fill_max_pixsteps(steps, nullptr, d);
    for (int p = 0, n = av_pix_fmt_count_planes(fmt); p < n; ++p) {
        const bool chroma = p == 1 || p == 2;                 // as vf_transpose sizes planes (alpha is full size)
        const int hs = chroma ? d->log2_chroma_w : 0, vs = chroma ? d->log2_chroma_h : 0;
        const int iw = AV_CEIL_RSHIFT(in->width, hs), ih = AV_CEIL_RSHIFT(in->height, vs);
        const int ow = AV_CEIL_RSHIFT(out->width, hs), oh = AV_CEIL_RSHIFT(out->height, vs);
        const uint8_t* s = in->data[p]; uint8_t* o = out->data[p];
        const ptrdiff_t sl = in->linesize[p], dl = out->linesize[p];
        switch (steps[p]) {
        case 1: remapPlane<1>(s, sl, iw, ih, o, dl, ow, oh, g); break;
        case 2: remapPlane<2>(s, sl, iw, ih, o, dl, ow, oh, g); break;
        case 3: remapPlane<3>(s, sl, iw, ih, o, dl, ow, oh, g); break;
        case 4: remapPlane<4>(s, sl, iw, ih, o, dl, ow, oh, g); break;
        case 6: remapPlane<6>(s, sl, iw, ih, o, dl, ow, oh, g); break;
        case 8: remapPlane<8>(s, sl, iw, ih, o, dl, ow, oh, g); break;
        default: return AVERROR_PATCHWELCOME;
        }
    }
    return 0;
}

// The stateful swscale API, for a pair the dynamic sws_scale_frame() path refuses.
int scaleLegacy(const AVFrame* src, AVFrame* dst) {
    SwsContext* c = sws_getContext(src->width, src->height, AVPixelFormat(src->format), dst->width, dst->height, AVPixelFormat(dst->format),
                                   SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!c) return AVERROR(EINVAL);
    const int cs = src->colorspace == AVCOL_SPC_UNSPECIFIED || src->colorspace == AVCOL_SPC_RGB ? SWS_CS_DEFAULT : int(src->colorspace);
    const int* coef = sws_getCoefficients(cs);
    sws_setColorspaceDetails(c, coef, src->color_range == AVCOL_RANGE_JPEG ? 1 : 0, coef, dst->color_range == AVCOL_RANGE_JPEG ? 1 : 0, 0, 1 << 16, 1 << 16);
    int r = av_frame_get_buffer(dst, 0);
    if (r >= 0) r = sws_scale(c, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    sws_freeContext(c);
    return r < 0 ? r : 0;
}

bool parseScale(const QString& f, int* w, int* h) {
    if (!f.startsWith("scale=")) return false;
    const QStringList p = f.mid(6).split(':');
    if (p.size() != 2) return false;
    auto num = [](QString s, const char* key, int* out) {
        if (s.startsWith(QLatin1String(key))) s = s.mid(2);
        bool ok = false; const int v = s.toInt(&ok);
        if (ok) *out = v;
        return ok;
    };
    return num(p[0], "w=", w) && num(p[1], "h=", h);
}

} // namespace

bool scaleSize(int inW, int inH, int reqW, int reqH, int* outW, int* outH) {
    if (inW <= 0 || inH <= 0) return false;
    int w = reqW == 0 ? inW : reqW, h = reqH == 0 ? inH : reqH;   // 0 = the input size (scale filter docs)
    const int fw = w < -1 ? -w : 1, fh = h < -1 ? -h : 1;
    if (w < 0 && h < 0) { w = inW; h = inH; }
    if (w < 0) w = int(av_rescale(h, inW, int64_t(inH) * fw) * fw);
    if (h < 0) h = int(av_rescale(w, inH, int64_t(inW) * fh) * fh);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    *outW = w; *outH = h;
    return true;
}

ThumbResult makeThumbnail(const QString& in, const QString& out, const ThumbOptions& o) {
    ThumbResult r;
    Irq irq;
    irq.cancel = &o.cancelled;
    irq.hasDeadline = o.timeoutMs > 0;
    if (irq.hasDeadline) irq.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.timeoutMs);
    auto fail = [&](int err, const QString& what) -> ThumbResult {
        r.ok = false; r.averror = err; r.timedOut = irq.timedOut; r.cancelled = irq.cancelled;
        r.error = irq.timedOut ? QString("timeout after %1 ms (%2 %3)").arg(o.timeoutMs).arg(what, in)
                : irq.cancelled ? QString("cancelled (%1 %2)").arg(what, in)
                : err ? QString("%1 %2: %3").arg(what, in, avErr(err)) : QString("%1 %2").arg(what, in);
        return r;
    };
    auto refuse = [&](const QString& what) -> ThumbResult {   // ffmpeg would do something not reproduced here: exit 64
        r.ok = false; r.unsupported = true; r.error = what + " " + in;
        return r;
    };
    if (in.isEmpty()) return fail(AVERROR(EINVAL), "no input");
    if (out.isEmpty()) return fail(AVERROR(EINVAL), "no output for");
    if (!o.overwrite && QFileInfo::exists(out)) { r.error = QString("File '%1' already exists. Exiting.").arg(out); r.averror = AVERROR(EEXIST); return r; }
    if (!(o.qscale > 0)) return fail(AVERROR(EINVAL), "bad qscale for");

    // ---- open: ffmpeg's scan_all_pmts=1, local files only, the interrupt callback
    FmtCtx fc;
    fc.p = avformat_alloc_context();
    if (!fc.p) return fail(AVERROR(ENOMEM), "cannot open");
    fc.p->interrupt_callback.callback = irqCb;
    fc.p->interrupt_callback.opaque = &irq;
    AVDictionary* fo = nullptr;
    av_dict_set(&fo, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    if (o.localOnly) av_dict_set(&fo, "protocol_whitelist", "file", 0);
    int err = avformat_open_input(&fc.p, in.toUtf8().constData(), nullptr, &fo);   // frees the context on failure
    av_dict_free(&fo);
    if (err < 0) return fail(err, "cannot open");
    if ((err = avformat_find_stream_info(fc.p, nullptr)) < 0) return fail(err, "no stream info for");
    if (irq.stop()) return fail(AVERROR_EXIT, "probing");

    const int vs = pickVideoStream(fc.p);
    if (vs < 0) return fail(0, "no video stream in");
    r.stream = vs;
    AVStream* st = fc.p->streams[vs];
    for (unsigned i = 0; i < fc.p->nb_streams; ++i) if (int(i) != vs) fc.p->streams[i]->discard = AVDISCARD_ALL;

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return fail(0, QString("no decoder for %1 in").arg(QString::fromLatin1(avcodec_get_name(st->codecpar->codec_id))));
    CodecCtx dec;
    dec.p = avcodec_alloc_context3(codec);
    if (!dec.p || (err = avcodec_parameters_to_context(dec.p, st->codecpar)) < 0) return fail(err ? err : AVERROR(ENOMEM), "decoder for");
    dec.p->pkt_timebase = st->time_base;
    dec.p->thread_count = std::max(1, o.threads);
    if ((err = avcodec_open2(dec.p, codec, nullptr)) < 0) return fail(err, "cannot open the decoder for");

    // ---- input -ss (ffmpeg_demux.c): seek to the keyframe before t; frames before t are dropped after decoding
    int64_t startPts = AV_NOPTS_VALUE;
    if (o.seekSec >= 0) {
        int64_t ts = std::llround(o.seekSec * AV_TIME_BASE);
        if (fc.p->start_time != AV_NOPTS_VALUE) ts += fc.p->start_time;
        int64_t seekTs = ts;
        if (!(fc.p->iformat->flags & AVFMT_SEEK_TO_PTS)) {
            for (unsigned i = 0; i < fc.p->nb_streams; ++i)
                if (fc.p->streams[i]->codecpar->video_delay) { seekTs -= 3 * AV_TIME_BASE / 23; break; }   // the dts heuristic
        }
        if (avformat_seek_file(fc.p, -1, INT64_MIN, seekTs, seekTs, 0) < 0)
            av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %.3f\n", in.toUtf8().constData(), double(ts) / AV_TIME_BASE);
        startPts = av_rescale_q(ts, AV_TIME_BASE_Q, st->time_base);
    }

    // ---- decode to the first frame at or after the target
    Packet pkt;
    Frame frame;
    bool got = false, flushed = false;
    int decodeErrors = 0;
    while (!got) {
        if (irq.stop()) return fail(AVERROR_EXIT, "decoding");
        int rr = avcodec_receive_frame(dec.p, frame.p);
        if (rr == 0) {
            int64_t pts = frame.p->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame.p->pts;
            if (startPts == AV_NOPTS_VALUE || (pts != AV_NOPTS_VALUE && pts >= startPts)) {
                got = true;
                if (pts != AV_NOPTS_VALUE)
                    r.frameSec = double(pts) * av_q2d(st->time_base) - (fc.p->start_time != AV_NOPTS_VALUE ? double(fc.p->start_time) / AV_TIME_BASE : 0.0);
                break;
            }
            av_frame_unref(frame.p);
            continue;
        }
        if (rr == AVERROR_EOF) break;
        if (rr != AVERROR(EAGAIN)) return fail(rr, "decoding");
        if (flushed) break;                                   // defensive: a drained decoder that never says EOF
        rr = av_read_frame(fc.p, pkt.p);
        if (rr < 0) {
            if (irq.stop()) return fail(AVERROR_EXIT, "reading");
            avcodec_send_packet(dec.p, nullptr);              // end of file (or a read error ffmpeg also treats as the end)
            flushed = true;
            continue;
        }
        if (pkt.p->stream_index == vs) {
            const int s = avcodec_send_packet(dec.p, pkt.p);
            // like ffmpeg, a corrupt packet is skipped, not fatal; a file that is only garbage still ends
            if (s < 0 && s != AVERROR(EAGAIN) && ++decodeErrors > 500) { av_packet_unref(pkt.p); return fail(s, "decoding"); }
        }
        av_packet_unref(pkt.p);
    }
    if (!got) {
        if (o.seekSec >= 0) return fail(0, QString("no video frame at or after %1 s in").arg(o.seekSec, 0, 'f', 3));
        return fail(0, "no video frame decoded from");
    }
    r.srcWidth = frame.p->width; r.srcHeight = frame.p->height;

    // ---- autorotate, which ffmpeg puts in front of -vf: a portrait phone video or photo comes out portrait
    const Orientation geo = orientationOf(frame.p, st);
    if (geo.odd) return refuse(QString("a display rotation of %1 degrees (ffmpeg's rotate filter) in").arg(geo.theta));
    if (geo.any() && !orientKeepsFormat(AVPixelFormat(frame.p->format), geo.transpose))
        return refuse(QString("%1 of a %2 picture (ffmpeg converts the format first) in").arg(geo.filters(), QString::fromLatin1(av_get_pix_fmt_name(AVPixelFormat(frame.p->format)))));
    r.autorotate = geo.filters();

    // the picture ffmpeg's scale filter gets; a YUVJ frame goes on as its plain twin at full range (the same planes;
    // newer swscale prefers it)
    Frame src;
    if ((err = geo.any() ? orientFrame(frame.p, src.p, geo) : av_frame_ref(src.p, frame.p)) < 0) return fail(err, "frame of");
    if (plainTwin(AVPixelFormat(src.p->format)) != AVPixelFormat(src.p->format)) {
        src.p->format = plainTwin(AVPixelFormat(src.p->format));
        src.p->color_range = AVCOL_RANGE_JPEG;
    }
    if (irq.stop()) return fail(AVERROR_EXIT, "rotating");

    // ---- scale (vf_scale's size rule and SAR, bicubic) into the encoder's format at full range
    int ow = 0, oh = 0;
    if (!scaleSize(src.p->width, src.p->height, o.width, o.height, &ow, &oh))
        return fail(AVERROR(EINVAL), QString("cannot scale %1x%2 to %3:%4 for").arg(src.p->width).arg(src.p->height).arg(o.width).arg(o.height));
    const AVCodec* mjpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!mjpeg) return fail(AVERROR_ENCODER_NOT_FOUND, "no mjpeg encoder for");
    CodecCtx enc;
    enc.p = avcodec_alloc_context3(mjpeg);
    if (!enc.p) return fail(AVERROR(ENOMEM), "encoder for");

    const AVPixelFormat encFmt = encoderFormat(enc.p, mjpeg, AVPixelFormat(src.p->format));
    AVRational sar = av_guess_sample_aspect_ratio(fc.p, st, frame.p);
    if (geo.transpose && sar.num > 0 && sar.den > 0) sar = av_div_q(AVRational{1, 1}, sar);   // vf_transpose inverts it
    if (sar.num > 0 && sar.den > 0) sar = av_mul_q(AVRational{oh * src.p->width, ow * src.p->height}, sar);
    else sar = AVRational{0, 1};

    auto initDst = [&](AVFrame* d) {
        d->format = encFmt; d->width = ow; d->height = oh;
        d->color_range = AVCOL_RANGE_JPEG;
        d->colorspace = isYuv(encFmt) && (src.p->colorspace == AVCOL_SPC_RGB || !isYuv(AVPixelFormat(src.p->format))) ? AVCOL_SPC_UNSPECIFIED : src.p->colorspace;
        d->color_primaries = src.p->color_primaries; d->color_trc = src.p->color_trc;
        d->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
        d->sample_aspect_ratio = sar;
    };
    // Dynamic sws_scale_frame() with the chroma siting left unspecified on both sides: measured byte-identical to
    // ffmpeg.exe's JPEG on every fixture (H.264 4:2:0 / 4:2:2 10-bit / 4:4:4, full-range, anamorphic, MPEG-2 480i,
    // MPEG-4, PNG / JPEG / WebP stills). With the decoder's "left" siting kept, downscales land a few code values off;
    // the stateful API matches downscales but not same-size range conversion. The stateful API stays the fallback for
    // a pair the dynamic one refuses.
    Frame dst;
    initDst(dst.p);
    src.p->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    Scaler sws;
    sws.p = sws_alloc_context();
    if (!sws.p) return fail(AVERROR(ENOMEM), "scaler for");
    sws.p->flags = SWS_BICUBIC;
    sws.p->threads = 1;
    if ((err = sws_scale_frame(sws.p, dst.p, src.p)) < 0) {
        av_frame_unref(dst.p);
        initDst(dst.p);
        if ((err = scaleLegacy(src.p, dst.p)) < 0)
            return fail(err, QString("cannot convert %1 to %2 for").arg(QString::fromLatin1(av_get_pix_fmt_name(AVPixelFormat(src.p->format))), QString::fromLatin1(av_get_pix_fmt_name(encFmt))));
    }
    if (irq.stop()) return fail(AVERROR_EXIT, "scaling");

    // ---- mjpeg at qscale, as ffmpeg's -q:v sets it (global_quality, and the frame's quality from it)
    enc.p->width = ow; enc.p->height = oh;
    enc.p->pix_fmt = encFmt;
    enc.p->color_range = AVCOL_RANGE_JPEG;
    enc.p->colorspace = dst.p->colorspace; enc.p->color_primaries = dst.p->color_primaries; enc.p->color_trc = dst.p->color_trc;
    enc.p->chroma_sample_location = dst.p->chroma_location;
    enc.p->sample_aspect_ratio = sar;
    const AVRational fps = av_guess_frame_rate(fc.p, st, frame.p);
    enc.p->time_base = fps.num > 0 && fps.den > 0 ? av_inv_q(fps) : AVRational{1, 25};
    enc.p->flags |= AV_CODEC_FLAG_QSCALE;
    enc.p->global_quality = int(FF_QP2LAMBDA * o.qscale);
    if ((err = avcodec_open2(enc.p, mjpeg, nullptr)) < 0) return fail(err, "cannot open the mjpeg encoder for");
    dst.p->quality = enc.p->global_quality;
    dst.p->pict_type = AV_PICTURE_TYPE_NONE;
    dst.p->pts = 0;
    if ((err = avcodec_send_frame(enc.p, dst.p)) < 0) return fail(err, "encoding");
    avcodec_send_frame(enc.p, nullptr);
    Packet jpg;
    if ((err = avcodec_receive_packet(enc.p, jpg.p)) < 0) return fail(err, "encoding");

    QSaveFile f(out);
    if (!f.open(QIODevice::WriteOnly)) return fail(0, QString("cannot write %1 (%2) for").arg(out, f.errorString()));
    if (f.write(reinterpret_cast<const char*>(jpg.p->data), jpg.p->size) != jpg.p->size)
        return fail(0, QString("cannot write %1 (%2) for").arg(out, f.errorString()));
    // last look before the JPEG appears under its name: a job the serve loop timed out or cancelled meanwhile has had
    // its {"ok": false} reply, so it must leave no file (the uncommitted QSaveFile drops its temporary file)
    if (irq.stop()) return fail(AVERROR_EXIT, "writing");
    if (!f.commit()) return fail(0, QString("cannot write %1 (%2) for").arg(out, f.errorString()));
    r.ok = true;
    r.width = ow; r.height = oh;
    r.bytes = jpg.p->size;
    r.pixFmt = QString::fromLatin1(av_get_pix_fmt_name(encFmt));
    return r;
}

QJsonObject thumbJson(const ThumbResult& r, const QString& out, const ThumbOptions& o) {
    if (r.unsupported) return ppcli::unsupportedResult(r.error);
    if (!r.ok) {
        QJsonObject e = ppcli::failure(r.error);
        e["averror"] = r.averror;
        if (r.timedOut) e["timeout"] = true;
        if (r.cancelled) e["cancelled"] = true;
        return e;
    }
    QJsonObject j{{"ok", true}, {"out", QDir::toNativeSeparators(QFileInfo(out).absoluteFilePath())},
                  {"width", r.width}, {"height", r.height}, {"srcWidth", r.srcWidth}, {"srcHeight", r.srcHeight},
                  {"at", o.seekSec >= 0 ? o.seekSec : 0.0}, {"frameTime", r.frameSec}, {"bytes", double(r.bytes)},
                  {"pixFmt", r.pixFmt}};
    if (!r.autorotate.isEmpty()) j["autorotate"] = r.autorotate;
    return j;
}

ThumbCommand parseThumbArgs(const QStringList& args) {
    ThumbCommand c;
    c.opt.width = c.opt.height = -1;          // no -vf: the input size, as ffmpeg keeps it
    bool sawInput = false, sawY = false, sawN = false, sawFrames = false, sawQ = false, explicitFormat = false, sawSs = false;
    QStringList outputs;
    for (int i = 0; i < args.size() && c.unsupported.isEmpty() && c.error.isEmpty(); ++i) {
        const QString& a = args[i];
        auto value = [&](QString* v) {
            if (i + 1 >= args.size()) { c.error = QString("Missing argument for option '%1'.").arg(a.mid(1)); return false; }
            *v = args[++i];
            return true;
        };
        // Where ffmpeg takes an option decides what it does. Global options go anywhere. A per-file option after the
        // output file is ignored by ffmpeg ("Trailing option(s) found"); an output option before -i is refused by it
        // (applied to the input). Either way this tool would differ, so both fall back.
        const bool global = a == "-y" || a == "-n" || a == "-v" || a == "-loglevel" || a == "-hide_banner" || a == "-nostdin" || a == "-nostats";
        if (!global && !outputs.isEmpty()) { c.unsupported = a + " after the output file"; break; }
        auto outputOption = [&] {
            if (!sawInput) c.unsupported = a + " before -i (ffmpeg would apply it to the input)";
            return sawInput;
        };
        QString v;
        if (a == "-y") sawY = true;
        else if (a == "-n") sawN = true;
        else if (a == "-v" || a == "-loglevel") { if (value(&v) && !parseLogLevel(v, &c.logLevel)) c.error = "Invalid loglevel \"" + v + "\""; }
        else if (a == "-hide_banner" || a == "-nostdin" || a == "-nostats" || a == "-an" || a == "-sn" || a == "-dn") {}
        else if (a == "-ss") {
            // before -i an input seek, after it an output seek: the same first frame at or after t; both = two seeks
            if (sawSs) { c.unsupported = "-ss given twice (an input and an output seek)"; break; }
            sawSs = true;
            if (!value(&v)) break;
            int64_t us = 0;
            if (av_parse_time(&us, v.toUtf8().constData(), 1) < 0 || us < 0) c.error = "Invalid duration specification for ss: " + v;
            else c.opt.seekSec = double(us) / AV_TIME_BASE;
        }
        else if (a == "-i") {
            if (!value(&v)) break;
            if (sawInput) c.unsupported = "-i " + v + " (a second input)";
            else if (!outputs.isEmpty()) c.unsupported = "output " + outputs.first() + " before -i";
            else if (isNonFileInput(v)) c.unsupported = "input " + v;
            else { c.in = v; sawInput = true; }
        }
        else if (a == "-frames:v" || a == "-vframes" || a == "-frames") { if (outputOption() && value(&v)) { if (v == "1") sawFrames = true; else c.unsupported = a + " " + v; } }
        else if (a == "-vf" || a == "-filter:v") { if (outputOption() && value(&v) && !parseScale(v, &c.opt.width, &c.opt.height)) c.unsupported = a + " " + v; }
        else if (a == "-q:v" || a == "-qscale:v" || a == "-q" || a == "-qscale") {
            if (!outputOption() || !value(&v)) break;
            bool ok = false; const double q = v.toDouble(&ok);
            if (!ok || !(q >= 1 && q <= 31)) c.error = a + " " + v + ": expected 1..31"; else { c.opt.qscale = q; sawQ = true; }
        }
        else if (a == "-update") { if (outputOption() && value(&v) && v != "1" && v != "true") c.unsupported = a + " " + v; }
        else if (a == "-f") {
            if (!value(&v)) break;
            if (!sawInput) c.unsupported = "-f " + v + " (input format)";
            else if (v == "image2" || v == "mjpeg") explicitFormat = true;
            else c.unsupported = "-f " + v;
        }
        else if (a == "-c:v" || a == "-vcodec" || a == "-codec:v") { if (outputOption() && value(&v) && v != "mjpeg") c.unsupported = a + " " + v; }   // before -i: a decoder
        else if (a.size() > 1 && a.startsWith('-')) c.unsupported = a;
        else if (!sawInput) c.unsupported = "output " + a + " before -i";
        else outputs << a;
    }
    if (!c.unsupported.isEmpty() || !c.error.isEmpty()) return c;
    if (sawY && sawN) { c.error = "Error, both -y and -n supplied. Exiting."; return c; }
    c.opt.overwrite = sawY;                   // without -y ffmpeg asks; with no terminal to answer, that is -n
    if (!sawInput) { c.error = "no input (-i <file>)"; return c; }
    if (outputs.isEmpty()) { c.error = "At least one output file must be specified"; return c; }
    if (outputs.size() > 1) { c.unsupported = "output " + outputs.at(1) + " (more than one output)"; return c; }
    c.out = outputs.first();
    if (isNonFileInput(c.out)) { c.unsupported = "output " + c.out; return c; }
    const QString ext = QFileInfo(c.out).suffix().toLower();
    if (!explicitFormat && ext != "jpg" && ext != "jpeg") { c.unsupported = "output format of " + c.out + " (only .jpg)"; return c; }
    // without these ffmpeg would write every frame / pick the bitrate-driven quality: not the thumbnail contract
    if (!sawFrames) { c.unsupported = "an output without -frames:v 1"; return c; }
    if (!sawQ) { c.unsupported = "mjpeg without -q:v"; return c; }
    return c;
}

int thumbCli(const QStringList& args) {
    const ThumbCommand c = parseThumbArgs(args);
    if (!c.unsupported.isEmpty()) return ppcli::unsupported(c.unsupported);
    if (!c.error.isEmpty()) return ppcli::fail(c.error);
    av_log_set_level(c.logLevel);
    ThumbOptions o = c.opt;
    o.timeoutMs = oneShotTimeoutMs();
    const ThumbResult r = makeThumbnail(c.in, c.out, o);
    if (!r.ok) av_log(nullptr, AV_LOG_ERROR, "%s\n", r.error.toUtf8().constData());
    return ppcli::finish(thumbJson(r, c.out, o));
}

} // namespace mediatool
