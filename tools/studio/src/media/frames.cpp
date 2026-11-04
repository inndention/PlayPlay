// Frame access on libav (ST-17). See frames.h.
#include "media/frames.h"

#include "media/avopen.h"
#include "media/imageops.h"   // orientImage

#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace studio::media {

struct FrameSampler::Impl {
    av::Deadline dl;                           // before `in`: the interrupt callback points at it
    std::unique_ptr<av::Input> in;
    AVFormatContext* fc = nullptr;
    AVStream* st = nullptr;
    int vs = -1;
    av::CodecPtr cc;
    av::PacketPtr pkt;
    av::FramePtr cur, pending, tmp;            // cur: on screen at the last target; pending: the first frame after it
    bool haveCur = false, havePending = false, eof = false, drained = false;
    int64_t curPts = AV_NOPTS_VALUE, pendingPts = AV_NOPTS_VALUE;
    AVRational tb{1, 1};
    int64_t startPts = 0, halfFrame = 1;
    double durationSec = 0, fpsValue = 25, seekAhead = 4.0, lastT = -1;
    qint64 decoded = 0;
    bool opened = false;
    QString error;
    bool autoRotate = true;
    int streamOrient = 1;                      // EXIF orientation of the stream's display matrix (1 = upright)
    // hard decode errors: `damaged` = the picture on screen was lost to one (until the next good frame)
    static constexpr int kMaxRestarts = 8;     // scrub.ts MAX_RESTARTS
    int restarts = 0;
    bool damaged = false, broken = false;
    QString brokenError;
    // scaler and an aligned staging buffer (sws writes SIMD-wide rows)
    av::SwsPtr sws;
    int sSrcW = 0, sSrcH = 0, sSrcFmt = -1, sDstW = 0, sDstH = 0, sFlags = 0, sRange = -1, sSpace = -1;
    uint8_t* buf[4] = {nullptr, nullptr, nullptr, nullptr};
    int lines[4] = {0, 0, 0, 0};
    int bufW = 0, bufH = 0, bufFmt = -1;

    Impl(const QString& path, const Options& o) : dl(o.timeoutMs, o.cancel), seekAhead(qMax(0.0, o.seekAheadSec)) {
        in = av::openFile(path, &dl, &error);
        if (!in) return;
        fc = in->fc();
        vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs >= 0 && (fc->streams[vs]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            vs = -1;   // cover art is a picture, not the video; look for a real video stream
            for (unsigned i = 0; i < fc->nb_streams; ++i)
                if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(fc->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) { vs = int(i); break; }
        }
        if (vs < 0) { error = QStringLiteral("no video stream"); return; }
        st = fc->streams[vs];
        autoRotate = o.autoRotate;
        if (autoRotate)
            if (const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX))
                streamOrient = av::displayOrientation(sd->data, sd->size);
        for (unsigned i = 0; i < fc->nb_streams; ++i) if (int(i) != vs) fc->streams[i]->discard = AVDISCARD_ALL;   // demux video only
        cc = av::openDecoder(st, qMax(0, o.threads), &error);
        if (!cc) return;
        pkt = av::newPacket(); cur = av::newFrame(); pending = av::newFrame(); tmp = av::newFrame();
        if (!pkt || !cur || !pending || !tmp) { error = QStringLiteral("out of memory"); return; }
        tb = st->time_base;
        if (tb.num <= 0 || tb.den <= 0) tb = AVRational{1, 90000};
        startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        AVRational fr = av_guess_frame_rate(fc, st, nullptr);
        if (fr.num <= 0 || fr.den <= 0) fr = AVRational{25, 1};
        fpsValue = av_q2d(fr);
        halfFrame = qMax<int64_t>(1, av_rescale_q(1, av_inv_q(fr), tb) / 2);
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) durationSec = double(st->duration) * av_q2d(tb);
        else if (fc->duration != AV_NOPTS_VALUE && fc->duration > 0) durationSec = double(fc->duration) / AV_TIME_BASE;
        const QString fmt = in->formatName();
        if (fmt.endsWith(QLatin1String("_pipe")) || fmt == QLatin1String("image2pipe")) durationSec = 0;   // one picture
        opened = true;
    }

    ~Impl() { if (buf[0]) av_freep(&buf[0]); }

    int64_t framePts(const AVFrame* f) const {
        if (f->best_effort_timestamp != AV_NOPTS_VALUE) return f->best_effort_timestamp;
        if (f->pts != AV_NOPTS_VALUE) return f->pts;
        return curPts != AV_NOPTS_VALUE ? curPts + 2 * halfFrame : startPts;
    }

    // The frame's own display matrix (decoders attach the stream's, or an H.264 / HEVC orientation SEI), else the
    // stream's.
    int orientOf(const AVFrame* f) const {
        if (!autoRotate) return 1;
        if (const AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_DISPLAYMATRIX)) return av::displayOrientation(sd->data, sd->size);
        return streamOrient;
    }

    // Seeking forward pays only when the keyframe before the target lies past the frame already decoded; with a long
    // GOP (OBS records 250 frames by default) it is often behind us, and the seek would decode those frames again.
    // Without an index covering the target (MPEG-TS has none; FLV / Matroska build theirs while reading) the time rule
    // alone decides. Index times are dts, a frame or two before pts: close enough for this choice; the decode loop
    // finds the exact frame either way.
    bool keyframeAhead(int64_t pos, int64_t target) const {
        const int n = avformat_index_get_entries_count(st);
        const AVIndexEntry* last = n > 0 ? avformat_index_get_entry(st, n - 1) : nullptr;
        if (!last || last->timestamp < target) return true;
        const AVIndexEntry* e = avformat_index_get_entry_from_timestamp(st, target, AVSEEK_FLAG_BACKWARD);
        return !e || e->timestamp > pos;
    }

    // 1 = a frame in `out`, 0 = end of stream, < 0 = error (AVERROR_EXIT: deadline / cancel).
    int decodeNext(AVFrame* out) {
        for (int guard = 0; guard < 1000000; ++guard) {
            if (dl.expired()) return AVERROR_EXIT;
            int r = avcodec_receive_frame(cc.get(), out);
            if (r == 0) { ++decoded; return 1; }
            if (r == AVERROR_EOF) { eof = true; return 0; }
            if (r == AVERROR_INVALIDDATA) continue;           // a damaged frame: skip it
            if (r != AVERROR(EAGAIN)) return r;
            if (drained) { eof = true; return 0; }
            r = av_read_frame(fc, pkt.get());
            if (r < 0) {
                if (dl.expired()) return AVERROR_EXIT;
                avcodec_send_packet(cc.get(), nullptr);         // end of file (or a damaged tail): drain the decoder
                drained = true;
                continue;
            }
            if (pkt->stream_index == vs) {
                const int s = avcodec_send_packet(cc.get(), pkt.get());
                if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_INVALIDDATA) { av_packet_unref(pkt.get()); return s; }
            }
            av_packet_unref(pkt.get());
        }
        return AVERROR_BUG;
    }

    void resetDecodeState() {
        avcodec_flush_buffers(cc.get());
        av_frame_unref(cur.get()); av_frame_unref(pending.get());
        haveCur = havePending = eof = drained = damaged = false;   // a seek lands on a clean keyframe
        curPts = pendingPts = AV_NOPTS_VALUE;
    }

    // A hard decode error (not a damaged frame the decoder skips by itself, not the end of the file): this target
    // fails, the decoder is flushed and the next call goes on from the next packet. The last good picture is dropped
    // (curPts stays as the decode position), so the targets before the next good frame fail too instead of quietly
    // repeating it. After kMaxRestarts the file counts as unreadable from here on.
    const AVFrame* damage(int r, double t) {
        avcodec_flush_buffers(cc.get());
        av_frame_unref(cur.get());
        haveCur = false;
        damaged = true;
        error = QStringLiteral("decode error at %1 s: %2").arg(t, 0, 'f', 3).arg(av::errText(r));
        if (++restarts > kMaxRestarts) {
            broken = true;
            brokenError = QStringLiteral("gave up after %1 decode errors (last: %2)").arg(restarts).arg(av::errText(r));
            error = brokenError;
        }
        return nullptr;
    }

    bool seekTo(int64_t target) {
        int r = av_seek_frame(fc, vs, target, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = avformat_seek_file(fc, vs, INT64_MIN, target, target, 0);
        if (r < 0) r = av_seek_frame(fc, vs, startPts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        resetDecodeState();
        return r >= 0;
    }

    const AVFrame* frameAt(double t) {
        error.clear();
        if (!opened) { error = error.isEmpty() ? QStringLiteral("not open") : error; return nullptr; }
        if (dl.expired()) { error = dl.reason(); return nullptr; }
        if (broken) { error = brokenError; return nullptr; }
        if (!std::isfinite(t) || t < 0) t = 0;
        const int64_t target = startPts + int64_t(std::llround(t / av_q2d(tb)));
        if (havePending && pendingPts <= target + halfFrame) {
            std::swap(cur, pending); curPts = pendingPts; haveCur = true; havePending = false; damaged = false;
            av_frame_unref(pending.get());
        }
        // a step back always seeks; a jump forward seeks only when decoding there would take longer than the seek
        // (after the end of the stream there is nothing left to skip: the last frame stays on screen). curPts is where
        // decoding stands, also after a damaged frame dropped the picture.
        const bool decodedSome = curPts != AV_NOPTS_VALUE;
        const int64_t pos = decodedSome ? curPts : startPts;
        const bool back = decodedSome && target < curPts - halfFrame;
        const bool farAhead = !eof && double(target - pos) * av_q2d(tb) > seekAhead && keyframeAhead(pos, target);
        if ((back || farAhead) && !seekTo(target) && back) {
            error = QStringLiteral("cannot seek back to %1 s").arg(t, 0, 'f', 3);
            return nullptr;
        }
        while (!havePending && !eof) {
            const int r = decodeNext(tmp.get());
            if (r == 0) break;
            if (r < 0) {
                if (r == AVERROR_EXIT) { error = dl.reason(); return nullptr; }
                return damage(r, t);
            }
            const int64_t pts = framePts(tmp.get());
            if (pts <= target + halfFrame) {
                av_frame_unref(cur.get()); av_frame_move_ref(cur.get(), tmp.get()); curPts = pts; haveCur = true; damaged = false;
            } else {
                av_frame_unref(pending.get()); av_frame_move_ref(pending.get(), tmp.get()); pendingPts = pts; havePending = true;
            }
        }
        if (!haveCur && havePending) {
            // after a decode error the target lies in the lost stretch: no picture to show for it
            if (damaged) { error = QStringLiteral("no decodable frame at %1 s (after a decode error)").arg(t, 0, 'f', 3); return nullptr; }
            // the target lies before the first frame we got (a seek past it, or t < first pts)
            std::swap(cur, pending); curPts = pendingPts; haveCur = true; havePending = false;
        }
        if (!haveCur) { error = QStringLiteral("no frame at %1 s").arg(t, 0, 'f', 3); return nullptr; }
        lastT = double(curPts - startPts) * av_q2d(tb);
        return cur.get();
    }

    // Scale `f` to dw x dh packed RGB24 rows at `out` (stride `outStride`); alpha formats are flattened onto white
    // (as the TS grabFrame does before a JPEG encode).
    bool render(const AVFrame* f, int dw, int dh, int flags, uchar* out, qsizetype outStride) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
        const bool alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
        const AVPixelFormat dstFmt = alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
        const int range = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        const int space = f->colorspace;
        if (!sws || sSrcW != f->width || sSrcH != f->height || sSrcFmt != f->format || sDstW != dw || sDstH != dh || sFlags != flags
            || sRange != range || sSpace != space || bufFmt != dstFmt) {
            sws.reset(sws_getContext(f->width, f->height, AVPixelFormat(f->format), dw, dh, dstFmt, flags, nullptr, nullptr, nullptr));
            if (!sws) { error = QStringLiteral("no scaler for this pixel format"); return false; }
            // BT.709 when tagged (or HD and untagged), else BT.601; RGB output is full range. RGB sources refuse this: fine.
            const int cs = (space == AVCOL_SPC_BT709 || (space == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) ? SWS_CS_ITU709 : SWS_CS_ITU601;
            sws_setColorspaceDetails(sws.get(), sws_getCoefficients(cs), range, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
            sSrcW = f->width; sSrcH = f->height; sSrcFmt = f->format; sDstW = dw; sDstH = dh; sFlags = flags; sRange = range; sSpace = space;
        }
        if (!buf[0] || bufW != dw || bufH != dh || bufFmt != dstFmt) {
            if (buf[0]) av_freep(&buf[0]);
            if (av_image_alloc(buf, lines, dw, dh, dstFmt, 64) < 0) { error = QStringLiteral("out of memory"); bufW = bufH = 0; return false; }
            bufW = dw; bufH = dh; bufFmt = dstFmt;
        }
        if (sws_scale(sws.get(), f->data, f->linesize, 0, f->height, buf, lines) <= 0) { error = QStringLiteral("scale failed"); return false; }
        for (int y = 0; y < dh; ++y) {
            const uint8_t* s = buf[0] + qsizetype(y) * lines[0];
            uchar* d = out + qsizetype(y) * outStride;
            if (!alpha) { std::memcpy(d, s, size_t(dw) * 3); continue; }
            for (int x = 0; x < dw; ++x, s += 4, d += 3) {
                const int a = s[3];
                for (int c = 0; c < 3; ++c) d[c] = uchar((s[c] * a + 255 * (255 - a) + 127) / 255);
            }
        }
        return true;
    }

    double sar(const AVFrame* f) const {
        if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0) return av_q2d(f->sample_aspect_ratio);
        if (cc->sample_aspect_ratio.num > 0 && cc->sample_aspect_ratio.den > 0) return av_q2d(cc->sample_aspect_ratio);
        return 1.0;
    }

    // coded width with the stream's sample aspect applied, and the coded height (0 before a decoder)
    int sarWidth() const {
        if (!cc || cc->width <= 0) return 0;
        const AVRational s = cc->sample_aspect_ratio;
        return qMax(1, int(std::lround(cc->width * (s.num > 0 && s.den > 0 ? av_q2d(s) : 1.0))));
    }
    int height() const { return cc ? cc->height : 0; }
};

FrameSampler::FrameSampler(const QString& path) : FrameSampler(path, Options()) {}
FrameSampler::FrameSampler(const QString& path, const Options& o) : d(std::make_unique<Impl>(path, o)) {}
FrameSampler::~FrameSampler() = default;

bool FrameSampler::ok() const { return d->opened; }
QString FrameSampler::error() const { return d->error; }
bool FrameSampler::aborted() const { return d->dl.timedOut() || d->dl.cancelled(); }
double FrameSampler::duration() const { return d->durationSec; }
int FrameSampler::width() const { return d->cc ? d->cc->width : 0; }
int FrameSampler::height() const { return d->cc ? d->cc->height : 0; }
int FrameSampler::displayWidth() const { return d->streamOrient >= 5 ? d->height() : d->sarWidth(); }
int FrameSampler::displayHeight() const { return d->streamOrient >= 5 ? d->sarWidth() : d->height(); }
int FrameSampler::orientation() const { return d->streamOrient; }
double FrameSampler::fps() const { return d->fpsValue; }
double FrameSampler::lastFrameTime() const { return d->lastT; }
qint64 FrameSampler::framesDecoded() const { return d->decoded; }

QByteArray FrameSampler::thumbAt(double t, int w, int h) {
    if (w < 1 || h < 1 || qint64(w) * h > 4096LL * 4096LL) { d->error = QStringLiteral("bad thumbnail size"); return {}; }
    const AVFrame* f = d->frameAt(t);
    if (!f) return {};
    QByteArray out(qsizetype(w) * h * 3, Qt::Uninitialized);
    const int o = d->orientOf(f);
    if (o == 1) {
        if (!d->render(f, w, h, SWS_AREA, reinterpret_cast<uchar*>(out.data()), qsizetype(w) * 3)) return {};
        return out;
    }
    // stretch the coded picture to the size it has before the turn, then turn it into w x h
    QImage coded(o >= 5 ? h : w, o >= 5 ? w : h, QImage::Format_RGB888);
    if (coded.isNull()) { d->error = QStringLiteral("out of memory"); return {}; }
    if (!d->render(f, coded.width(), coded.height(), SWS_AREA, coded.bits(), coded.bytesPerLine())) return {};
    const QImage up = orientImage(coded, o);
    if (up.isNull()) { d->error = QStringLiteral("out of memory"); return {}; }
    for (int y = 0; y < h; ++y) std::memcpy(out.data() + qsizetype(y) * w * 3, up.constScanLine(y), size_t(w) * 3);
    return out;
}

QImage FrameSampler::imageAt(double t, int maxW, int maxH) {
    const AVFrame* f = d->frameAt(t);
    if (!f) return {};
    const int o = d->orientOf(f);
    const double dispW = f->width * d->sar(f), dispH = f->height;   // before the turn
    const double outW = o >= 5 ? dispH : dispW, outH = o >= 5 ? dispW : dispH;
    double k = 1.0;
    if (maxW > 0) k = qMin(k, maxW / outW);
    if (maxH > 0) k = qMin(k, maxH / outH);
    const int dw = qMax(1, int(std::lround(dispW * k))), dh = qMax(1, int(std::lround(dispH * k)));
    QImage img(dw, dh, QImage::Format_RGB888);
    if (img.isNull()) { d->error = QStringLiteral("out of memory"); return {}; }
    if (!d->render(f, dw, dh, SWS_BICUBIC, img.bits(), img.bytesPerLine())) return {};
    if (o == 1) return img;
    QImage up = orientImage(img, o);
    if (up.isNull()) d->error = QStringLiteral("out of memory");
    return up;
}

ThumbScan sampleThumbs(const QString& path, const std::vector<double>& times, int w, int h, const FrameSampler::Options& o) {
    ThumbScan s;
    FrameSampler fs(path, o);
    if (!fs.ok()) { s.error = fs.error(); s.failed = int(times.size()); return s; }
    s.duration = fs.duration(); s.width = fs.width(); s.height = fs.height();
    for (size_t i = 0; i < times.size(); ++i) {
        QByteArray rgb = fs.thumbAt(times[i], w, h);
        if (rgb.isEmpty()) {
            ++s.failed;
            if (s.error.isEmpty()) s.error = fs.error();
            if (fs.aborted()) { s.failed += int(times.size() - i - 1); s.error = fs.error(); break; }
            continue;
        }
        s.times.push_back(times[i]);
        s.rgb.push_back(std::move(rgb));
    }
    s.ok = !s.rgb.empty() && !fs.aborted();
    return s;
}

} // namespace studio::media
