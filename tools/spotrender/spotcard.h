#ifndef SPOTRENDER_SPOTCARD_H
#define SPOTRENDER_SPOTCARD_H

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

/**
 * spotcard: the layouts of scripts/spot_card.py on the card kit (DEP-08b, docs/plans/inhouse-deps.md §6.3), and the
 * `PlayPlaySpotRender card` / `scenes` sub-commands with the script's command line and one-line JSON result.
 *  - renderFurniture() = render_furniture: the 1920x1080 RGBA furniture of a single-card spot (kicker chip, headline of
 *    up to 3 lines that shrinks once, up to 4 bullet lines, the station strap) over a left scrim (photo) or a bottom
 *    vignette (no photo).
 *  - renderStory() / renderIntro() / renderOutro() = render_story / render_intro / render_outro: the Headline News scene
 *    furniture, including the boxed story picture (_story_box), the market rows and sparklines (_draw_rows,
 *    _draw_spark) and the detail line.
 *  - photoBed() = render_photo_bed: the 2880x1620 still bed ffmpeg moves: "cover" (landscape fills the frame), "inset"
 *    (square / portrait on the right over a blurred copy of itself), "backdrop" (intro / outro: dimmed, blurred) and "box"
 *    (the story layout whose picture sits whole in the furniture: the bed is its blurred copy). A picture that cannot be
 *    read, is truncated or is under 80 px a side throws, and runScenes() then uses the gradient bed, as the script does.
 *  - runCard() / runScenes(): spot_card.py's main() for `--spec --out [--bed]` and `--scenes`; the result objects carry
 *    exactly the script's keys and values ({ok, furniture, bed, headlineLines, lines} / {ok, scenes: [{kind, furniture,
 *    bed, photo, layout, photoSize}]}); paths are absolute and native, as Path.resolve() prints them.
 * What differs: glyphs are drawn by Qt (DirectWrite) from the same font files, so text is the same size and place within
 * a pixel or two but not the same pixels; Pillow's aliased polygons / lines (sparklines, rounded panels) are drawn by
 * QPainter. Animated scenes (`anim`, weather_cards.py) are DEP-09: runScenes() refuses the whole spec with `unsupported`
 * (exit 64) before writing anything, so the Supervisor runs the script.
 * Needs a QGuiApplication (fonts). Every function is reentrant; nothing is shared between calls but the font cache.
 */
namespace spotcard {

constexpr int kBedW = 2880, kBedH = 1620;      // BED_W x BED_H: 1.5x the frame, so zoompan only ever scales down

/// What a render drew, for the tests: every text call the script makes with a visible fill (its draw.text calls, a
/// _spaced() string as one item) and every element's painted extent, its blur / shadow reach included.
struct TextItem {
    QString text;
    QPoint at;                                  // the "la" anchor, as passed to draw.text
    int size = 0;                               // font pixel size
    int width = 0, height = 0;                  // text_w() and ascent + descent
    QRect box() const { return QRect(at, QSize(qMax(1, width), qMax(1, height))); }
};
struct Trace {
    QList<TextItem> texts;
    QList<QPair<QString, QRect>> boxes;         // element name ("kicker", "strap", "scrim" ...) -> painted extent
    void addBox(const QString& name, const QRect& r) { if (!r.isEmpty()) boxes.append({name, r}); }
    QRect box(const QString& name) const;       // the union of every box of that name
};

/// _setup(): frame size (width / height, 1920 x 1080 by default), scale H / 1080, accent / ink, and the font lists.
struct Setup {
    int W = 1920, H = 1080;
    double s = 1.0;
    QColor accent, ink;
    QStringList bold, reg;
};
Setup setup(const QJsonObject& spec);           // throws std::runtime_error for a size that is not a positive int

struct FurnitureInfo { QStringList headlineLines; int lines = 0; };
QImage renderFurniture(const QJsonObject& spec, FurnitureInfo* info = nullptr, Trace* t = nullptr);
QImage renderStory(const QJsonObject& spec, const QJsonObject& scene, bool photo, const QString& layout,
                   const QString& photoPath = QString(), Trace* t = nullptr);
QImage renderIntro(const QJsonObject& spec, const QJsonObject& scene, bool photo, Trace* t = nullptr);
QImage renderOutro(const QJsonObject& spec, const QJsonObject& scene, bool photo, Trace* t = nullptr);

/// render_photo_bed() without the write: the RGB bed, its layout and the picture's upright size. Throws
/// std::runtime_error when the picture cannot be used.
struct PhotoBed { QImage image; QString layout; QSize photoSize; };
PhotoBed photoBed(const QString& photo, const QString& kind, const QString& storyLayout = QStringLiteral("box"));
/// _backdrop(): _cover to 360x202 (centred), Gaussian blur 5, bicubic-ish up to 2880x1620, brightness `dim`.
QImage backdrop(const QImage& rgb, double dim);

/// Image.save(path, "PNG", compress_level=3): PNG bytes whatever the extension (the script forces the format),
/// atomically, creating the folder first.
bool savePng(const QImage& img, const QString& path, QString* error = nullptr);
/// str(Path(p).resolve()): absolute, native separators, the on-disk spelling for a file that exists.
QString resolvedPath(const QString& p);

/// spot_card.py --spec <spec> --out <out> [--bed <bed>]: the result object (ok or {ok:false, error}).
QJsonObject runCard(const QString& specPath, const QString& out, const QString& bed, Trace* t = nullptr);
/// spot_card.py --scenes <file>: the result object; {ok:false, unsupported:true} for an animated scene (nothing written).
QJsonObject runScenes(const QString& scenesPath, QList<Trace>* traces = nullptr);

/// `card`: the script's own argparse (--spec --out --bed --scenes; "--opt=value"; unique prefixes as argparse allows).
int cardCli(const QStringList& args);
/// `scenes <file>` or `scenes --scenes <file>`.
int scenesCli(const QStringList& args);

}   // namespace spotcard

#endif // SPOTRENDER_SPOTCARD_H
