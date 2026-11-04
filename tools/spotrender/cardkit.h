#ifndef SPOTRENDER_CARDKIT_H
#define SPOTRENDER_CARDKIT_H

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QJsonObject>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <functional>

class QPainter;

/**
 * cardkit: the drawing primitives of scripts/spot_card.py (weather_cards.py borrows them too) for `PlayPlaySpotRender
 * card / scenes` (DEP-08a; the layouts built on them come with DEP-08b, docs/plans/inhouse-deps.md §6.3). Each function
 * names the Python helper it ports.
 *  - Exact: clean(), hexRgb(), the wrap() / ellipsize() algorithms (given the same text widths), the scrim / vignette /
 *    wash alpha ramps, gradientBed()'s float32 arithmetic (within one code value where numpy's float32 pow rounds
 *    differently), and Pillow's Gaussian blur (blur.h) under every drop shadow.
 *  - The same look: glyphs. Pillow rasterises with FreeType; here Qt's text engine (DirectWrite on Windows) draws the same
 *    font files, unhinted and grey-antialiased. Widths agree to a few pixels, so a line break can land differently at the
 *    margin. DEP-08b checks the layout boxes against the script within ±8 px. A Face's width() follows Pillow's textbbox:
 *    the pen line from 0 to the advance, plus any ink that overhangs it.
 * Coordinates are Pillow's: a text position is the top-left of the ascender line (anchor "la"), and where Pillow takes
 * rectangle corners they are inclusive. Canvases are QImage::Format_ARGB32_Premultiplied (newCanvas()); every layer is
 * composited source-over, as Image.alpha_composite does. Fonts need a QGuiApplication.
 */
namespace cardkit {

// ---------------------------------------------------------------------------------------------- text hygiene, colours
/// clean(): smart quotes -> plain, en dash -> "-", em dash -> " - ", ellipsis -> "...", middle dot / bullet -> "-",
/// NBSP / tab -> space; then every code point below 32 is dropped (a newline joins its words, as in the script) and
/// whitespace runs (Python's str.split() set) become one space, trimmed.
QString clean(const QString& s);
/// hex_rgb(): strip, drop leading '#', "abc" -> "aabbcc", then int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16) with
/// Python's int() syntax. Anything that does not parse (or a negative part) gives `fallback`.
QColor hexRgb(const QString& s, const QColor& fallback = QColor(216, 0, 24));
/// Python's str.isspace() for one UTF-16 unit (all of Python's whitespace code points are in the BMP).
bool isPySpace(QChar c);

// ---------------------------------------------------------------------------------------------- fonts
/// One font file at one pixel size: what ImageFont.truetype(path, size) is to the script.
class Face {
public:
    Face();                                                  // the fallback family at 16 px
    Face(const QFont& font, const QString& path);
    const QFont& font() const { return m_font; }
    QString path() const { return m_path; }                  // the file it came from; "" for the fallback family
    int pixelSize() const { return m_font.pixelSize(); }
    /// text_w(): draw.textbbox((0, 0), s)[2] - [0]: max(advance, ink right) - min(0, ink left), whole pixels.
    int width(const QString& s) const;
    /// font.getlength(s): the advance in pixels (what _spaced() steps by; a space has one, but no ink).
    double length(const QString& s) const;
    /// font.getmetrics(): FreeType's size metrics from the hhea table (not the OS/2 win metrics DirectWrite reports;
    /// Calibri differs by 7 px at 32 px), scaled in 26.6 and rounded up; DirectWrite's when the face has no hhea.
    int ascent() const { return m_ascent; }
    int descent() const { return m_descent; }
    int height() const { return ascent() + descent(); }      // _metrics_h()
private:
    QFont m_font;
    QString m_path;
    QFontMetricsF m_fm;
    int m_ascent = 0, m_descent = 0;
    void computeMetrics();
};

QStringList boldCandidates();                                // FONT_CANDIDATES_BOLD
QStringList regularCandidates();                             // FONT_CANDIDATES_REG
/// _setup()'s lists: spec.fonts.bold / spec.fonts.regular first (when set), then the candidates.
QStringList fontPaths(const QJsonObject& spec, bool bold);
/// load_font(paths, size): the first file that loads, at `pixelSize` px (Pillow's size is the em in pixels, as
/// QFont::setPixelSize). A bare file name is looked up in %WINDIR%\Fonts too, as ImageFont.truetype does. When none
/// loads: the Segoe UI family (bold when `boldFallback`), where Pillow falls back to its tiny bitmap font.
Face loadFont(const QStringList& paths, int pixelSize, bool boldFallback = false);
/// The family / style a font file registers as (cached; thread-safe). False when Qt cannot load the file.
bool fontFileFace(const QString& path, QString* family, QString* styleName = nullptr);

// ---------------------------------------------------------------------------------------------- wrap / ellipsize
using Measure = std::function<int(const QString&)>;
/// wrap(): greedy word wrap of clean(s) into lines no wider than maxW (one word alone may be wider). Past maxLines
/// the last kept line loses words, then characters, until it fits with "..." appended. maxLines < 1 gives no lines.
QStringList wrap(const QString& s, const Measure& width, int maxW, int maxLines);
QStringList wrap(const QString& s, const Face& f, int maxW, int maxLines);
/// ellipsize(): clean(s) as is when it fits, else words then characters off the end until it fits with "...".
QString ellipsize(const QString& s, const Measure& width, int maxW);
QString ellipsize(const QString& s, const Face& f, int maxW);

// ---------------------------------------------------------------------------------------------- drawing
/// A transparent Format_ARGB32_Premultiplied image (Image.new("RGBA", (w, h), (0, 0, 0, 0))).
QImage newCanvas(int w, int h);
/// A painter set up the way every card is drawn (antialiased text and shapes, source-over).
void preparePainter(QPainter& p);
/// draw.text(xy, s, font=f, fill=fill): xy is the top-left of the ascender line.
void drawText(QPainter& p, const QPointF& xy, const QString& s, const Face& f, const QColor& fill);
void drawText(QImage& img, const QPointF& xy, const QString& s, const Face& f, const QColor& fill);
/// shadow_text(): the text in black at `alpha`, moved by `off`, Gaussian-blurred by `blur` (Pillow's filter, on a crop
/// just big enough to give the full-frame result), composited under the text drawn in `fill`.
void shadowText(QImage& img, const QPointF& xy, const QString& s, const Face& f, const QColor& fill,
                int blur = 6, const QPoint& off = QPoint(0, 3), int alpha = 170);
/// The rectangle draw.rectangle([x0, y0, x1, y1]) covers: corners inclusive, float corners truncated as Pillow does.
QRect pilRect(double x0, double y0, double x1, double y1);
/// Source-over fill of a Pillow rectangle (a layer drawn with it, then alpha-composited).
void fillRect(QImage& img, const QRect& r, const QColor& c);
/// draw.rectangle(xy, fill=c) straight onto the RGBA card, not through a layer: ImageDraw does not blend on an RGBA
/// image, it writes the ink, so a translucent fill replaces what was under it (the box's pixels become exactly c, as
/// _story_box's frame and _draw_rows' rules do). Opaque fills look the same either way.
void setRect(QImage& img, const QRect& r, const QColor& c);
/// draw.rectangle(xy, outline=c, width=w): a ring w pixels wide (at least 1) just inside r. `replace` = drawn straight
/// onto the card (setRect()), else composited as from a layer (fillRect()).
void ring(QImage& img, const QRect& r, const QColor& c, int w, bool replace = false);

struct Chip { int right = 0; int height = 0; };
/// _chip(): a filled label from (x, y) to (x + text width + 2 * int(20 s), y + height) inclusive, height = the face's
/// height + 2 * int(8 s); an optional outline int(2 s) (at least 1) wide inside the box; the text at
/// (x + int(20 s), y + int(8 s)). Returns the right edge and the height.
Chip chip(QImage& img, int x, int y, const QString& text, const Face& f, const QColor& bg, const QColor& fg, double s,
          const QColor& outline = QColor());
/// _spaced(): letter-spaced text, one code point at a time, each advanced by its own advance + `spacing`. Returns the
/// width drawn, int() like the script.
int spaced(QImage& img, const QPointF& xy, const QString& text, const Face& f, const QColor& fill, int spacing);

// ---------------------------------------------------------------------------------------------- readability layers
/// _scrim() (and render_furniture's photo scrim, span 0.70 / peak 225): ink columns from the left edge, column x of
/// n = int(W * span) at alpha int(peak * (1 - x / n) ** 1.35).
void scrim(QImage& img, const QColor& ink, double span, int peak);
/// render_furniture's vignette when there is no photo: black rows up from the bottom, row H-1-yy for
/// yy < int(H * frac) at alpha int(peak * (1 - yy / (H * frac))).
void bottomVignette(QImage& img, double frac = 0.25, int peak = 90);
/// _wash(): the whole frame in ink at `alpha`, except the bottom int(H * 0.3) rows, which are black at
/// min(255, alpha + int(80 * (1 - yy / (H * 0.3)))).
void wash(QImage& img, const QColor& ink, int alpha);

// ---------------------------------------------------------------------------------------------- beds
/// render_bed(): the static station bed ffmpeg zoompans when there is no photograph. A vertical gradient from
/// ink + (22, 26, 34) to ink - (6, 8, 10), an accent glow top right (centre 0.86 w / 0.12 h, radius 0.55 w,
/// (1 - d)^2.2 * 0.42), a cool (40, 90, 140) glow bottom left (0.08 w / 0.95 h, radius 0.5 w, (1 - d)^2.5 * 0.22) and a
/// +10 scanline on every 6th row. numpy's float32 order of operations; RGB888.
QImage gradientBed(int w, int h, const QColor& accent, const QColor& ink);

}   // namespace cardkit

#endif // SPOTRENDER_CARDKIT_H
