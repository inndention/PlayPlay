#ifndef SPOTRENDER_IMAGEIO_H
#define SPOTRENDER_IMAGEIO_H

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <cstdint>

/**
 * imageio: picture input and output for the card renderer (DEP-08a, docs/plans/inhouse-deps.md §6.3), standing in for
 * spot_card.py's `ImageOps.exif_transpose(Image.open(p)).convert("RGB")`, `img.resize(.., LANCZOS)` and `img.save(..)`.
 *  - load(): QImageReader first, with setAutoTransform(true): the EXIF orientation is applied, as exif_transpose does
 *    (for a format whose Qt handler does not apply it itself, such as PNG eXIf or WebP EXIF, from the file's own block).
 *    When Qt cannot read the file (WebP, GIF, TIFF, AVIF where only qjpeg is staged, or a file Qt chokes on), libav
 *    decodes the first picture instead. The orientation then comes from the decoder's display matrix (the JPEG decoder
 *    turns EXIF into one), read with ffmpeg's own autorotate rule, else from the file's EXIF block (JPEG APP1, WebP
 *    "EXIF", PNG "eXIf"). libav opens local files only (protocol_whitelist=file), under a deadline and an optional cancel
 *    flag. Both paths refuse pictures over maxPixels before allocating them (Qt from the header size, libav through
 *    the decoders' max_pixels, which also covers the frame find_stream_info decodes).
 *  - Truncated files are refused, as Pillow refuses them ("image file is truncated"; spot_card then uses the gradient
 *    bed). Qt and libav would decode what is there and fill the rest with grey, so a JPEG must reach an EOI marker after
 *    its first scan, a PNG must not stop inside a chunk or right after image data, and a WebP must be as long as its
 *    RIFF header says. Other formats are left to the decoders.
 *  - The result is Format_RGB32 when every pixel is opaque, else Format_ARGB32_Premultiplied.
 *  - resizeHQ(): Qt's smooth scaling between 1x and 2x down; libswscale Lanczos for a stronger reduction and for any
 *    enlargement (closer to Pillow's LANCZOS than Qt's bilinear upscale). Every channel is resampled on its own.
 *  - cover() = _cover(), brightness() = ImageEnhance.Brightness().enhance() (Image.blend against black).
 *  - save(): PNG at zlib level 3 (compress_level=3) or JPEG at quality 90, by extension. The write is atomic (QSaveFile),
 *    so the compose step never sees half a picture, and the folder is created first (Path.parent.mkdir).
 * QtGui + libavformat/libavcodec/libswscale. No QGuiApplication needed, but QImageReader needs a QCoreApplication for its
 * plugins.
 */
namespace imageio {

struct LoadOptions {
    bool autoTransform = true;                   // apply the EXIF orientation (ImageOps.exif_transpose)
    bool useQt = true;                           // QImageReader first
    bool useLibav = true;                        // libav when Qt declines or fails
    int timeoutMs = 20000;                       // libav deadline (open + decode); <= 0 = none
    qint64 maxPixels = 89478485;                 // Pillow's DecompressionBomb warning threshold (about 342 MB as 32-bit
                                                 // pixels; Pillow itself loads up to twice that); <= 0 = no limit
    bool allowTruncated = false;                 // ImageFile.LOAD_TRUNCATED_IMAGES: decode a file that stops early
    const std::atomic<bool>* cancel = nullptr;   // libav: stops at the next interrupt check / packet
};

struct Loaded {
    QImage image;                                // null on failure
    QString via;                                 // "qt" | "libav" | ""
    QString format;                              // Qt's format name ("jpeg", "png") or the libav decoder ("webp", "gif")
    int orientation = 1;                         // the EXIF orientation applied (1 = none)
    QSize storedSize;                            // as stored in the file, before the orientation
    QString qtError;                             // why Qt declined, when libav took over
    QString error;                               // why the picture could not be loaded
    bool timedOut = false, cancelled = false, truncated = false;
    bool ok() const { return !image.isNull(); }
};

Loaded load(const QString& path, const LoadOptions& o = {});

/// The picture as EXIF orientation 1..8 says to show it (2 mirror, 3 rotate 180, 4 flip, 5 transpose, 6 rotate 90
/// clockwise, 7 transverse, 8 rotate 90 anticlockwise); anything else returns it unchanged. 32-bit formats keep theirs;
/// others are converted to RGB32 / ARGB32_Premultiplied first.
QImage orient(const QImage& img, int exifOrientation);
/// ffmpeg's autorotate decision for a display matrix (fftools configure_input_video_filter) as an EXIF orientation:
/// 90 -> 5 (transpose=cclock_flip) or 6 (clock), 180 -> 2 / 3 / 4 (hflip / both / vflip), 270 -> 7 (clock_flip) or 8
/// (cclock), 0 -> 1 or 4 (vflip). 0 = not a quarter turn (ffmpeg's rotate filter) or a degenerate matrix.
int orientationFromMatrix(const int32_t matrix[9]);
/// The Orientation tag (0x0112) of an EXIF block at the start of a file: JPEG APP1, WebP RIFF "EXIF", PNG "eXIf", or a
/// bare TIFF / "Exif\0\0" block. 0 when there is none.
int exifOrientation(const QByteArray& fileHead);
/// Why the JPEG / PNG / WebP in `file` stops early (load()'s truncation rule), or "" when it is complete or another format.
QString truncation(const QByteArray& file);

QImage resizeHQ(const QImage& img, const QSize& size);
/// _cover(img, w, h, ybias): scale by max(w / iw, h / ih) to max(w, round(iw k)) x max(h, round(ih k)) (round half to
/// even, as Python's round()), crop w x h at x = (nw - w) // 2, y = int((nh - h) * ybias).
QImage cover(const QImage& img, int w, int h, double ybias = 0.4);
/// ImageEnhance.Brightness(img).enhance(factor): each colour byte becomes (uint8)(factor * v) (clipped above 1.0);
/// alpha is kept (premultiplied colour stays under its alpha).
QImage brightness(const QImage& img, double factor);

/// PNG (.png, zlib level 3) or JPEG (.jpg / .jpeg, `jpegQuality`, flattened to RGB) by extension, atomically.
bool save(const QImage& img, const QString& path, QString* error = nullptr, int jpegQuality = 90);

}   // namespace imageio

#endif // SPOTRENDER_IMAGEIO_H
