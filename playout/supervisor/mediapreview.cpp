#include "mediapreview.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <QMutexLocker>
#include <QDebug>
#include <cmath>
#include <functional>
#include <QMap>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {
QString avErr(int r) { char b[AV_ERROR_MAX_STRING_SIZE] = {0}; av_strerror(r, b, sizeof(b)); return QString::fromUtf8(b); }
int even(double v) { int x = int(std::lround(v)); return qMax(2, x - (x & 1)); }
}

// ---------------------------------------------------------------------------
// One open file: demuxer + video decoder + scaler + JPEG encoder, and the JPEGs of recent frames.
// ---------------------------------------------------------------------------
struct MediaPreview::Decoder {
    QMutex lock;
    QString path;
    qint64 fileSize = 0, fileMtime = 0;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* cc = nullptr;
    int vs = -1;
    AVRational tb{1, 1}, fps{25, 1};
    int64_t startPts = 0;
    int64_t frameDur = 1;            // pts units per frame
    double durationSec = 0.0;
    qint64 frames = 0;
    AVPacket* pkt = nullptr;
    AVFrame* tmp = nullptr;
    AVFrame* last = nullptr;         // the most recently decoded frame
    int64_t lastPts = AV_NOPTS_VALUE;
    bool hasLast = false, eof = false;
    SwsContext* sws = nullptr; int swsSrcW = 0, swsSrcH = 0, swsSrcFmt = -1, swsDstW = 0, swsDstH = 0, swsSrcRange = -1, swsSrcSpace = -1;
    AVCodecContext* enc = nullptr; int encW = 0, encH = 0; int64_t encPts = 0;
    QList<QPair<QString, MediaPreview::Frame>> jpegs;   // LRU of produced JPEGs, key "frame@width"
    // Streams without a seek index (MPEG-TS/PS): keyframe pts -> byte position, learnt while reading, so a step back
    // seeks straight to the keyframe before the target instead of searching by timestamp.
    bool byteSeek = false;
    QMap<int64_t, int64_t> keyPos;

    ~Decoder() {
        if (sws) sws_freeContext(sws);
        if (enc) avcodec_free_context(&enc);
        if (cc) avcodec_free_context(&cc);
        if (fmt) avformat_close_input(&fmt);
        if (pkt) av_packet_free(&pkt);
        if (tmp) av_frame_free(&tmp);
        if (last) av_frame_free(&last);
    }

    bool open(const QString& p, bool keyframesOnly, QString* error) {
        path = p;
        const QFileInfo fi(p); fileSize = fi.size(); fileMtime = fi.lastModified().toMSecsSinceEpoch();
        const QByteArray u = QDir::toNativeSeparators(p).toUtf8();
        int r = avformat_open_input(&fmt, u.constData(), nullptr, nullptr);
        if (r < 0) { if (error) *error = "cannot open " + p + ": " + avErr(r); return false; }
        if ((r = avformat_find_stream_info(fmt, nullptr)) < 0) { if (error) *error = "no stream info: " + avErr(r); return false; }
        const AVCodec* dec = nullptr;
        vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (vs < 0 || !dec) { if (error) *error = "no video stream"; return false; }
        AVStream* st = fmt->streams[vs];
        for (unsigned i = 0; i < fmt->nb_streams; ++i) if (int(i) != vs) fmt->streams[i]->discard = AVDISCARD_ALL;   // demux video only
        cc = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cc, st->codecpar);
        cc->thread_count = keyframesOnly ? 2 : 4;
        if (keyframesOnly) cc->skip_frame = AVDISCARD_NONKEY;
        if ((r = avcodec_open2(cc, dec, nullptr)) < 0) { if (error) *error = "decoder: " + avErr(r); return false; }
        tb = st->time_base;
        const QString fmtName = QString::fromLatin1(fmt->iformat ? fmt->iformat->name : "");
        byteSeek = (fmtName.contains("mpegts") || fmtName.contains("mpeg")) && !(fmt->iformat->flags & AVFMT_NO_BYTE_SEEK);
        fps = av_guess_frame_rate(fmt, st, nullptr);
        if (fps.num <= 0 || fps.den <= 0) fps = AVRational{25, 1};
        startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        frameDur = qMax<int64_t>(1, av_rescale_q(1, av_inv_q(fps), tb));
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) durationSec = st->duration * av_q2d(tb);
        else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) durationSec = double(fmt->duration) / AV_TIME_BASE;
        frames = st->nb_frames > 0 ? st->nb_frames : qint64(std::llround(durationSec * av_q2d(fps)));
        pkt = av_packet_alloc(); tmp = av_frame_alloc(); last = av_frame_alloc();
        return true;
    }

    int64_t ptsOfFrame(qint64 n) const { return startPts + av_rescale_q_rnd(n, av_inv_q(fps), tb, AV_ROUND_NEAR_INF); }
    qint64 frameOfPts(int64_t pts) const { return qint64(std::llround(double(pts - startPts) * av_q2d(tb) * av_q2d(fps))); }

    void resetDecoder() { avcodec_flush_buffers(cc); av_frame_unref(last); hasLast = false; eof = false; lastPts = AV_NOPTS_VALUE; }

    void noteKeyframe(const AVPacket* p) {
        if (!byteSeek || p->stream_index != vs || !(p->flags & AV_PKT_FLAG_KEY) || p->pos < 0) return;
        const int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        if (ts != AV_NOPTS_VALUE) keyPos.insert(ts, p->pos);
    }

    bool seekTo(int64_t targetPts) {
        if (byteSeek && !keyPos.isEmpty()) {
            auto it = keyPos.upperBound(targetPts);
            if (it != keyPos.begin()) {
                --it;
                const int64_t gap = av_rescale_q(20, AVRational{1, 1}, tb);
                if (targetPts - it.key() <= gap && av_seek_frame(fmt, vs, it.value(), AVSEEK_FLAG_BYTE) >= 0) { resetDecoder(); return true; }
            }
        }
        int r = av_seek_frame(fmt, vs, targetPts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = av_seek_frame(fmt, vs, startPts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = avformat_seek_file(fmt, vs, INT64_MIN, targetPts, targetPts, 0);
        resetDecoder();
        return r >= 0;
    }

    /// Leaves `last` = the first frame at or after (target - half a frame): the frame on screen at the target.
    /// Steps forward from the current position when that is close; otherwise seeks to the keyframe before.
    bool decodeTo(int64_t targetPts, QString* error, const std::function<void(const AVFrame*, int64_t)>& onBefore = {}) {
        const int64_t half = frameDur / 2;
        if (hasLast && lastPts != AV_NOPTS_VALUE && std::llabs(lastPts - targetPts) <= half) return true;
        const int64_t window = av_rescale_q(3, AVRational{1, 1}, tb);   // decode up to 3 s forward instead of seeking
        const bool forward = hasLast && !eof && lastPts != AV_NOPTS_VALUE && lastPts < targetPts && targetPts - lastPts <= window;
        // A seek in a stream without a keyframe index (MPEG-TS) can land after the target, and decoding then starts at the
        // next keyframe. When the first frame out of a seek is already past the target, seek again further back.
        int64_t backoff = av_rescale_q(1, AVRational{1, 1}, tb);
        int64_t seekedTo = targetPts;
        bool justSeeked = false;
        int reseeks = 0;
        if (!forward) { seekTo(targetPts); justSeeked = true; }
        bool drained = false;
        for (int guard = 0; guard < 200000; ++guard) {
            int r = avcodec_receive_frame(cc, tmp);
            if (r == 0) {
                int64_t pts = tmp->pts != AV_NOPTS_VALUE ? tmp->pts : tmp->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) pts = hasLast ? lastPts + frameDur : targetPts;
                if (justSeeked) {
                    justSeeked = false;
                    if (pts > targetPts + half && reseeks < 7 && seekedTo > startPts) {
                        seekedTo = qMax<int64_t>(startPts, targetPts - backoff);
                        backoff *= 2; ++reseeks;
                        av_frame_unref(tmp);
                        seekTo(seekedTo); justSeeked = true; drained = false;
                        continue;
                    }
                }
                av_frame_unref(last); av_frame_move_ref(last, tmp); lastPts = pts; hasLast = true;
                if (pts >= targetPts - half) return true;
                if (onBefore) onBefore(last, pts);
                continue;
            }
            if (r == AVERROR_EOF) { eof = true; return hasLast; }                  // past the end: the last frame
            if (r != AVERROR(EAGAIN)) { if (error) *error = "decode: " + avErr(r); return hasLast; }
            if (drained) { eof = true; return hasLast; }
            r = av_read_frame(fmt, pkt);
            if (r < 0) { avcodec_send_packet(cc, nullptr); drained = true; continue; }
            noteKeyframe(pkt);
            if (pkt->stream_index == vs) {
                const int s = avcodec_send_packet(cc, pkt);
                if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_INVALIDDATA) { av_packet_unref(pkt); if (error) *error = "decode: " + avErr(s); return hasLast; }
            }
            av_packet_unref(pkt);
        }
        if (error) *error = "decode did not reach the frame";
        return hasLast;
    }

    /// The next frame from the current position (no seek): after seekTo() on a keyframes-only decoder,
    /// the keyframe at or before the seek target.
    bool decodeFirst(QString* error) {
        bool drained = false;
        for (int guard = 0; guard < 100000; ++guard) {
            int r = avcodec_receive_frame(cc, tmp);
            if (r == 0) {
                int64_t pts = tmp->pts != AV_NOPTS_VALUE ? tmp->pts : tmp->best_effort_timestamp;
                av_frame_unref(last); av_frame_move_ref(last, tmp); lastPts = pts; hasLast = true;
                return true;
            }
            if (r == AVERROR_EOF) { eof = true; return hasLast; }
            if (r != AVERROR(EAGAIN)) { if (error) *error = "decode: " + avErr(r); return false; }
            if (drained) return hasLast;
            r = av_read_frame(fmt, pkt);
            if (r < 0) { avcodec_send_packet(cc, nullptr); drained = true; continue; }
            noteKeyframe(pkt);
            if (pkt->stream_index == vs) avcodec_send_packet(cc, pkt);
            av_packet_unref(pkt);
        }
        return false;
    }

    /// Display size for an output width (pixel aspect applied, even dimensions).
    void outSize(const AVFrame* f, int wantW, int* ow, int* oh) const {
        AVRational sar = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio : (cc->sample_aspect_ratio.num > 0 ? cc->sample_aspect_ratio : AVRational{1, 1});
        const double dispW = f->width * av_q2d(sar);
        const int w = even(qBound(64.0, double(wantW), qMin(1920.0, qMax(64.0, dispW))));
        *ow = w; *oh = even(w * double(f->height) / dispW);
    }

    /// Scale `src` into `dst` planes at (dx, dy) of a YUV420P full-range picture (JPEG range, BT.601 like JFIF).
    bool scaleInto(const AVFrame* src, uint8_t* const dstData[4], const int dstLines[4], int dx, int dy, int dw, int dh) {
        const int srcRange = src->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        const int srcSpace = src->colorspace;
        if (!sws || swsSrcW != src->width || swsSrcH != src->height || swsSrcFmt != src->format || swsDstW != dw || swsDstH != dh || swsSrcRange != srcRange || swsSrcSpace != srcSpace) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(src->width, src->height, AVPixelFormat(src->format), dw, dh, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!sws) return false;
            const int csIn = (srcSpace == AVCOL_SPC_BT709 || (srcSpace == AVCOL_SPC_UNSPECIFIED && src->height >= 720)) ? SWS_CS_ITU709 : SWS_CS_ITU601;
            sws_setColorspaceDetails(sws, sws_getCoefficients(csIn), srcRange, sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
            swsSrcW = src->width; swsSrcH = src->height; swsSrcFmt = src->format; swsDstW = dw; swsDstH = dh; swsSrcRange = srcRange; swsSrcSpace = srcSpace;
        }
        uint8_t* d[4] = { dstData[0] + size_t(dy) * dstLines[0] + dx, dstData[1] + size_t(dy / 2) * dstLines[1] + dx / 2, dstData[2] + size_t(dy / 2) * dstLines[2] + dx / 2, nullptr };
        const int l[4] = { dstLines[0], dstLines[1], dstLines[2], 0 };
        return sws_scale(sws, src->data, src->linesize, 0, src->height, d, l) > 0;
    }

    QByteArray encodeJpeg(const AVFrame* yuv, int quality, QString* error, bool retried = false) {
        if (!enc || encW != yuv->width || encH != yuv->height) {
            if (enc) avcodec_free_context(&enc);
            const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
            if (!c) { if (error) *error = "no MJPEG encoder"; return {}; }
            enc = avcodec_alloc_context3(c);
            enc->width = yuv->width; enc->height = yuv->height;
            enc->pix_fmt = AV_PIX_FMT_YUV420P; enc->color_range = AVCOL_RANGE_JPEG;
            enc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
            enc->time_base = AVRational{1, 25};
            enc->flags |= AV_CODEC_FLAG_QSCALE;
            enc->global_quality = FF_QP2LAMBDA * quality;
            const int r = avcodec_open2(enc, c, nullptr);
            if (r < 0) { if (error) *error = "MJPEG encoder: " + avErr(r); avcodec_free_context(&enc); return {}; }
            encW = yuv->width; encH = yuv->height; encPts = 0;
        }
        AVFrame* f = av_frame_clone(yuv);
        // Encoders refuse a timestamp that does not increase; MJPEG is intra-only, so the bytes of a frame do not depend on it.
        f->quality = FF_QP2LAMBDA * quality; f->pts = encPts++; f->color_range = AVCOL_RANGE_JPEG;
        QByteArray out;
        int r = avcodec_send_frame(enc, f);
        av_frame_free(&f);
        if (r >= 0) {
            AVPacket* p = av_packet_alloc();
            r = avcodec_receive_packet(enc, p);
            if (r == 0) out = QByteArray(reinterpret_cast<const char*>(p->data), p->size);
            av_packet_free(&p);
        }
        if (out.isEmpty() && !retried) { avcodec_free_context(&enc); return encodeJpeg(yuv, quality, error, true); }   // rebuild the encoder once
        if (out.isEmpty() && error) *error = "JPEG encode failed: " + avErr(r);
        return out;
    }
};

// ---------------------------------------------------------------------------

MediaPreview::MediaPreview(const QString& cacheDir, QObject* parent) : QObject(parent), m_cacheDir(cacheDir) {
    QDir().mkpath(m_cacheDir);
    m_pool.setMaxThreadCount(2);
    m_sbPool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(60000);
    m_sbPool.setExpiryTimeout(60000);
}

MediaPreview::~MediaPreview() {
    m_pool.clear(); m_sbPool.clear();
    m_pool.waitForDone(3000); m_sbPool.waitForDone(3000);
    QMutexLocker l(&m_cacheMutex);
    m_decoders.clear();
}

QSharedPointer<MediaPreview::Decoder> MediaPreview::decoderFor(const QString& path, QString* error) {
    const QFileInfo fi(path);
    if (!fi.isFile()) { if (error) *error = "file not found: " + path; return {}; }
    QMutexLocker l(&m_cacheMutex);
    for (int i = 0; i < m_decoders.size(); ++i) {
        const auto d = m_decoders[i];
        if (d->path == path) {
            if (d->fileSize == fi.size() && d->fileMtime == fi.lastModified().toMSecsSinceEpoch()) { m_decoders.move(i, 0); return d; }
            m_decoders.removeAt(i); break;                       // the file changed: reopen
        }
    }
    QSharedPointer<Decoder> d(new Decoder);
    if (!d->open(path, false, error)) return {};
    m_decoders.prepend(d);
    while (m_decoders.size() > kMaxDecoders) m_decoders.removeLast();   // a decoder in use elsewhere lives on until released
    return d;
}

QJsonObject MediaPreview::Info::toJson() const {
    QJsonObject o;
    o["ok"] = ok; if (!error.isEmpty()) o["error"] = error;
    QJsonObject f; f["num"] = fpsNum; f["den"] = fpsDen; o["fps"] = f;
    o["fpsValue"] = fpsDen > 0 ? double(fpsNum) / fpsDen : 0.0;
    o["durationSec"] = durationSec; o["frames"] = double(frames);
    o["width"] = width; o["height"] = height;
    QJsonObject s; s["num"] = sarNum; s["den"] = sarDen; o["sar"] = s;
    o["startSec"] = startSec;
    QJsonObject tc; tc["dropFrame"] = dropFrame; tc["base"] = tcBase; o["timecode"] = tc;
    return o;
}

MediaPreview::Info MediaPreview::info(const QString& path) {
    Info in;
    QString err;
    const auto d = decoderFor(path, &err);
    if (!d) { in.error = err; return in; }
    QMutexLocker l(&d->lock);
    in.ok = true;
    in.fpsNum = d->fps.num; in.fpsDen = d->fps.den;
    in.durationSec = d->durationSec; in.frames = d->frames;
    in.width = d->cc->width; in.height = d->cc->height;
    const AVRational sar = d->cc->sample_aspect_ratio.num > 0 ? d->cc->sample_aspect_ratio : AVRational{1, 1};
    in.sarNum = sar.num; in.sarDen = sar.den;
    in.startSec = d->startPts * av_q2d(d->tb);
    in.dropFrame = d->fps.den == 1001 && (d->fps.num == 30000 || d->fps.num == 60000);
    in.tcBase = int(std::lround(av_q2d(d->fps)));
    return in;
}

MediaPreview::Frame MediaPreview::frameJpeg(const QString& path, double tSec, qint64 frameNo, int width) {
    Frame out;
    QString err;
    const auto d = decoderFor(path, &err);
    if (!d) { out.error = err; return out; }
    QMutexLocker l(&d->lock);
    qint64 n = frameNo >= 0 ? frameNo : qint64(std::floor(qMax(0.0, tSec) * av_q2d(d->fps) + 1e-6));
    if (d->frames > 0) n = qMin(n, d->frames - 1);
    n = qMax<qint64>(0, n);
    const int w = qBound(64, width > 0 ? width : 640, 1920);
    const QString key = QString::number(n) + "@" + QString::number(w);
    for (int i = 0; i < d->jpegs.size(); ++i) if (d->jpegs[i].first == key) { d->jpegs.move(i, 0); return d->jpegs.first().second; }

    auto encodeOne = [&](const AVFrame* src, int64_t pts, Frame* fo) -> bool {
        int ow = 0, oh = 0; d->outSize(src, w, &ow, &oh);
        AVFrame* yuv = av_frame_alloc();
        yuv->format = AV_PIX_FMT_YUV420P; yuv->width = ow; yuv->height = oh; yuv->color_range = AVCOL_RANGE_JPEG;
        const bool ok = av_frame_get_buffer(yuv, 32) >= 0 && d->scaleInto(src, yuv->data, yuv->linesize, 0, 0, ow, oh);
        if (ok) fo->jpeg = d->encodeJpeg(yuv, 3, &err);
        av_frame_free(&yuv);
        if (!ok || fo->jpeg.isEmpty()) return false;
        fo->frame = qMax<qint64>(0, d->frameOfPts(pts));
        fo->timeSec = double(fo->frame) * av_q2d(av_inv_q(d->fps));
        fo->width = ow; fo->height = oh;
        return true;
    };
    auto remember = [&](const Frame& fo, const QString& k) {
        for (int i = 0; i < d->jpegs.size(); ++i) if (d->jpegs[i].first == k) { d->jpegs.removeAt(i); break; }
        d->jpegs.prepend(qMakePair(k, fo));
    };
    // Frames decoded on the way to the target (after a seek: from the keyframe on) that are within 24 frames of it are
    // kept as JPEGs too, so the next steps back are answered from memory instead of another seek and decode.
    const int64_t targetPts = d->ptsOfFrame(n);
    const int64_t keepFrom = targetPts - 24 * d->frameDur;
    auto onBefore = [&](const AVFrame* f, int64_t pts) {
        if (pts < keepFrom) return;
        Frame fo;
        if (encodeOne(f, pts, &fo)) remember(fo, QString::number(fo.frame) + "@" + QString::number(w));
    };
    if (!d->decodeTo(targetPts, &err, onBefore) || !d->hasLast) { out.error = err.isEmpty() ? "no frame" : err; return out; }
    if (!encodeOne(d->last, d->lastPts, &out)) { out.error = err.isEmpty() ? "encode failed" : err; return out; }
    // Remember it under the asked frame number and under the frame it really is (the end of the file clamps).
    remember(out, key);
    const QString realKey = QString::number(out.frame) + "@" + QString::number(w);
    if (realKey != key) remember(out, realKey);
    while (d->jpegs.size() > 160) d->jpegs.removeLast();
    return out;
}

QJsonObject MediaPreview::storyboard(const QString& path, const QString& key, int n, int tileW, QString* error) {
    const QString jpgPath = QDir(m_cacheDir).filePath(key + ".jpg"), jsonPath = QDir(m_cacheDir).filePath(key + ".json");
    if (QFileInfo::exists(jpgPath) && QFileInfo::exists(jsonPath)) {
        QFile f(jsonPath);
        if (f.open(QIODevice::ReadOnly)) { const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object(); if (!o.isEmpty()) return o; }
    }
    n = qBound(10, n, 400); tileW = even(qBound(64, tileW, 320));
    Decoder d;                                             // a decoder of its own: keyframes only, never shared
    QString err;
    if (!d.open(path, true, &err)) { if (error) *error = err; return {}; }
    if (d.durationSec <= 0.0) { if (error) *error = "unknown duration"; return {}; }
    const AVRational sar = d.cc->sample_aspect_ratio.num > 0 ? d.cc->sample_aspect_ratio : AVRational{1, 1};
    const double dispW = d.cc->width * av_q2d(sar);
    const int tileH = even(tileW * double(d.cc->height) / qMax(1.0, dispW));
    const int cols = qMin(10, n), rows = (n + cols - 1) / cols;
    AVFrame* sprite = av_frame_alloc();
    sprite->format = AV_PIX_FMT_YUV420P; sprite->width = cols * tileW; sprite->height = rows * tileH; sprite->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(sprite, 32) < 0) { av_frame_free(&sprite); if (error) *error = "out of memory"; return {}; }
    for (int y = 0; y < sprite->height; ++y) memset(sprite->data[0] + size_t(y) * sprite->linesize[0], 0, sprite->width);
    for (int y = 0; y < sprite->height / 2; ++y) { memset(sprite->data[1] + size_t(y) * sprite->linesize[1], 128, sprite->width / 2); memset(sprite->data[2] + size_t(y) * sprite->linesize[2], 128, sprite->width / 2); }
    QJsonArray times;
    int placed = 0;
    for (int i = 0; i < n; ++i) {
        const double t = (i + 0.5) * d.durationSec / n;
        times.append(std::round(t * 1000.0) / 1000.0);
        const int64_t target = d.startPts + int64_t(std::llround(t / av_q2d(d.tb)));
        d.seekTo(target);
        // keyframes only: the first frame out after the seek is the keyframe at or before t
        QString e2;
        if (!d.decodeFirst(&e2) || !d.hasLast) continue;
        if (d.scaleInto(d.last, sprite->data, sprite->linesize, (i % cols) * tileW, (i / cols) * tileH, tileW, tileH)) ++placed;
    }
    if (!placed) { av_frame_free(&sprite); if (error) *error = "no frames decoded"; return {}; }
    const QByteArray jpg = d.encodeJpeg(sprite, 5, &err);
    av_frame_free(&sprite);
    if (jpg.isEmpty()) { if (error) *error = err; return {}; }
    QSaveFile sf(jpgPath);
    if (!sf.open(QIODevice::WriteOnly) || sf.write(jpg) != jpg.size() || !sf.commit()) { if (error) *error = "cannot write " + jpgPath; return {}; }
    QJsonObject o;
    QJsonObject f; f["num"] = d.fps.num; f["den"] = d.fps.den; o["fps"] = f;
    o["durationSec"] = d.durationSec; o["frames"] = double(d.frames);
    o["n"] = n; o["cols"] = cols; o["rows"] = rows; o["tileW"] = tileW; o["tileH"] = tileH;
    o["spriteW"] = cols * tileW; o["spriteH"] = rows * tileH; o["bytes"] = double(jpg.size());
    o["interval"] = d.durationSec / n; o["times"] = times; o["placed"] = placed;
    o["url"] = "/storyboards/" + key + ".jpg";
    QSaveFile jf(jsonPath);
    if (jf.open(QIODevice::WriteOnly)) { jf.write(QJsonDocument(o).toJson(QJsonDocument::Compact)); jf.commit(); }
    return o;
}

QString MediaPreview::timecode(qint64 frame, int fpsNum, int fpsDen) {
    if (fpsNum <= 0 || fpsDen <= 0) return "00:00:00:00";
    const int base = int(std::lround(double(fpsNum) / fpsDen));
    const bool drop = fpsDen == 1001 && (fpsNum == 30000 || fpsNum == 60000);
    qint64 f = qMax<qint64>(0, frame);
    if (drop) {
        // SMPTE drop-frame: skip frame numbers 0,1 (4 at 59.94) at the start of every minute except each tenth minute.
        const int dropN = base == 60 ? 4 : 2;
        const qint64 per10 = qint64(base) * 600 - 9 * dropN;     // frames in 10 minutes
        const qint64 perMin = qint64(base) * 60 - dropN;          // frames in a dropped minute
        const qint64 d = f / per10, m = f % per10;
        f += 9 * dropN * d + (m > dropN ? dropN * ((m - dropN) / perMin) : 0);
    }
    const int ff = int(f % base); const qint64 s = f / base;
    return QString("%1:%2:%3%4%5").arg(s / 3600, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
        .arg(drop ? ';' : ':').arg(ff, 2, 10, QChar('0'));
}
