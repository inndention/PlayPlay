#pragma once
// Image operations on QImage (ST-17): decode untrusted bytes with limits, encode JPEG / PNG, fit / cover / flatten,
// the request-size shrink of StarMedia's payload.ts (shrinkImage, REFERENCE_IMAGE / FIRST_FRAME), packed RGB
// thumbnails for the stills measures, data: URLs, and the numbered contact sheet of scrub.ts for the vision pass.
//
// Works in a QCoreApplication (the Studio service has no GUI): no QPixmap, no fonts. Contact-sheet labels are drawn
// with a built-in 5x7 pixel font, so the same input gives the same pixels on every machine. JPEG and WebP go through
// Qt's image plugins when they are deployed (qjpeg / qwebp) and through libav otherwise (MJPEG encoder, libav's
// still-image decoders), so a missing plugin never breaks a spot. PNG is built into QtGui.
//
// Pure functions of their inputs, thread-safe; large images cost real CPU, so run them on a worker.
#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <vector>

#include "media/stills.h"   // SheetLayout

namespace studio::media {

constexpr qint64 kMiB = 1024 * 1024;

/// payload.ts ImageLimit: longest side in pixels and bytes of the encoded image.
struct ImageLimit { int maxSide = 0; qint64 maxBytes = 0; };
/// References and first frames: large enough for any model to see detail, small enough for every provider.
constexpr ImageLimit kReferenceImage{1536, 3 * kMiB};
constexpr ImageLimit kFirstFrame{1920, 4 * kMiB};

/// Which encoder / decoder to use. Auto = Qt when its plugin handles the format, libav otherwise.
enum class Codec { Auto, Qt, Libav };

struct DecodeLimits {
    qint64 maxBytes = 64 * kMiB;               // input size
    qint64 maxPixels = 50'000'000;             // width x height, checked from the header before any pixel is decoded
};

/// "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp", "image/tiff" from the magic bytes; "" otherwise.
QString sniffMime(QByteArrayView bytes);

/// Decodes untrusted bytes. Refuses (null image, `error`) inputs over the limits and anything that is not an image.
/// The EXIF orientation of a photo is applied (the picture comes back upright, as createImageBitmap gives it to
/// payload.ts), so a re-encode never bakes in a sideways phone photo. The result keeps its alpha channel when it has
/// one. `codec` forces a path (tests); Auto falls back to libav when Qt cannot read the format. The libav path takes
/// still-image formats only (PNG, JPEG, WebP, GIF, BMP, TIFF, PNM, QOI, JPEG XL): a video container, SVG and anything
/// else are refused, and its decoder refuses a picture over maxPixels from the header, before allocating it.
/// Never touches process-wide Qt state (QImageReader::setAllocationLimit is one static for every thread).
QImage decodeImage(const QByteArray& bytes, QString* error = nullptr, const DecodeLimits& limits = DecodeLimits(),
                   Codec codec = Codec::Auto);

/// The image turned / mirrored by an EXIF orientation code (2..8; 1, 0 or out of range: unchanged; 5..8 swap width and
/// height): 6 = turn 90 degrees clockwise, 8 = 90 counter-clockwise, 3 = 180, 2 / 4 = mirror left-right / top-bottom,
/// 5 / 7 = transpose / transverse. Exact pixel moves (no resampling) for any format of 8..64 bits per pixel; 1-bit and
/// other packed formats are converted to (A)RGB32 first.
QImage orientImage(const QImage& img, int exifOrientation);

/// JPEG at quality 0.3..1 (JPEG has no alpha: transparent pixels are flattened onto white first). Empty on failure.
QByteArray encodeJpeg(const QImage& img, double quality = 0.85, QString* error = nullptr, Codec codec = Codec::Auto);
/// Lossless PNG (alpha kept).
QByteArray encodePng(const QImage& img, QString* error = nullptr);

/// The image composited onto `background`, without an alpha channel (Format_RGB32).
QImage flatten(const QImage& img, const QColor& background = QColor(Qt::white));
/// Scaled to fit inside maxW x maxH (<= 0: no limit on that side), aspect kept, never upscaled, smooth.
QImage fitInside(const QImage& img, int maxW, int maxH);
/// Scaled to cover w x h exactly (aspect kept, centre crop), smooth.
QImage coverFit(const QImage& img, int w, int h);
/// Packed RGB24 (w*h*3 bytes) of the image stretched to w x h, flattened onto white: a stills.h signature for a
/// picture (near-duplicate checks between an upload and a scrubbed still use the same measure).
QByteArray rgbThumb(const QImage& img, int w, int h);

/// payload.ts shrinkImage: an image that already fits (longest side <= maxSide, bytes <= maxBytes, and JPEG / PNG /
/// WebP) comes back unchanged, so PNG transparency survives when it can. Otherwise JPEG on white at qualities
/// 0.9 / 0.82 / 0.72, then 0.75x smaller, up to 6 passes; after that an error says it could not be made small enough.
struct Shrunk {
    bool ok = false;
    QString error;
    QByteArray bytes;
    QString mime;
    int width = 0, height = 0;
    bool changed = false;                      // re-encoded (false: the input bytes as they were)
    double quality = 0;                        // JPEG quality used when changed
};
Shrunk shrinkImage(const QByteArray& bytes, const ImageLimit& limit = kReferenceImage, const QString& mime = QString());

/// "data:<mime>;base64,<...>" (the inline form of vision requests).
QByteArray dataUrl(QByteArrayView bytes, const QString& mime);

/// A numbered contact sheet (scrub.ts contactSheet): cells cover-fitted into contactSheetLayout(n, cellW, cellW*9/16,
/// gap 6) on a #18181b background, each label in a dark pill bottom-left. A cell whose image is null is drawn #3f3f46
/// but still labelled (the numbering must match the frame list in the prompt) and listed in `failed`.
struct SheetInput { QImage image; QString label; };
struct ContactSheet {
    bool ok = false;
    QString error;
    QByteArray jpeg;
    SheetLayout layout;
    QStringList failed;                        // labels of the cells drawn empty
};
ContactSheet contactSheet(const std::vector<SheetInput>& cells, int cellW = 384, double quality = 0.82);

/// The pixel font: width in pixels of `text` at `px` pixels per font unit (glyphs are 5 x 7 units, 1 unit apart),
/// and drawing it at (x, y) = top-left. Letters are upper-cased; characters the font lacks draw as '?'.
int pixelTextWidth(const QString& text, int px);
void drawPixelText(QImage& img, int x, int y, const QString& text, int px, QRgb color);

} // namespace studio::media
