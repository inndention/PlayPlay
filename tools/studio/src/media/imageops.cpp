// Image operations on QImage (ST-17). See imageops.h.
#include "media/imageops.h"

#include "media/avopen.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QImageWriter>
#include <QMutex>
#include <QPainter>
#include <QSet>

#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace studio::media {
namespace {

QString fmtMB(qint64 n) {
    const double mb = double(n) / double(kMiB);
    return QString::number(mb, 'f', n < 10 * kMiB ? 1 : 0) + QStringLiteral(" MB");
}

QByteArray qtFormatFor(const QString& mime) {
    if (mime == QLatin1String("image/png")) return "png";
    if (mime == QLatin1String("image/jpeg")) return "jpeg";
    if (mime == QLatin1String("image/webp")) return "webp";
    if (mime == QLatin1String("image/gif")) return "gif";
    if (mime == QLatin1String("image/bmp")) return "bmp";
    if (mime == QLatin1String("image/tiff")) return "tiff";
    return {};
}

// The plugin lists are read once per process (they only change if plugins are loaded later, which the Studio never does).
bool qtCan(bool write, const QByteArray& fmt) {
    static QMutex lock;
    static bool loaded = false;
    static QSet<QByteArray> readers, writers;
    QMutexLocker l(&lock);
    if (!loaded) {
        for (const QByteArray& f : QImageReader::supportedImageFormats()) readers.insert(f.toLower());
        for (const QByteArray& f : QImageWriter::supportedImageFormats()) writers.insert(f.toLower());
        loaded = true;
    }
    return !fmt.isEmpty() && (write ? writers : readers).contains(fmt);
}

// The demuxers the libav fallback runs: still-image formats only. Anything else is refused, not decoded: a video
// container would give its first frame, and svg_pipe would hand the bytes to librsvg (this FFmpeg build has it),
// which renders vector input with its own loader.
constexpr const char* kImageDemuxers[] = {
    "png_pipe", "jpeg_pipe", "jpegls_pipe", "webp_pipe", "gif", "gif_pipe", "bmp_pipe", "tiff_pipe",
    "ppm_pipe", "pgm_pipe", "pbm_pipe", "pam_pipe", "qoi_pipe", "jpegxl_pipe",
};

bool isImageDemuxer(const QString& name) {
    for (const char* d : kImageDemuxers) if (name == QLatin1String(d)) return true;
    return false;
}

QImage decodeWithLibav(const QByteArray& bytes, const DecodeLimits& limits, QString* error) {
    auto fail = [&](const QString& m) { if (error) *error = m; return QImage(); };
    auto tooBig = [&](int w, int h) {
        return fail(QStringLiteral("image is %1 x %2 pixels; the limit is %3 pixels").arg(w).arg(h).arg(limits.maxPixels));
    };
    av::Deadline dl(30000, nullptr);
    QString err;
    // no avformat_find_stream_info: it would decode a frame with an unlimited decoder of its own before the size
    // check below could refuse it. The image demuxers need none; the decoder opened here carries the pixel limit.
    std::unique_ptr<av::Input> in = av::openMemory(bytes, &dl, &err, false);
    if (!in) return fail(QStringLiteral("not an image (%1)").arg(err));
    if (!isImageDemuxer(in->formatName())) return fail(QStringLiteral("not an image (%1)").arg(in->formatName()));
    AVFormatContext* fc = in->fc();
    const int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) return fail(QStringLiteral("not an image (no picture in %1)").arg(in->formatName()));
    AVStream* st = fc->streams[vs];
    if (qint64(st->codecpar->width) * st->codecpar->height > limits.maxPixels) return tooBig(st->codecpar->width, st->codecpar->height);
    av::CodecPtr cc = av::openDecoder(st, 1, &err, limits.maxPixels);
    if (!cc) return fail(err);
    av::PacketPtr pkt = av::newPacket();
    av::FramePtr frame = av::newFrame();
    if (!pkt || !frame) return fail(QStringLiteral("out of memory"));
    bool got = false, drained = false;
    int sendErr = 0;
    for (int guard = 0; guard < 10000 && !got; ++guard) {
        int r = avcodec_receive_frame(cc.get(), frame.get());
        if (r == 0) { got = true; break; }
        if (r != AVERROR(EAGAIN) || drained) break;
        r = av_read_frame(fc, pkt.get());
        if (r < 0) { avcodec_send_packet(cc.get(), nullptr); drained = true; continue; }
        if (pkt->stream_index == vs && (r = avcodec_send_packet(cc.get(), pkt.get())) < 0 && r != AVERROR(EAGAIN) && !sendErr) sendErr = r;
        av_packet_unref(pkt.get());
    }
    // a picture over max_pixels ends here too: the decoder refuses it from its header (EINVAL)
    if (!got) return fail(QStringLiteral("could not decode the picture%1").arg(sendErr ? QStringLiteral(" (%1)").arg(av::errText(sendErr)) : QString()));
    const AVFrame* f = frame.get();
    if (qint64(f->width) * f->height > limits.maxPixels || f->width < 1 || f->height < 1) return tooBig(f->width, f->height);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    const bool alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
    const AVPixelFormat dstFmt = alpha ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    QImage img(f->width, f->height, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
    if (img.isNull()) return fail(QStringLiteral("out of memory"));
    av::SwsPtr sws(sws_getContext(f->width, f->height, AVPixelFormat(f->format), f->width, f->height, dstFmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!sws) return fail(QStringLiteral("no converter for this picture format"));
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_ITU601), f->color_range == AVCOL_RANGE_JPEG ? 1 : 0,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    uint8_t* stage[4] = {nullptr, nullptr, nullptr, nullptr};
    int lines[4] = {0, 0, 0, 0};
    if (av_image_alloc(stage, lines, f->width, f->height, dstFmt, 64) < 0) return fail(QStringLiteral("out of memory"));
    const bool scaled = sws_scale(sws.get(), f->data, f->linesize, 0, f->height, stage, lines) > 0;
    const size_t rowBytes = size_t(f->width) * (alpha ? 4 : 3);
    if (scaled) for (int y = 0; y < f->height; ++y) std::memcpy(img.scanLine(y), stage[0] + qsizetype(y) * lines[0], rowBytes);
    av_freep(&stage[0]);
    if (!scaled) return fail(QStringLiteral("could not convert the picture"));
    // the JPEG decoder turns EXIF orientation into a display matrix on the frame; Qt's path applies the same tag
    const AVFrameSideData* dm = av_frame_get_side_data(f, AV_FRAME_DATA_DISPLAYMATRIX);
    return dm ? orientImage(img, av::displayOrientation(dm->data, dm->size)) : img;
}

// libjpeg-style quality (30..100) to an MJPEG qscale (2 best .. 31 worst).
int qscaleFor(double quality) {
    const double q = qBound(0.3, quality, 1.0) * 100.0;
    return qBound(2, int(std::lround((100.0 - q) / 5.0 + 1.0)), 31);
}

QByteArray encodeJpegWithLibav(const QImage& flat, double quality, QString* error) {
    auto fail = [&](const QString& m) { if (error) *error = m; return QByteArray(); };
    const QImage rgb = flat.convertToFormat(QImage::Format_RGB888);
    const int w = rgb.width(), h = rgb.height();
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return fail(QStringLiteral("no JPEG encoder"));
    av::CodecPtr enc(avcodec_alloc_context3(codec));
    if (!enc) return fail(QStringLiteral("out of memory"));
    const int qs = qscaleFor(quality);
    enc->width = w; enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_YUV420P; enc->color_range = AVCOL_RANGE_JPEG;   // full range = JFIF (yuvj420p is deprecated)
    enc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    enc->time_base = AVRational{1, 25};
    enc->flags |= AV_CODEC_FLAG_QSCALE;
    enc->global_quality = FF_QP2LAMBDA * qs;
    int r = avcodec_open2(enc.get(), codec, nullptr);
    if (r < 0) return fail(QStringLiteral("JPEG encoder: ") + av::errText(r));
    av::FramePtr yuv = av::newFrame();
    if (!yuv) return fail(QStringLiteral("out of memory"));
    yuv->format = AV_PIX_FMT_YUV420P; yuv->width = w; yuv->height = h;
    yuv->color_range = AVCOL_RANGE_JPEG;
    if ((r = av_frame_get_buffer(yuv.get(), 32)) < 0) return fail(QStringLiteral("out of memory"));
    av::SwsPtr sws(sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!sws) return fail(QStringLiteral("no converter to YUV"));
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_ITU601), 1, sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
    const uint8_t* src[4] = {rgb.constBits(), nullptr, nullptr, nullptr};
    const int srcLines[4] = {int(rgb.bytesPerLine()), 0, 0, 0};
    if (sws_scale(sws.get(), src, srcLines, 0, h, yuv->data, yuv->linesize) <= 0) return fail(QStringLiteral("could not convert to YUV"));
    yuv->quality = FF_QP2LAMBDA * qs;
    yuv->pts = 0;
    QByteArray out;
    if ((r = avcodec_send_frame(enc.get(), yuv.get())) >= 0) {
        avcodec_send_frame(enc.get(), nullptr);
        av::PacketPtr p = av::newPacket();
        while (p && (r = avcodec_receive_packet(enc.get(), p.get())) == 0) {
            out.append(reinterpret_cast<const char*>(p->data), p->size);
            av_packet_unref(p.get());
        }
    }
    if (out.isEmpty()) return fail(QStringLiteral("JPEG encode failed: ") + av::errText(r));
    return out;
}

// 5 x 7 glyphs, one byte per row, bit 4 = leftmost column.
struct Glyph { char c; uchar rows[7]; };
constexpr Glyph kGlyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}}, {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
};

const Glyph& glyphFor(QChar ch) {
    const char c = char(ch.toUpper().unicode() < 128 ? ch.toUpper().unicode() : '?');
    for (const Glyph& g : kGlyphs) if (g.c == c) return g;
    for (const Glyph& g : kGlyphs) if (g.c == '?') return g;
    return kGlyphs[0];
}

void paintPixelText(QPainter& p, int x, int y, const QString& text, int px, QRgb color) {
    const QColor col = QColor::fromRgba(color);
    for (int i = 0; i < text.size(); ++i) {
        const Glyph& g = glyphFor(text.at(i));
        const int gx = x + i * 6 * px;
        for (int row = 0; row < 7; ++row)
            for (int col5 = 0; col5 < 5; ++col5)
                if (g.rows[row] & (0x10 >> col5)) p.fillRect(gx + col5 * px, y + row * px, px, px, col);
    }
}

} // namespace

QString sniffMime(QByteArrayView b) {
    auto starts = [&](const char* sig, qsizetype n, qsizetype at = 0) { return b.size() >= at + n && std::memcmp(b.data() + at, sig, size_t(n)) == 0; };
    if (starts("\x89PNG\r\n\x1a\n", 8)) return QStringLiteral("image/png");
    if (starts("\xFF\xD8\xFF", 3)) return QStringLiteral("image/jpeg");
    if (starts("RIFF", 4) && starts("WEBP", 4, 8)) return QStringLiteral("image/webp");
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return QStringLiteral("image/gif");
    if (starts("BM", 2) && b.size() >= 26) return QStringLiteral("image/bmp");
    if (starts("II*\0", 4) || starts("MM\0*", 4)) return QStringLiteral("image/tiff");
    return QString();
}

QImage decodeImage(const QByteArray& bytes, QString* error, const DecodeLimits& limits, Codec codec) {
    auto fail = [&](const QString& m) { if (error) *error = m; return QImage(); };
    if (bytes.isEmpty()) return fail(QStringLiteral("no image data"));
    if (bytes.size() > limits.maxBytes) return fail(QStringLiteral("image is %1; the limit is %2").arg(fmtMB(bytes.size()), fmtMB(limits.maxBytes)));
    const QByteArray fmt = qtFormatFor(sniffMime(bytes));
    if (codec != Codec::Libav && qtCan(false, fmt)) {
        QByteArray copy = bytes;   // QBuffer needs a non-const array; implicitly shared, no copy is made
        QBuffer buf(&copy);
        buf.open(QIODevice::ReadOnly);
        QImageReader r(&buf, fmt);
        // EXIF orientation applied (off by default in Qt): a phone photo stored sideways comes back upright
        r.setAutoTransform(true);
        // No setAllocationLimit here: it is a static, one int for every QImageReader / QImage::load on every thread,
        // so a per-call value would race with other workers and leak into unrelated loads. Our pixel limit is the
        // header check below (the pixel count is the same before and after the turn); Qt's process default (256 MB)
        // stays as the backstop for a header that does not say.
        auto tooBig = [&](const QSize& s) { return qint64(s.width()) * s.height() > limits.maxPixels; };
        auto tooBigText = [&](const QSize& s) {
            return QStringLiteral("image is %1 x %2 pixels; the limit is %3 pixels").arg(s.width()).arg(s.height()).arg(limits.maxPixels);
        };
        const QSize sz = r.size();
        if (sz.isValid() && tooBig(sz)) return fail(tooBigText(sz));
        QImage img = r.read();
        if (!img.isNull()) return tooBig(img.size()) ? fail(tooBigText(img.size())) : img;
        if (codec == Codec::Qt) return fail(QStringLiteral("could not decode the image: %1").arg(r.errorString()));
    } else if (codec == Codec::Qt) {
        return fail(QStringLiteral("no Qt image plugin for this format"));
    }
    return decodeWithLibav(bytes, limits, error);
}

QImage orientImage(const QImage& img, int o) {
    if (img.isNull() || o < 2 || o > 8) return img;
    QImage src = img;
    if (src.depth() < 8 || src.depth() % 8) src = src.convertToFormat(src.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    const int w = src.width(), h = src.height(), bpp = src.depth() / 8;
    const qsizetype bpl = src.bytesPerLine();
    const bool swap = o >= 5;
    QImage out(swap ? h : w, swap ? w : h, src.format());
    if (out.isNull()) return {};
    out.setColorTable(src.colorTable());
    out.setColorSpace(src.colorSpace());
    // output row y walks the source from `start` in steps of `step` bytes (a column when the sides swap)
    qsizetype step = 0;
    auto start = [&](int y) -> qsizetype {
        switch (o) {
        case 2: step = -bpp; return qsizetype(y) * bpl + qsizetype(w - 1) * bpp;              // mirror left-right
        case 3: step = -bpp; return qsizetype(h - 1 - y) * bpl + qsizetype(w - 1) * bpp;      // 180
        case 4: step = bpp;  return qsizetype(h - 1 - y) * bpl;                               // mirror top-bottom
        case 5: step = bpl;  return qsizetype(y) * bpp;                                       // transpose
        case 6: step = -bpl; return qsizetype(h - 1) * bpl + qsizetype(y) * bpp;              // 90 clockwise
        case 7: step = -bpl; return qsizetype(h - 1) * bpl + qsizetype(w - 1 - y) * bpp;      // transverse
        default: step = bpl; return qsizetype(w - 1 - y) * bpp;                               // 8: 90 counter-clockwise
        }
    };
    const uchar* bits = src.constBits();
    for (int y = 0; y < out.height(); ++y) {
        uchar* d = out.scanLine(y);
        qsizetype off = start(y);
        for (int x = 0; x < out.width(); ++x, d += bpp, off += step) std::memcpy(d, bits + off, size_t(bpp));
    }
    return out;
}

QByteArray encodeJpeg(const QImage& img, double quality, QString* error, Codec codec) {
    if (img.isNull()) { if (error) *error = QStringLiteral("no image"); return {}; }
    const double q = std::isfinite(quality) ? qBound(0.3, quality, 1.0) : 0.85;
    const QImage flat = img.hasAlphaChannel() ? flatten(img) : img;
    if (codec != Codec::Libav && qtCan(true, "jpeg")) {
        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, "jpeg");
        w.setQuality(int(std::lround(q * 100)));
        if (w.write(flat) && !out.isEmpty()) return out;
        if (codec == Codec::Qt) { if (error) *error = QStringLiteral("JPEG encode failed: ") + w.errorString(); return {}; }
    } else if (codec == Codec::Qt) {
        if (error) *error = QStringLiteral("no Qt JPEG plugin");
        return {};
    }
    return encodeJpegWithLibav(flat, q, error);
}

QByteArray encodePng(const QImage& img, QString* error) {
    if (img.isNull()) { if (error) *error = QStringLiteral("no image"); return {}; }
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, "png");
    if (!w.write(img)) { if (error) *error = QStringLiteral("PNG encode failed: ") + w.errorString(); return {}; }
    return out;
}

QImage flatten(const QImage& img, const QColor& background) {
    if (img.isNull()) return {};
    if (!img.hasAlphaChannel()) return img.convertToFormat(QImage::Format_RGB32);
    QImage out(img.size(), QImage::Format_RGB32);
    if (out.isNull()) return {};
    out.fill(background);
    QPainter p(&out);
    p.drawImage(0, 0, img);
    p.end();
    return out;
}

QImage fitInside(const QImage& img, int maxW, int maxH) {
    if (img.isNull()) return {};
    double k = 1.0;
    if (maxW > 0) k = qMin(k, double(maxW) / img.width());
    if (maxH > 0) k = qMin(k, double(maxH) / img.height());
    if (k >= 1.0) return img;
    const int w = qMax(1, int(std::lround(img.width() * k))), h = qMax(1, int(std::lround(img.height() * k)));
    return img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage coverFit(const QImage& img, int w, int h) {
    if (img.isNull() || w < 1 || h < 1) return {};
    const double s = qMax(double(w) / img.width(), double(h) / img.height());
    const int sw = qMax(w, int(std::ceil(img.width() * s - 1e-9))), sh = qMax(h, int(std::ceil(img.height() * s - 1e-9)));
    const QImage scaled = img.scaled(sw, sh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return scaled.copy((sw - w) / 2, (sh - h) / 2, w, h);
}

QByteArray rgbThumb(const QImage& img, int w, int h) {
    if (img.isNull() || w < 1 || h < 1) return {};
    const QImage t = flatten(img).scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    QByteArray out(qsizetype(w) * h * 3, Qt::Uninitialized);
    for (int y = 0; y < h; ++y) std::memcpy(out.data() + qsizetype(y) * w * 3, t.constScanLine(y), size_t(w) * 3);
    return out;
}

Shrunk shrinkImage(const QByteArray& bytes, const ImageLimit& limit, const QString& mime) {
    Shrunk s;
    const QString m = mime.isEmpty() ? sniffMime(bytes) : mime;
    QString err;
    const QImage img = decodeImage(bytes, &err);
    if (img.isNull()) { s.error = err; return s; }
    const int side = qMax(img.width(), img.height());
    const bool webSafe = m.contains(QLatin1String("jpeg")) || m.contains(QLatin1String("png")) || m.contains(QLatin1String("webp"));
    if (side <= limit.maxSide && bytes.size() <= limit.maxBytes && webSafe) {
        s.ok = true; s.bytes = bytes; s.mime = m; s.width = img.width(); s.height = img.height();
        return s;
    }
    const QImage flat = flatten(img);   // JPEG has no alpha: flatten onto white, not black
    double scale = limit.maxSide > 0 ? qMin(1.0, double(limit.maxSide) / side) : 1.0;
    for (int pass = 0; pass < 6; ++pass) {
        const int w = qMax(1, int(std::lround(img.width() * scale))), h = qMax(1, int(std::lround(img.height() * scale)));
        const QImage scaled = (w == flat.width() && h == flat.height()) ? flat : flat.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (const double q : {0.9, 0.82, 0.72}) {
            const QByteArray out = encodeJpeg(scaled, q, &err);
            if (out.isEmpty()) { s.error = err; return s; }
            if (out.size() <= limit.maxBytes) {
                s.ok = true; s.bytes = out; s.mime = QStringLiteral("image/jpeg"); s.width = w; s.height = h; s.changed = true; s.quality = q;
                return s;
            }
        }
        scale *= 0.75;
    }
    s.error = QStringLiteral("this image could not be made smaller than %1").arg(fmtMB(limit.maxBytes));
    return s;
}

QByteArray dataUrl(QByteArrayView bytes, const QString& mime) {
    return "data:" + (mime.isEmpty() ? QByteArray("application/octet-stream") : mime.toLatin1()) + ";base64,"
         + QByteArray::fromRawData(bytes.data(), bytes.size()).toBase64();
}

int pixelTextWidth(const QString& text, int px) {
    px = qMax(1, px);
    return text.isEmpty() ? 0 : int(text.size()) * 6 * px - px;
}

void drawPixelText(QImage& img, int x, int y, const QString& text, int px, QRgb color) {
    if (img.isNull()) return;
    QPainter p(&img);
    paintPixelText(p, x, y, text, qMax(1, px), color);
}

ContactSheet contactSheet(const std::vector<SheetInput>& cells, int cellW, double quality) {
    ContactSheet out;
    if (cells.empty()) { out.error = QStringLiteral("contact sheet needs at least one image"); return out; }
    cellW = qMax(64, cellW);
    const int cellH = int(std::lround(cellW * 9.0 / 16.0));
    const double q = std::isfinite(quality) ? qBound(0.3, quality, 1.0) : 0.82;
    out.layout = contactSheetLayout(int(cells.size()), cellW, cellH, 6);
    QImage sheet(out.layout.width, out.layout.height, QImage::Format_RGB32);
    if (sheet.isNull()) { out.error = QStringLiteral("out of memory"); return out; }
    sheet.fill(QColor(0x18, 0x18, 0x1b));   // dark gutters read as separators, not content
    const int fontPx = qMax(12, int(std::lround(cellH * 0.11)));
    const int unit = qMax(1, int(std::lround(fontPx * 0.1)));   // glyph cap height 7 units ~ 0.7 em
    QPainter p(&sheet);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const SheetCell& cell : out.layout.cells) {
        const SheetInput& src = cells[size_t(cell.i)];
        const QRect r(cell.x, cell.y, cell.w, cell.h);
        p.setClipRect(r);
        if (!src.image.isNull()) p.drawImage(cell.x, cell.y, coverFit(src.image, cell.w, cell.h));
        else { p.fillRect(r, QColor(0x3f, 0x3f, 0x46)); out.failed << src.label; }
        // label pill, bottom-left
        const int padX = int(std::lround(fontPx * 0.55)), pillH = int(std::lround(fontPx * 1.6));
        const int pillW = pixelTextWidth(src.label, unit) + padX * 2;
        const int px = cell.x + int(std::lround(fontPx * 0.5)), py = cell.y + cell.h - pillH - int(std::lround(fontPx * 0.5));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, int(std::lround(0.72 * 255))));
        p.drawRoundedRect(QRectF(px, py, pillW, pillH), pillH / 2.0, pillH / 2.0);
        p.setRenderHint(QPainter::Antialiasing, false);
        paintPixelText(p, px + padX, py + (pillH - 7 * unit) / 2, src.label, unit, qRgb(255, 255, 255));
        p.setRenderHint(QPainter::Antialiasing, true);
    }
    p.end();
    QString err;
    out.jpeg = encodeJpeg(sheet, q, &err);
    if (out.jpeg.isEmpty()) { out.error = err; return out; }
    out.ok = true;
    return out;
}

} // namespace studio::media
