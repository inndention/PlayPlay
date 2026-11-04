// Picture input / output for the card renderer, see imageio.h.
#include "imageio.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace imageio {
namespace {

QString avErr(int r) { char b[AV_ERROR_MAX_STRING_SIZE] = {0}; av_strerror(r, b, sizeof(b)); return QString::fromUtf8(b); }

// Deadline + cancel flag, polled by libav's I/O (AVIOInterruptCB) and by the decode loop between packets.
struct Irq {
    std::chrono::steady_clock::time_point deadline;
    bool hasDeadline = false;
    const std::atomic<bool>* cancel = nullptr;
    bool timedOut = false, cancelled = false;
    bool stop() {
        if (timedOut || cancelled) return true;
        if (cancel && cancel->load(std::memory_order_relaxed)) cancelled = true;
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

AVPixelFormat plainTwin(AVPixelFormat f) {   // a YUVJ format = the plain one at full range (same planes)
    switch (f) {
    case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P: return AV_PIX_FMT_YUV411P;
    default: return f;
    }
}

// "http://", "data:", "file:" ...: two or more scheme letters before the colon (a drive letter is one). Pictures come
// from local files only; a URL in a scene spec never makes this tool fetch anything.
bool isUrl(const QString& p) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9+.\\-]+:"));
    return re.match(p).hasMatch();
}

// ---- EXIF Orientation, for a picture whose decoder gives no display matrix
quint32 rd(const QByteArray& b, qsizetype at, int bytes, bool le) {
    if (at < 0 || at + bytes > b.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(b.constData() + at);
    if (bytes == 2) return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    return le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
}

// A TIFF block ("II*\0" / "MM\0*", optionally behind "Exif\0\0"): IFD0's Orientation as SHORT (or LONG) 1..8.
int tiffOrientation(QByteArray t) {
    if (t.startsWith(QByteArray("Exif\0\0", 6))) t.remove(0, 6);
    if (t.size() < 8) return 0;
    const bool le = t.startsWith("II");
    if (!le && !t.startsWith("MM")) return 0;
    if (rd(t, 2, 2, le) != 42) return 0;
    const qsizetype ifd = qsizetype(rd(t, 4, 4, le));
    const int n = int(rd(t, ifd, 2, le));
    for (int i = 0; i < n && i < 512; ++i) {
        const qsizetype e = ifd + 2 + qsizetype(i) * 12;
        if (e + 12 > t.size()) break;
        if (rd(t, e, 2, le) != 0x0112) continue;
        const quint32 type = rd(t, e + 2, 2, le);
        const quint32 v = type == 3 ? rd(t, e + 8, 2, le) : type == 4 ? rd(t, e + 8, 4, le) : 0;
        return v >= 1 && v <= 8 ? int(v) : 0;
    }
    return 0;
}

// The container walk, over a reader so a whole file never has to be read: JPEG markers up to the scan, WebP RIFF chunks
// (EXIF follows the bitstream), PNG chunks up to IEND.
using ReadAt = std::function<QByteArray(qint64 pos, qint64 len)>;
int containerOrientation(const ReadAt& read, qint64 size) {
    const QByteArray head = read(0, 16);
    constexpr qint64 kMaxBlock = 4 << 20;
    if (head.startsWith("\xff\xd8")) {
        qint64 pos = 2;
        for (int i = 0; i < 1024 && pos + 4 <= size; ++i) {
            const QByteArray m = read(pos, 4);
            if (m.size() < 4 || uchar(m[0]) != 0xff) return 0;
            const uchar marker = uchar(m[1]);
            if (marker == 0xff) { ++pos; continue; }               // fill byte
            if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) { pos += 2; continue; }
            if (marker == 0xda || marker == 0xd9) return 0;         // the scan starts: no EXIF before it
            const qint64 len = qint64(rd(m, 2, 2, false));
            if (len < 2) return 0;
            if (marker == 0xe1) {
                const QByteArray seg = read(pos + 4, len - 2);
                if (seg.startsWith(QByteArray("Exif\0\0", 6))) return tiffOrientation(seg);
            }
            pos += 2 + len;
        }
        return 0;
    }
    if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
        qint64 pos = 12;
        for (int i = 0; i < 1024 && pos + 8 <= size; ++i) {
            const QByteArray c = read(pos, 8);
            if (c.size() < 8) return 0;
            const qint64 len = qint64(rd(c, 4, 4, true));
            if (c.startsWith("EXIF")) return len <= kMaxBlock ? tiffOrientation(read(pos + 8, len)) : 0;
            pos += 8 + len + (len & 1);
        }
        return 0;
    }
    if (head.startsWith("\x89PNG\r\n\x1a\n")) {
        qint64 pos = 8;
        for (int i = 0; i < 100000 && pos + 8 <= size; ++i) {
            const QByteArray c = read(pos, 8);
            if (c.size() < 8) return 0;
            const qint64 len = qint64(rd(c, 0, 4, false));
            if (c.mid(4, 4) == "eXIf") return len <= kMaxBlock ? tiffOrientation(read(pos + 8, len)) : 0;
            if (c.mid(4, 4) == "IEND") return 0;
            pos += 12 + len;
        }
        return 0;
    }
    return tiffOrientation(read(0, std::min<qint64>(size, kMaxBlock)));
}

ReadAt fileReader(QFile& f) {
    return [&f](qint64 pos, qint64 len) -> QByteArray {
        if (pos < 0 || len <= 0 || !f.seek(pos)) return {};
        return f.read(len);
    };
}

int fileOrientation(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return containerOrientation(fileReader(f), f.size());
}

// Pillow's ImageFile.load() raises "image file is truncated" when the data ends before its decoder is done; Qt and
// libav decode what is there instead. The structural signs, per format:
//  - JPEG: libjpeg reads on to the EOI marker. The segments up to the first scan are walked by length (an EXIF
//    thumbnail is a whole JPEG inside APP1, its EOI must not count), then FF D9 is searched for: inside entropy-coded
//    data FF is always followed by 00 or a restart marker. A walk that meets a non-marker byte (libjpeg skips such
//    bytes) searches from there.
//  - PNG: Pillow stops reading at IEND but does not need it (load_end ends quietly on a short read); its zlib stream
//    does need every IDAT byte. So a file cut inside a chunk is truncated, and so is one that ends right after an IDAT
//    chunk (more image data could follow); one that ends after a later ancillary chunk is not.
//  - WebP: libwebp decodes from the whole RIFF payload, so the file must be as long as the RIFF header says.
QString truncationOf(const ReadAt& read, qint64 size) {
    const QByteArray head = read(0, 16);
    if (head.startsWith("\xff\xd8")) {
        qint64 pos = 2, scan = -1;
        for (int i = 0; i < 4096 && pos + 4 <= size; ++i) {
            const QByteArray m = read(pos, 4);
            if (m.size() < 4 || uchar(m[0]) != 0xff) break;
            const uchar marker = uchar(m[1]);
            if (marker == 0xff) { ++pos; continue; }
            if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) { pos += 2; continue; }
            if (marker == 0xd9) return {};                         // EOI before any scan: the decoders fail on their own
            const qint64 len = qint64(rd(m, 2, 2, false));
            if (marker == 0xda) { scan = pos + 2 + len; break; }
            if (len < 2) break;
            pos += 2 + len;
        }
        if (scan < 0) scan = pos;
        constexpr qint64 kBlock = 1 << 20;
        for (qint64 at = scan; at + 1 < size; at += kBlock) {      // blocks overlap by one byte, so FF | D9 is found
            if (read(at, std::min(kBlock + 1, size - at)).contains(QByteArray("\xff\xd9", 2))) return {};
        }
        return QStringLiteral("JPEG ends without its EOI marker (%1 bytes)").arg(size);
    }
    if (head.startsWith("\x89PNG\r\n\x1a\n")) {
        qint64 pos = 8;
        QByteArray last;
        for (int i = 0; i < 1000000; ++i) {
            if (pos == size) {
                if (last == "IDAT" || last.isEmpty()) return QStringLiteral("PNG ends after %1 without IEND (%2 bytes)").arg(QString::fromLatin1(last.isEmpty() ? "the signature" : "image data")).arg(size);
                return {};
            }
            if (pos + 12 > size) return QStringLiteral("PNG ends inside a chunk header (%1 bytes)").arg(size);
            const QByteArray c = read(pos, 8);
            if (c.size() < 8) return QStringLiteral("PNG unreadable at %1").arg(pos);
            const qint64 len = qint64(rd(c, 0, 4, false));
            last = c.mid(4, 4);
            if (last == "IEND") return {};
            if (pos + 12 + len > size)
                return QStringLiteral("PNG ends inside its %1 chunk (%2 bytes)").arg(QString::fromLatin1(last)).arg(size);
            pos += 12 + len;
        }
        return {};
    }
    if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
        const qint64 riff = qint64(rd(head, 4, 4, true));
        if (8 + riff > size) return QStringLiteral("WebP is %1 bytes, its RIFF header says %2").arg(size).arg(8 + riff);
    }
    return {};
}

const int32_t* displayMatrix(const AVFrame* f, const AVStream* st) {
    if (const AVFrameSideData* sd = av_frame_side_data_get(f->side_data, f->nb_side_data, AV_FRAME_DATA_DISPLAYMATRIX); sd && sd->size >= 9 * sizeof(int32_t))
        return reinterpret_cast<const int32_t*>(sd->data);
    if (const AVPacketSideData* pd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        pd && pd->size >= 9 * sizeof(int32_t))
        return reinterpret_cast<const int32_t*>(pd->data);
    return nullptr;
}

// Qt's EXIF mapping (qjpeghandler exif2Qt) backwards, so `orientation` reads the same for both paths.
int exifFromTransform(QImageIOHandler::Transformations t) {
    switch (int(t)) {
    case QImageIOHandler::TransformationMirror: return 2;
    case QImageIOHandler::TransformationRotate180: return 3;
    case QImageIOHandler::TransformationFlip: return 4;
    case QImageIOHandler::TransformationFlipAndRotate90: return 5;
    case QImageIOHandler::TransformationRotate90: return 6;
    case QImageIOHandler::TransformationMirrorAndRotate90: return 7;
    case QImageIOHandler::TransformationRotate270: return 8;
    default: return 1;
    }
}

bool allOpaque(const QImage& argb32) {
    for (int y = 0; y < argb32.height(); ++y) {
        const auto* p = reinterpret_cast<const quint32*>(argb32.constScanLine(y));
        for (int x = 0; x < argb32.width(); ++x) if ((p[x] >> 24) != 0xff) return false;
    }
    return true;
}

// RGB32 when every pixel is opaque, else ARGB32_Premultiplied (what QPainter composites fastest).
QImage normalized(const QImage& in) {
    if (in.isNull()) return in;
    if (!in.hasAlphaChannel()) return in.format() == QImage::Format_RGB32 ? in : in.convertToFormat(QImage::Format_RGB32);
    QImage a = in.convertToFormat(QImage::Format_ARGB32);
    if (allOpaque(a)) { a.reinterpretAsFormat(QImage::Format_RGB32); return a; }
    return a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage to32(const QImage& in) {
    if (in.format() == QImage::Format_RGB32 || in.format() == QImage::Format_ARGB32_Premultiplied) return in;
    return in.convertToFormat(in.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
}

// The stateful swscale API, for a pair the dynamic sws_scale_frame() path refuses (as the MediaTool thumbnailer does).
int scaleLegacy(const AVFrame* src, AVFrame* dst) {
    SwsContext* c = sws_getContext(src->width, src->height, AVPixelFormat(src->format), dst->width, dst->height, AVPixelFormat(dst->format),
                                   SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
    if (!c) return AVERROR(EINVAL);
    const int cs = src->colorspace == AVCOL_SPC_UNSPECIFIED || src->colorspace == AVCOL_SPC_RGB ? SWS_CS_DEFAULT : int(src->colorspace);
    const int* coef = sws_getCoefficients(cs);
    sws_setColorspaceDetails(c, coef, src->color_range == AVCOL_RANGE_JPEG ? 1 : 0, coef, 1, 0, 1 << 16, 1 << 16);
    int r = av_frame_get_buffer(dst, 0);
    if (r >= 0) r = sws_scale(c, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    sws_freeContext(c);
    return r < 0 ? r : 0;
}

Loaded loadLibav(const QString& path, const LoadOptions& o, Loaded r) {
    Irq irq;
    irq.cancel = o.cancel;
    irq.hasDeadline = o.timeoutMs > 0;
    if (irq.hasDeadline) irq.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.timeoutMs);
    auto fail = [&](int err, const QString& what) -> Loaded {
        r.image = QImage(); r.via.clear();
        r.timedOut = irq.timedOut; r.cancelled = irq.cancelled;
        r.error = irq.timedOut ? QString("timeout after %1 ms (%2 %3)").arg(o.timeoutMs).arg(what, path)
                : irq.cancelled ? QString("cancelled (%1 %2)").arg(what, path)
                : err ? QString("%1 %2: %3").arg(what, path, avErr(err)) : QString("%1 %2").arg(what, path);
        if (!r.qtError.isEmpty()) r.error += QStringLiteral(" (Qt: %1)").arg(r.qtError);
        return r;
    };

    FmtCtx fc;
    fc.p = avformat_alloc_context();
    if (!fc.p) return fail(AVERROR(ENOMEM), "cannot open");
    fc.p->interrupt_callback.callback = irqCb;
    fc.p->interrupt_callback.opaque = &irq;
    AVDictionary* fo = nullptr;
    av_dict_set(&fo, "protocol_whitelist", "file", 0);        // local files only, nested opens included
    int err = avformat_open_input(&fc.p, path.toUtf8().constData(), nullptr, &fo);   // frees the context on failure
    av_dict_free(&fo);
    if (err < 0) return fail(err, "cannot open");
    // An image demuxer knows no size until a frame is decoded, and find_stream_info decodes one. max_pixels makes the
    // decoders refuse an oversized picture before allocating it, there and in our own decode below.
    const qint64 limit = o.maxPixels > 0 ? std::min<qint64>(o.maxPixels, INT_MAX) : 0;
    std::vector<AVDictionary*> streamOpts(fc.p->nb_streams, nullptr);
    if (limit) for (AVDictionary*& d : streamOpts) av_dict_set_int(&d, "max_pixels", limit, 0);
    err = avformat_find_stream_info(fc.p, streamOpts.empty() ? nullptr : streamOpts.data());
    for (AVDictionary*& d : streamOpts) av_dict_free(&d);
    if (err < 0) return fail(err, "no stream info in");
    if (irq.stop()) return fail(AVERROR_EXIT, "probing");
    const AVCodec* codec = nullptr;
    const int vs = av_find_best_stream(fc.p, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vs < 0 || !codec) return fail(vs < 0 ? vs : 0, "no picture in");
    AVStream* st = fc.p->streams[vs];
    for (unsigned i = 0; i < fc.p->nb_streams; ++i) if (int(i) != vs) fc.p->streams[i]->discard = AVDISCARD_ALL;
    if (limit && qint64(st->codecpar->width) * st->codecpar->height > limit)
        return fail(0, QString("picture too large (%1x%2) in").arg(st->codecpar->width).arg(st->codecpar->height));
    if (st->codecpar->width <= 0 || st->codecpar->height <= 0)
        return fail(0, limit ? QString("no picture size (not a picture, or too large: over %1 pixels) in").arg(limit)
                             : QStringLiteral("no picture size in"));

    CodecCtx dec;
    dec.p = avcodec_alloc_context3(codec);
    if (!dec.p || (err = avcodec_parameters_to_context(dec.p, st->codecpar)) < 0) return fail(err ? err : AVERROR(ENOMEM), "decoder for");
    dec.p->pkt_timebase = st->time_base;
    dec.p->thread_count = 1;
    if (limit) dec.p->max_pixels = limit;
    if ((err = avcodec_open2(dec.p, codec, nullptr)) < 0) return fail(err, "cannot open the decoder for");

    // the first picture (an animated WebP / GIF gives its first frame)
    Packet pkt;
    Frame frame;
    bool got = false, flushed = false;
    int badPackets = 0;
    while (!got) {
        if (irq.stop()) return fail(AVERROR_EXIT, "decoding");
        int rr = avcodec_receive_frame(dec.p, frame.p);
        if (rr == 0) { got = true; break; }
        if (rr == AVERROR_EOF) break;
        if (rr != AVERROR(EAGAIN)) return fail(rr, "cannot decode");
        if (flushed) break;
        rr = av_read_frame(fc.p, pkt.p);
        if (rr < 0) {
            if (irq.stop()) return fail(AVERROR_EXIT, "reading");
            avcodec_send_packet(dec.p, nullptr);
            flushed = true;
            continue;
        }
        if (pkt.p->stream_index == vs) {
            const int s = avcodec_send_packet(dec.p, pkt.p);
            if (s < 0 && s != AVERROR(EAGAIN) && ++badPackets > 64) { av_packet_unref(pkt.p); return fail(s, "cannot decode"); }
        }
        av_packet_unref(pkt.p);
    }
    if (!got) return fail(0, "no picture decoded from");
    const int w = frame.p->width, h = frame.p->height;
    if (w <= 0 || h <= 0 || (limit && qint64(w) * h > limit)) return fail(0, QString("picture of %1x%2 refused in").arg(w).arg(h));

    // orientation: the decoder's display matrix (the JPEG decoder makes one from EXIF), else the file's own EXIF block
    int orientation = 1;
    if (o.autoTransform) {
        if (const int32_t* m = displayMatrix(frame.p, st)) orientation = std::max(1, orientationFromMatrix(m));
        else orientation = std::max(1, fileOrientation(path));
    }

    // to BGRA = QImage::Format_ARGB32's bytes on a little-endian CPU
    Frame src;
    if ((err = av_frame_ref(src.p, frame.p)) < 0) return fail(err, "frame of");
    if (plainTwin(AVPixelFormat(src.p->format)) != AVPixelFormat(src.p->format)) {
        src.p->format = plainTwin(AVPixelFormat(src.p->format));
        src.p->color_range = AVCOL_RANGE_JPEG;
    }
    Frame dst;
    dst.p->format = AV_PIX_FMT_BGRA;
    dst.p->width = w; dst.p->height = h;
    Scaler sws;
    sws.p = sws_alloc_context();
    if (!sws.p) return fail(AVERROR(ENOMEM), "scaler for");
    sws.p->flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    sws.p->threads = 1;
    if ((err = sws_scale_frame(sws.p, dst.p, src.p)) < 0) {
        av_frame_unref(dst.p);
        dst.p->format = AV_PIX_FMT_BGRA; dst.p->width = w; dst.p->height = h;
        if ((err = scaleLegacy(src.p, dst.p)) < 0)
            return fail(err, QString("cannot convert %1 from").arg(QString::fromLatin1(av_get_pix_fmt_name(AVPixelFormat(src.p->format)))));
    }
    if (irq.stop()) return fail(AVERROR_EXIT, "converting");
    QImage img(w, h, QImage::Format_ARGB32);
    if (img.isNull()) return fail(AVERROR(ENOMEM), "no memory for");
    for (int y = 0; y < h; ++y) std::memcpy(img.scanLine(y), dst.p->data[0] + qsizetype(y) * dst.p->linesize[0], size_t(w) * 4);

    r.via = QStringLiteral("libav");
    r.format = QString::fromLatin1(codec->name);
    r.storedSize = QSize(w, h);
    r.orientation = orientation;
    r.image = orient(normalized(img), orientation);
    return r;
}

}   // namespace

// ---------------------------------------------------------------------------------------------- load
Loaded load(const QString& path, const LoadOptions& o) {
    Loaded r;
    if (path.isEmpty()) { r.error = QStringLiteral("no picture path"); return r; }
    if (isUrl(path)) { r.error = QStringLiteral("not a local file: %1").arg(path); return r; }
    const QFileInfo fi(path);
    if (!fi.exists()) { r.error = QStringLiteral("no such file: %1").arg(path); return r; }
    if (!fi.isFile()) { r.error = QStringLiteral("not a file: %1").arg(path); return r; }
    if (!o.allowTruncated) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { r.error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString()); return r; }
        const QString why = truncationOf(fileReader(f), f.size());
        if (!why.isEmpty()) { r.truncated = true; r.error = QStringLiteral("truncated: %1: %2").arg(why, path); return r; }
    }
    if (o.useQt) {
        QImageReader reader(path);
        reader.setAutoTransform(o.autoTransform);
        reader.setDecideFormatFromContent(true);              // a PNG named .jpg is still a PNG
        if (o.maxPixels > 0) reader.setAllocationLimit(int(std::min<qint64>((o.maxPixels * 4 >> 20) + 1, 1 << 20)));
        if (reader.canRead()) {
            const QSize sz = reader.size();
            if (o.maxPixels > 0 && sz.isValid() && qint64(sz.width()) * sz.height() > o.maxPixels) {
                r.error = QStringLiteral("picture too large (%1x%2): %3").arg(sz.width()).arg(sz.height()).arg(path);
                return r;
            }
            // Qt's JPEG and TIFF handlers turn the picture themselves; for the others (PNG eXIf, WebP EXIF, which
            // exif_transpose also honours) the file's own EXIF block is applied after reading.
            const bool qtTurns = reader.supportsOption(QImageIOHandler::ImageTransformation);
            const int orientation = !o.autoTransform ? 1
                                  : qtTurns ? exifFromTransform(reader.transformation()) : std::max(1, fileOrientation(path));
            const QByteArray fmt = reader.format();             // only answers before read(): it asks the handler canRead()
            QImage img = reader.read();
            if (!img.isNull()) {
                r.via = QStringLiteral("qt");
                r.format = QString::fromLatin1(fmt);
                r.orientation = orientation;
                if (qtTurns) r.storedSize = orientation >= 5 ? img.size().transposed() : img.size();
                else { r.storedSize = img.size(); img = orient(img, orientation); }
                r.image = normalized(img);
                return r;
            }
        }
        r.qtError = reader.errorString();
    }
    if (o.useLibav) return loadLibav(path, o, r);
    r.error = QStringLiteral("cannot read %1: %2").arg(path, r.qtError.isEmpty() ? QStringLiteral("no decoder allowed") : r.qtError);
    return r;
}

// ---------------------------------------------------------------------------------------------- orientation
QImage orient(const QImage& in, int o) {
    if (in.isNull() || o < 2 || o > 8) return in;
    const QImage src = to32(in);
    const int W = src.width(), H = src.height();
    const bool swap = o >= 5;
    QImage out(swap ? H : W, swap ? W : H, src.format());
    if (out.isNull()) return in;
    for (int Y = 0; Y < out.height(); ++Y) {
        auto* d = reinterpret_cast<quint32*>(out.scanLine(Y));
        for (int X = 0; X < out.width(); ++X) {
            int x, y;                                          // the stored pixel shown at (X, Y)
            switch (o) {
            case 2: x = W - 1 - X; y = Y; break;               // mirror
            case 3: x = W - 1 - X; y = H - 1 - Y; break;       // rotate 180
            case 4: x = X; y = H - 1 - Y; break;               // flip
            case 5: x = Y; y = X; break;                       // transpose
            case 6: x = Y; y = H - 1 - X; break;               // rotate 90 clockwise
            case 7: x = W - 1 - Y; y = H - 1 - X; break;       // transverse
            default: x = W - 1 - Y; y = X; break;              // 8: rotate 90 anticlockwise
            }
            d[X] = reinterpret_cast<const quint32*>(src.constScanLine(y))[x];
        }
    }
    out.setDotsPerMeterX(swap ? src.dotsPerMeterY() : src.dotsPerMeterX());
    out.setDotsPerMeterY(swap ? src.dotsPerMeterX() : src.dotsPerMeterY());
    return out;
}

int orientationFromMatrix(const int32_t m[9]) {
    // fftools get_rotation(): theta = -round(rotation), folded into [0, 360); then configure_input_video_filter's branches
    double theta = -std::round(av_display_rotation_get(m));
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    if (!std::isfinite(theta)) return 0;
    if (std::fabs(theta - 90) < 1.0) return m[3] > 0 ? 5 : 6;           // transpose=cclock_flip : clock
    if (std::fabs(theta - 180) < 1.0) {
        const bool hflip = m[0] < 0, vflip = m[4] < 0;
        return hflip && vflip ? 3 : hflip ? 2 : vflip ? 4 : 1;
    }
    if (std::fabs(theta - 270) < 1.0) return m[3] < 0 ? 7 : 8;          // transpose=clock_flip : cclock
    if (std::fabs(theta) > 1.0) return 0;                               // ffmpeg's rotate filter: not reproduced
    return m[4] < 0 ? 4 : 1;
}

int exifOrientation(const QByteArray& head) {
    return containerOrientation([&head](qint64 pos, qint64 len) { return pos < 0 || len <= 0 ? QByteArray() : head.mid(pos, len); }, head.size());
}

QString truncation(const QByteArray& file) {
    return truncationOf([&file](qint64 pos, qint64 len) { return pos < 0 || len <= 0 ? QByteArray() : file.mid(pos, len); }, file.size());
}

// ---------------------------------------------------------------------------------------------- resize, crop, brightness
QImage resizeHQ(const QImage& in, const QSize& size) {
    if (in.isNull() || size.isEmpty()) return {};
    QImage src = to32(in);
    if (src.size() == size) return src;
    const bool lanczos = size.width() > src.width() || size.height() > src.height()
                      || src.width() > 2 * size.width() || src.height() > 2 * size.height();
    if (!lanczos) return src.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int sw = src.width(), sh = src.height(), dw = size.width(), dh = size.height();
    SwsContext* c = sws_getContext(sw, sh, AV_PIX_FMT_GRAY8, dw, dh, AV_PIX_FMT_GRAY8, SWS_LANCZOS | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (!c) return src.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // One channel at a time through a grey plane: no colour conversion, every byte resampled on its own (premultiplied
    // colour with its alpha, so transparent pixels do not bleed).
    const bool alpha = src.format() == QImage::Format_ARGB32_Premultiplied;
    QImage out(dw, dh, src.format());
    out.fill(0xff000000u);                                     // RGB32's unused byte stays 0xff
    std::vector<uint8_t> sp(size_t(sw) * size_t(sh)), dp(size_t(dw) * size_t(dh));
    for (int ch = 0; ch < (alpha ? 4 : 3); ++ch) {
        for (int y = 0; y < sh; ++y) {
            const uchar* row = src.constScanLine(y);
            uint8_t* o = sp.data() + size_t(y) * size_t(sw);
            for (int x = 0; x < sw; ++x) o[x] = row[x * 4 + ch];
        }
        const uint8_t* srcPlanes[4] = {sp.data(), nullptr, nullptr, nullptr};
        const int srcStride[4] = {sw, 0, 0, 0};
        uint8_t* dstPlanes[4] = {dp.data(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {dw, 0, 0, 0};
        sws_scale(c, srcPlanes, srcStride, 0, sh, dstPlanes, dstStride);
        for (int y = 0; y < dh; ++y) {
            uchar* row = out.scanLine(y);
            const uint8_t* s = dp.data() + size_t(y) * size_t(dw);
            for (int x = 0; x < dw; ++x) row[x * 4 + ch] = s[x];
        }
    }
    sws_freeContext(c);
    if (alpha) {                                               // Lanczos ringing may push colour past its alpha
        for (int y = 0; y < dh; ++y) {
            uchar* row = out.scanLine(y);
            for (int x = 0; x < dw; ++x) {
                uchar* p = row + x * 4;
                const uchar a = p[3];
                p[0] = std::min(p[0], a); p[1] = std::min(p[1], a); p[2] = std::min(p[2], a);
            }
        }
    }
    out.setDotsPerMeterX(src.dotsPerMeterX());
    out.setDotsPerMeterY(src.dotsPerMeterY());
    return out;
}

QImage cover(const QImage& img, int w, int h, double ybias) {
    if (img.isNull() || w <= 0 || h <= 0) return {};
    const double k = std::max(double(w) / img.width(), double(h) / img.height());
    const int nw = std::max(w, int(std::nearbyint(img.width() * k)));    // round(): half to even
    const int nh = std::max(h, int(std::nearbyint(img.height() * k)));
    const QImage r = resizeHQ(img, QSize(nw, nh));
    return r.copy((nw - w) / 2, int((nh - h) * ybias), w, h);
}

QImage brightness(const QImage& in, double factor) {
    if (in.isNull()) return in;
    QImage img = to32(in);
    img.detach();
    const float f = float(factor);                             // ImagingBlend takes the factor as a C float
    const bool pm = img.format() == QImage::Format_ARGB32_Premultiplied;
    for (int y = 0; y < img.height(); ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            uchar* p = row + x * 4;
            for (int c = 0; c < 3; ++c) {
                int v = int(f * float(p[c]));                  // (int) truncation, then CLIP8
                v = std::clamp(v, 0, pm ? int(p[3]) : 255);
                p[c] = uchar(v);
            }
        }
    }
    return img;
}

// ---------------------------------------------------------------------------------------------- save
bool save(const QImage& img, const QString& path, QString* error, int jpegQuality) {
    auto failWith = [&](const QString& e) { if (error) *error = e; return false; };
    if (img.isNull()) return failWith(QStringLiteral("no picture to write to %1").arg(path));
    const QString suffix = QFileInfo(path).suffix().toLower();
    const QByteArray fmt = suffix == QLatin1String("png") ? QByteArray("png")
                         : suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") ? QByteArray("jpeg") : QByteArray();
    if (fmt.isEmpty()) return failWith(QStringLiteral("not a .png / .jpg path: %1").arg(path));
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) return failWith(QStringLiteral("cannot create %1").arg(dir));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return failWith(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    QImageWriter w(&f, fmt);
    QImage out = img;
    if (fmt == "jpeg") {
        out = img.convertToFormat(QImage::Format_RGB32);
        w.setQuality(std::clamp(jpegQuality, 1, 100));
    } else {
        w.setCompression(35);                                  // Qt's write_png_image maps 0..100 to zlib as v * 9 / 91: level 3
    }
    if (!w.write(out)) { f.cancelWriting(); return failWith(QStringLiteral("cannot encode %1: %2").arg(path, w.errorString())); }
    if (!f.commit()) return failWith(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    return true;
}

}   // namespace imageio
