// test_inhouse_card (DEP-08a): the card kit under PlayPlaySpotRender `card` / `scenes` (docs/plans/inhouse-deps.md §6.3;
// DEP-08b adds the spot_card layouts and their tests to this file).
//  - cardkit: clean() / hex_rgb() / wrap() / ellipsize() equal scripts/spot_card.py (fixed cases, and the script's own
//    functions on random input); fonts resolve like ImageFont.truetype and measure within a few pixels of Pillow; wrapped
//    and ellipsized lines stay inside their column when drawn; the scrim / vignette / wash ramps and the gradient bed
//    equal the script's pixels; shadowText()'s cropped blur equals a full-frame blur; setRect() replaces like ImageDraw.
//  - blur: bit-identical to Pillow's GaussianBlur / BoxBlur; mirror-symmetric; alpha stays inside the input's range;
//    a block spreads with the Gaussian's variance; a crop with spread() + 1 margin blurs like the whole frame.
//  - imageio: EXIF orientations 1..8 come out upright through Qt and through libav (ffmpeg's display-matrix rule);
//    WebP decodes through libav in a child process whose Qt has only qjpeg (as the package stages it); truncated,
//    oversized and URL inputs are refused; resize / cover / brightness / save behave like the script's Pillow calls.
//  - spotcard (DEP-08b), on the fixtures in tests/inhouse/data/cards (made by make_fixtures.py): `card` and `scenes`
//    through the exe and in process give the script's JSON keys, 1920x1080 RGBA furniture and 2880x1620 JPEG photo beds
//    (WxH PNG gradient beds without a usable picture); every text inside title-safe (the strap's inside action-safe);
//    nothing drawn outside the declared chrome; headlines of at most 3 lines with the ellipsis; cover / inset / backdrop
//    / box beds hold the picture where the layout says; an animated scene exits 64 having written nothing; the argparse
//    forms and error strings. layoutsVsScript runs spot_card.py itself: the same JSON, every text within ±8 px.
// Python 3 with Pillow + numpy (PLAYPLAY_TEST_PYTHON overrides the search) enables the *VsScript / *VsPillow tests.
#include "blur.h"
#include "cardkit.h"
#include "imageio.h"
#include "ppcli.h"
#include "spotcard.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/display.h>
}

namespace {

const char* const kNoPy = "no Python 3 with Pillow + numpy here (set PLAYPLAY_TEST_PYTHON)";

// ---------------------------------------------------------------------------------------------- pictures
const QColor kRed(220, 30, 30), kGreen(30, 200, 60), kBlue(30, 60, 220), kYellow(230, 210, 40);
// EXIF orientation o (index o - 1): the colours at top-left, top-right, bottom-left, bottom-right once quadrants()
// (stored R G / B Y) is shown upright, per the EXIF spec as ImageOps.exif_transpose applies it.
const char* const kUpright[8] = {"RGBY", "GRYB", "YBGR", "BYRG", "RBGY", "BRYG", "YGBR", "GYRB"};

QImage quadrants(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(0, 0, w / 2, h / 2, kRed);
    p.fillRect(w / 2, 0, w - w / 2, h / 2, kGreen);
    p.fillRect(0, h / 2, w / 2, h - h / 2, kBlue);
    p.fillRect(w / 2, h / 2, w - w / 2, h - h / 2, kYellow);
    return img;
}

QString quadrantCode(const QImage& img) {
    const QImage c = img.convertToFormat(QImage::Format_RGB32);
    const std::pair<char, QColor> cs[] = {{'R', kRed}, {'G', kGreen}, {'B', kBlue}, {'Y', kYellow}};
    const QPointF at[] = {{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};
    QString code;
    for (const QPointF& f : at) {
        const QRgb p = c.pixel(int(f.x() * c.width()), int(f.y() * c.height()));
        char best = '?';
        int bestD = 120;
        for (const auto& [ch, col] : cs) {
            const int d = std::abs(qRed(p) - col.red()) + std::abs(qGreen(p) - col.green()) + std::abs(qBlue(p) - col.blue());
            if (d < bestD) { bestD = d; best = ch; }
        }
        code += QLatin1Char(best);
    }
    return code;
}

// Smooth colour fields plus a dark disc and a bright box: something a photo bed would hold.
QImage photoLike(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto* p = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x)
            p[x] = qRgb(int(128 + 100 * std::sin(x * 0.05) * std::cos(y * 0.03)), int(128 + 90 * std::sin((x + y) * 0.02)),
                        int(128 + 110 * std::cos(x * 0.013 - y * 0.04)));
    }
    QPainter pt(&img);
    pt.setRenderHint(QPainter::Antialiasing);
    pt.setPen(Qt::NoPen);
    pt.setBrush(QColor(20, 24, 30));
    pt.drawEllipse(QPointF(w * 0.3, h * 0.45), w * 0.12, h * 0.2);
    pt.fillRect(QRectF(w * 0.6, h * 0.2, w * 0.25, h * 0.3), QColor(245, 240, 230));
    return img;
}

QByteArray encode(const QImage& img, const char* fmt, int quality) {
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, fmt);
    if (quality >= 0) w.setQuality(quality);
    return w.write(img) ? b : QByteArray();
}

QByteArray readAll(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool writeBytes(const QString& path, const QByteArray& b) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(b) == b.size(); }
bool writeJson(const QString& path, const QJsonValue& v) {
    return writeBytes(path, v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact) : QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
}
QJsonDocument readJson(const QString& path) { return QJsonDocument::fromJson(readAll(path)); }
QStringList toStrings(const QJsonValue& v) { QStringList l; for (const QJsonValue& x : v.toArray()) l << x.toString(); return l; }

// IFD0 with one Orientation entry (SHORT, or LONG), no next IFD.
QByteArray tiffOrientation(quint16 o, bool bigEndian = false, bool asLong = false) {
    QByteArray t = bigEndian ? QByteArray("MM\0*", 4) : QByteArray("II*\0", 4);
    auto u16 = [&](quint16 v) { char b[2]; if (bigEndian) qToBigEndian(v, b); else qToLittleEndian(v, b); t.append(b, 2); };
    auto u32 = [&](quint32 v) { char b[4]; if (bigEndian) qToBigEndian(v, b); else qToLittleEndian(v, b); t.append(b, 4); };
    u32(8); u16(1); u16(0x0112); u16(asLong ? 4 : 3); u32(1);
    if (asLong) u32(o); else { u16(o); u16(0); }
    u32(0);
    return t;
}
// An APP1 segment right after SOI, the way a phone writes EXIF.
QByteArray withApp1(const QByteArray& jpeg, const QByteArray& payload) {
    const int len = int(payload.size()) + 2;
    return jpeg.left(2) + QByteArray("\xff\xe1", 2) + char(len >> 8) + char(len & 0xff) + payload + jpeg.mid(2);
}
QByteArray withExif(const QByteArray& jpeg, quint16 o, bool bigEndian = false, bool asLong = false) {
    return withApp1(jpeg, QByteArray("Exif\0\0", 6) + tiffOrientation(o, bigEndian, asLong));
}

quint32 crc32(const QByteArray& d) {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) { quint32 c = n; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[n] = c; }
        return t;
    }();
    quint32 c = 0xffffffffu;
    for (char ch : d) c = table[(c ^ uchar(ch)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
QByteArray pngChunk(const QByteArray& type, const QByteArray& data) {
    char b[4];
    QByteArray c;
    qToBigEndian(quint32(data.size()), b); c.append(b, 4);
    c += type + data;
    qToBigEndian(crc32(type + data), b); c.append(b, 4);
    return c;
}
QByteArray pngWithExif(const QByteArray& png, quint16 o) { return png.left(33) + pngChunk("eXIf", tiffOrientation(o)) + png.mid(33); }   // after IHDR

// A simple (VP8L / VP8) WebP turned into the extended layout: VP8X with the EXIF flag, the image chunk, an EXIF chunk.
QByteArray webpWithExif(const QByteArray& simple, int w, int h, quint16 o) {
    auto le = [](quint32 v, int bytes) { QByteArray b; for (int i = 0; i < bytes; ++i) b += char((v >> (8 * i)) & 0xff); return b; };
    const QByteArray tiff = tiffOrientation(o);
    QByteArray body = QByteArray("WEBP") + "VP8X" + le(10, 4) + char(0x08) + QByteArray(3, '\0') + le(quint32(w - 1), 3) + le(quint32(h - 1), 3);
    body += simple.mid(12);
    body += QByteArray("EXIF") + le(quint32(tiff.size()), 4) + tiff + (tiff.size() & 1 ? QByteArray(1, '\0') : QByteArray());
    return QByteArray("RIFF") + le(quint32(body.size()), 4) + body;
}

QByteArray rawBytes(const QImage& img, int bpp) {
    QByteArray b;
    b.reserve(qsizetype(img.width()) * img.height() * bpp);
    for (int y = 0; y < img.height(); ++y) b.append(reinterpret_cast<const char*>(img.constScanLine(y)), qsizetype(img.width()) * bpp);
    return b;
}
QImage fromRaw(const QByteArray& b, int w, int h, QImage::Format f, int bpp) {
    QImage img(w, h, f);
    for (int y = 0; y < h && qsizetype(y + 1) * w * bpp <= b.size(); ++y) std::memcpy(img.scanLine(y), b.constData() + qsizetype(y) * w * bpp, size_t(w) * size_t(bpp));
    return img;
}
QImage randomImage(int w, int h, QImage::Format f, int bpp, QRandomGenerator& rng) {
    QImage img(w, h, f);
    for (int y = 0; y < h; ++y) { uchar* p = img.scanLine(y); for (int i = 0; i < w * bpp; ++i) p[i] = uchar(rng.bounded(256)); }
    return img;
}
QImage mirror32(const QImage& in, bool horizontal, bool vertical) {
    QImage out(in.size(), in.format());
    for (int y = 0; y < in.height(); ++y) {
        const auto* s = reinterpret_cast<const quint32*>(in.constScanLine(vertical ? in.height() - 1 - y : y));
        auto* d = reinterpret_cast<quint32*>(out.scanLine(y));
        for (int x = 0; x < in.width(); ++x) d[x] = s[horizontal ? in.width() - 1 - x : x];
    }
    return out;
}
QRgb px(const QImage& img, int x, int y) { return reinterpret_cast<const QRgb*>(img.constScanLine(y))[x]; }

// The bounding box of every pixel at or above minAlpha (an ARGB32 / ARGB32_Premultiplied image).
QRect inkBox(const QImage& img, int minAlpha = 1) {
    int l = INT_MAX, t = INT_MAX, r = -1, b = -1;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(p[x]) >= minAlpha) { l = std::min(l, x); r = std::max(r, x); t = std::min(t, y); b = std::max(b, y); }
    }
    return r < 0 ? QRect() : QRect(QPoint(l, t), QPoint(r, b));
}

double psnrRgb(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull()) return 0.0;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    double se = 0;
    for (int r = 0; r < x.height(); ++r) {
        const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(r));
        const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(r));
        for (int c = 0; c < x.width(); ++c) {
            const double dr = qRed(p[c]) - qRed(q[c]), dg = qGreen(p[c]) - qGreen(q[c]), db = qBlue(p[c]) - qBlue(q[c]);
            se += dr * dr + dg * dg + db * db;
        }
    }
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / (3.0 * x.width() * x.height())));
}

// Byte-wise difference of two equally sized buffers: how many bytes differ and by how much at most.
struct Diff { qint64 n = 0; int max = 0; qsizetype first = -1; };
Diff diffBytes(const QByteArray& a, const QByteArray& b) {
    Diff d;
    if (a.size() != b.size()) { d.n = -1; return d; }
    for (qsizetype i = 0; i < a.size(); ++i) {
        const int v = std::abs(int(uchar(a[i])) - int(uchar(b[i])));
        if (v) { if (d.first < 0) d.first = i; ++d.n; d.max = std::max(d.max, v); }
    }
    return d;
}
// Premultiplied pixels against straight ones (Pillow's): alpha within aTol, colour within cTol after premultiplying.
Diff diffPremultiplied(const QImage& oursPm, const QImage& theirsStraight, int* alphaMax = nullptr) {
    Diff d;
    const QImage t = theirsStraight.convertToFormat(QImage::Format_ARGB32);
    int am = 0;
    for (int y = 0; y < oursPm.height(); ++y)
        for (int x = 0; x < oursPm.width(); ++x) {
            const QRgb o = px(oursPm, x, y), s = qPremultiply(px(t, x, y));
            am = std::max(am, std::abs(qAlpha(o) - qAlpha(s)));
            const int v = std::max({std::abs(qRed(o) - qRed(s)), std::abs(qGreen(o) - qGreen(s)), std::abs(qBlue(o) - qBlue(s)), std::abs(qAlpha(o) - qAlpha(s))});
            if (v) { if (d.first < 0) d.first = qsizetype(y) * oursPm.width() + x; ++d.n; d.max = std::max(d.max, v); }
        }
    if (alphaMax) *alphaMax = am;
    return d;
}

QString findPython() {
    QStringList c;
    if (qEnvironmentVariableIsSet("PLAYPLAY_TEST_PYTHON")) c << qEnvironmentVariable("PLAYPLAY_TEST_PYTHON");
    c << "C:/Program Files/Python311/python.exe" << "C:/Program Files/Python312/python.exe" << "C:/Program Files/Python313/python.exe";   // spotgenerator.cpp's list
    const QString onPath = QStandardPaths::findExecutable("python");
    if (!onPath.isEmpty()) c << onPath;
    for (const QString& p : c) {
        if (!QFileInfo::exists(p)) continue;
        QProcess q;
        q.start(p, {"-I", "-c", "import PIL, numpy"});
        if (q.waitForFinished(30000) && q.exitStatus() == QProcess::NormalExit && q.exitCode() == 0) return p;
    }
    return {};
}
QString scriptsDir() { return QDir(QStringLiteral(PP_SOURCE_DIR)).filePath("scripts"); }
QString windowsFonts() { return QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath("Fonts"); }

// ---------------------------------------------------------------------------------------------- Python snippets
// Each one runs as `python -I -c <code> args...` and calls the script's own functions (spot_card.py) or Pillow.
const char* const kPyClean = R"PY(
import json, sys
sys.path.insert(0, sys.argv[1])
import spot_card
d = json.load(open(sys.argv[2], encoding="utf-8"))
out = {"clean": [spot_card.clean(s) for s in d["clean"]],
       "hex": [[max(0, min(255, v)) for v in spot_card.hex_rgb(s, (216, 0, 24))] for s in d["hex"]]}
json.dump(out, open(sys.argv[3], "w", encoding="utf-8"))
)PY";

const char* const kPyWrap = R"PY(
import json, sys
sys.path.insert(0, sys.argv[1])
import spot_card
class Draw:
    def textbbox(self, xy, s, font=None):
        return (0, 0, sum(5 + (ord(c) * 7) % 13 for c in s), 10)
dr = Draw()
out = [{"wrap": spot_card.wrap(dr, c["s"], None, c["w"], c["n"]), "ellipsize": spot_card.ellipsize(dr, c["s"], None, c["w"])}
       for c in json.load(open(sys.argv[2], encoding="utf-8"))]
json.dump(out, open(sys.argv[3], "w", encoding="utf-8"))
)PY";

const char* const kPyFonts = R"PY(
import json, sys
from PIL import Image, ImageDraw, ImageFont
dr = ImageDraw.Draw(Image.new("RGBA", (8, 8)))
out = []
for c in json.load(open(sys.argv[1], encoding="utf-8")):
    f = ImageFont.truetype(c["file"], c["size"])
    asc, desc = f.getmetrics()
    w = []
    for s in c["texts"]:
        b = dr.textbbox((0, 0), s, font=f)
        w.append(b[2] - b[0])
    out.append({"ascent": asc, "descent": desc, "width": w, "length": [f.getlength(s) for s in c["texts"]]})
json.dump(out, open(sys.argv[2], "w", encoding="utf-8"))
)PY";

const char* const kPyBlur = R"PY(
import json, sys
from PIL import Image, ImageFilter
for c in json.load(open(sys.argv[1], encoding="utf-8")):
    im = Image.frombytes(c["mode"], (c["w"], c["h"]), open(c["in"], "rb").read())
    f = ImageFilter.BoxBlur(c["radius"]) if c["box"] else ImageFilter.GaussianBlur(c["radius"])
    open(c["out"], "wb").write(im.filter(f).tobytes())
)PY";

const char* const kPyLayers = R"PY(
import sys
sys.path.insert(0, sys.argv[1])
import spot_card
from PIL import Image, ImageDraw
W, H, ink, out = 1920, 1080, (11, 18, 32), sys.argv[2]
def blank(): return Image.new("RGBA", (W, H), (0, 0, 0, 0))
im = blank(); spot_card._scrim(im, W, H, ink, 0.70, 225); im.save(out + "/py_scrim70.png")
im = blank(); spot_card._scrim(im, W, H, ink, 0.62, 205); im.save(out + "/py_scrim62.png")
im = blank(); spot_card._wash(im, W, H, ink, 40); im.save(out + "/py_wash40.png")
im = blank(); spot_card._wash(im, W, H, ink, 150); im.save(out + "/py_wash150.png")
im = blank(); vig = blank(); vd = ImageDraw.Draw(vig)          # render_furniture's no-photo vignette, as written there
for yy in range(int(H * 0.25)):
    a = int(90 * (1.0 - yy / (H * 0.25)))
    vd.line([(0, H - 1 - yy), (W, H - 1 - yy)], fill=(0, 0, 0, a))
im.alpha_composite(vig); im.save(out + "/py_vignette.png")
im = Image.new("RGBA", (W, H), (200, 150, 90, 255)); spot_card._scrim(im, W, H, ink, 0.74, 236); im.save(out + "/py_scrim74_opaque.png")
im = Image.new("RGBA", (W, H), (200, 150, 90, 128)); spot_card._wash(im, W, H, ink, 120); im.save(out + "/py_wash120_half.png")
)PY";

const char* const kPyBed = R"PY(
import json, sys
sys.path.insert(0, sys.argv[1])
import spot_card
for c in json.load(open(sys.argv[2], encoding="utf-8")):
    spot_card.render_bed(c["out"], c["w"], c["h"], tuple(c["accent"]), tuple(c["ink"]))
)PY";

const char* const kPyExif = R"PY(
import json, sys
from PIL import Image, ImageOps
for c in json.load(open(sys.argv[1], encoding="utf-8")):
    ImageOps.exif_transpose(Image.open(c["in"])).convert("RGB").save(c["out"])
)PY";

const char* const kPyResize = R"PY(
import json, sys
sys.path.insert(0, sys.argv[1])
import spot_card
from PIL import Image, ImageEnhance
for c in json.load(open(sys.argv[2], encoding="utf-8")):
    im = Image.open(c["in"]).convert("RGB")
    if c["op"] == "cover":
        r = spot_card._cover(im, c["w"], c["h"], c["ybias"])
    elif c["op"] == "resize":
        r = im.resize((c["w"], c["h"]), Image.LANCZOS)
    else:
        r = ImageEnhance.Brightness(im).enhance(c["f"])
    r.save(c["out"])
)PY";

// ---------------------------------------------------------------------------------------------- child process
// `--load-child <pluginDir> <outDir> <file>...`: loads each file with imageio::load() under a Qt whose plugin search path
// is only <pluginDir> (the package stages imageformats/qjpeg.dll and nothing else), writes each picture as PNG and prints
// {formats, libraryPaths, results[{file, via, format, width, height, orientation, error, png}]}.
int loadChild() {
    const QStringList a = QCoreApplication::arguments();
    const qsizetype at = a.indexOf(QStringLiteral("--load-child"));
    if (at < 0 || a.size() < at + 3) return 2;
    QCoreApplication::setLibraryPaths({a[at + 1]});
    QJsonArray formats;
    for (const QByteArray& f : QImageReader::supportedImageFormats()) formats.append(QString::fromLatin1(f));
    const QDir out(a[at + 2]);
    QJsonArray results;
    for (qsizetype i = at + 3; i < a.size(); ++i) {
        const imageio::Loaded l = imageio::load(a[i]);
        QJsonObject o{{"file", a[i]}, {"via", l.via}, {"format", l.format}, {"width", l.image.width()}, {"height", l.image.height()},
                      {"orientation", l.orientation}, {"error", l.error}};
        if (l.ok()) {
            const QString png = out.filePath(QStringLiteral("child_%1.png").arg(i - at - 3));
            if (imageio::save(l.image, png)) o.insert("png", png);
        }
        results.append(o);
    }
    const QByteArray line = QJsonDocument(QJsonObject{{"formats", formats}, {"libraryPaths", QJsonArray::fromStringList(QCoreApplication::libraryPaths())},
                                                      {"results", results}}).toJson(QJsonDocument::Compact) + "\n";
    fwrite(line.constData(), 1, size_t(line.size()), stdout);
    fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------------------------- DEP-08b: layouts, CLI
const QString kCards = QStringLiteral(PP_INHOUSE_DATA_DIR "/cards");
const QString kExe = QStringLiteral(PP_SPOTRENDER_EXE);
const char* const kCardSpecs[] = {"card_photo.json", "card_nophoto.json", "card_small.json", "card_fonts.json"};
const char* const kSceneSpecs[] = {"scenes_box.json", "scenes_full.json"};

struct ExeRun { int code = -1; QByteArray out, err; QJsonObject json; int lines = 0; };
ExeRun runExe(const QStringList& args, const QString& cwd = QString(), const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment()) {
    ExeRun r;
    QProcess p;
    p.setProcessEnvironment(env);
    if (!cwd.isEmpty()) p.setWorkingDirectory(cwd);
    p.start(kExe, args);
    if (!p.waitForStarted(20000)) { r.err = "could not start " + kExe.toUtf8(); return r; }
    if (!p.waitForFinished(120000)) { p.kill(); p.waitForFinished(5000); r.err = "timeout"; return r; }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    r.json = ppcli::parseResult(r.out);
    for (const QByteArray& l : r.out.split('\n')) if (!l.trimmed().isEmpty()) ++r.lines;
    return r;
}

// A fixture scenes.json with its pictures pointing into the fixture folder and every output into `dir`.
QString prepareScenes(const QString& name, const QString& dir) {
    QJsonObject spec = readJson(QDir(kCards).filePath(name)).object();
    QJsonArray scenes = spec.value("scenes").toArray();
    for (int i = 0; i < scenes.size(); ++i) {
        QJsonObject sc = scenes[i].toObject();
        if (!sc.value("photo").toString().isEmpty()) sc["photo"] = QDir::toNativeSeparators(QDir(kCards).filePath(sc.value("photo").toString()));
        sc["furniture"] = QDir::toNativeSeparators(QDir(dir).filePath(sc.value("furniture").toString()));
        sc["bedBase"] = QDir::toNativeSeparators(QDir(dir).filePath(sc.value("bedBase").toString()));
        scenes[i] = sc;
    }
    spec["scenes"] = scenes;
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(name);
    return writeJson(path, spec) ? path : QString();
}

QStringList keysOf(const QJsonObject& o) { QStringList k = o.keys(); k.sort(); return k; }

// Title-safe (90 %) and action-safe (93 %) areas of a frame (SMPTE ST 2046-1).
QRect safeArea(QSize f, double pct) {
    const int mx = int(f.width() * (1 - pct) / 2), my = int(f.height() * (1 - pct) / 2);
    return QRect(mx, my, f.width() - 2 * mx, f.height() - 2 * my);
}

// Every text item inside title-safe, the strap's (below the strap's top) inside action-safe; empty = all good.
QString safeAreaViolations(const spotcard::Trace& t, QSize frame) {
    const double s = frame.height() / 1080.0;
    const int strapTop = frame.height() - int(84 * s) - int(26 * s);
    const QRect title = safeArea(frame, 0.90), action = safeArea(frame, 0.93);
    QStringList bad;
    for (const spotcard::TextItem& it : t.texts) {
        const QRect area = it.at.y() >= strapTop ? action : title;
        if (!area.contains(it.box()))
            bad << QStringLiteral("'%1' %2,%3 %4x%5 outside %6,%7 %8x%9").arg(it.text).arg(it.at.x()).arg(it.at.y()).arg(it.width).arg(it.height)
                       .arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height());
    }
    return bad.join(QLatin1String("; "));
}

// Pixels with alpha outside every box the render declared (its chrome, text, shadows and layers).
qint64 inkOutsideChrome(const QImage& furniture, const spotcard::Trace& t, QPoint* first = nullptr) {
    QImage mask(furniture.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    {
        QPainter p(&mask);
        for (const auto& b : t.boxes) p.fillRect(b.second, Qt::white);
    }
    const QImage f = furniture.convertToFormat(QImage::Format_ARGB32);
    qint64 n = 0;
    for (int y = 0; y < f.height(); ++y) {
        const auto* p = reinterpret_cast<const QRgb*>(f.constScanLine(y));
        const uchar* m = mask.constScanLine(y);
        for (int x = 0; x < f.width(); ++x)
            if (!m[x] && qAlpha(p[x])) { if (!n && first) *first = QPoint(x, y); ++n; }
    }
    return n;
}

// The card's furniture composited over mid grey (for a PSNR against the script's).
QImage overGrey(const QImage& rgba) {
    QImage out(rgba.size(), QImage::Format_RGB32);
    out.fill(QColor(128, 128, 128));
    QPainter(&out).drawImage(0, 0, rgba.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    return out;
}

// Runs scripts/spot_card.py's main() per case with draw.text / _spaced recorded (visible fills only, as the native
// trace records them), one list per render_* call: {code, stdout, texts: [[{text, x, y, size}...]...]}.
const char* const kPySpotCard = R"PY(
import contextlib, io, json, sys
sys.path.insert(0, sys.argv[1])
import spot_card
from PIL import ImageDraw
cases, out_path = json.load(open(sys.argv[2], encoding="utf-8")), sys.argv[3]
rec, depth, groups = [], [0], []
_text = ImageDraw.ImageDraw.text
def text(self, xy, s, *a, **k):
    fill = k.get("fill")
    if depth[0] == 0 and s and fill is not None and tuple(fill[:3]) != (0, 0, 0):
        rec.append({"text": s, "x": int(xy[0]), "y": int(xy[1]), "size": int(k["font"].size)})
    return _text(self, xy, s, *a, **k)
ImageDraw.ImageDraw.text = text
_sp = spot_card._spaced
def spaced(img, xy, s, font, fill, spacing):
    if s:
        rec.append({"text": s, "x": int(xy[0]), "y": int(xy[1]), "size": int(font.size)})
    depth[0] += 1
    try:
        return _sp(img, xy, s, font, fill, spacing)
    finally:
        depth[0] -= 1
spot_card._spaced = spaced
def grouped(fn):
    def w(*a, **k):
        rec.clear()
        r = fn(*a, **k)
        groups.append(list(rec))
        return r
    return w
for n in ("render_furniture", "render_story", "render_intro", "render_outro"):
    setattr(spot_card, n, grouped(getattr(spot_card, n)))
res = []
for c in cases:
    groups.clear()
    sys.argv = ["spot_card.py"] + c["args"]
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = spot_card.main()
    res.append({"code": code, "stdout": buf.getvalue(), "texts": list(groups)})
json.dump(res, open(out_path, "w", encoding="utf-8"))
)PY";

// A JSON value with every string's `from` folder replaced by `to` (the two renders write to different folders).
QJsonValue swapDir(const QJsonValue& v, const QString& from, const QString& to) {
    if (v.isString()) { QString s = v.toString(); s.replace(from, to, Qt::CaseInsensitive); return s; }
    if (v.isArray()) { QJsonArray a; for (const QJsonValue& x : v.toArray()) a.append(swapDir(x, from, to)); return a; }
    if (v.isObject()) { QJsonObject o; const QJsonObject in = v.toObject(); for (auto it = in.begin(); it != in.end(); ++it) o.insert(it.key(), swapDir(it.value(), from, to)); return o; }
    return v;
}

}   // namespace

class TestInhouseCard : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_py;
    QString tmp(const QString& name) const { return QDir(m_tmp.path()).filePath(name); }

    struct PyRun { bool ok = false; QByteArray out, err; qint64 ms = 0; };
    PyRun runPy(const char* code, const QStringList& args, int timeoutMs = 100000) const {
        PyRun r;
        QProcess p;
        QElapsedTimer t;
        t.start();
        p.start(m_py, QStringList{"-I", "-c", QString::fromUtf8(code)} + args);
        if (!p.waitForStarted(20000)) { r.err = "cannot start " + m_py.toUtf8(); return r; }
        if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(5000); r.err = "python timed out"; return r; }
        r.ms = t.elapsed();
        r.out = p.readAllStandardOutput();
        r.err = p.readAllStandardError();
        r.ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
        return r;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_py = findPython();
        qInfo("Python with Pillow + numpy: %s", m_py.isEmpty() ? "none (the *VsScript / *VsPillow tests skip)" : qPrintable(m_py));
        qInfo("platform %s; image formats here: %s", qPrintable(QGuiApplication::platformName()), QImageReader::supportedImageFormats().join(' ').constData());
    }

    // ------------------------------------------------------------------------------------------ text hygiene, colours
    void cleanAndHexRgb() {
        using cardkit::clean;
        QCOMPARE(clean(QStringLiteral("  Hello\tWorld  ")), QStringLiteral("Hello World"));
        QCOMPARE(clean(QString::fromUtf16(u"\u2018Quoted\u2019 \u201cdouble\u201d")), QStringLiteral("'Quoted' \"double\""));
        QCOMPARE(clean(QString::fromUtf16(u"en\u2013dash em\u2014dash")), QStringLiteral("en-dash em - dash"));
        QCOMPARE(clean(QString::fromUtf16(u"wait\u2026 a\u00b7b\u2022c non\u00a0breaking")), QStringLiteral("wait... a-b-c non breaking"));
        QCOMPARE(clean(QStringLiteral("line\nbreak\r\x0b x")), QStringLiteral("linebreak x"));       // control characters go first
        QCOMPARE(clean(QString::fromUtf16(u"x\u2003y\u3000z") + QChar(0x85) + "w"), QStringLiteral("x y z w"));   // str.split()'s whitespace
        QCOMPARE(clean(QString(QChar(0x1c)) + QChar(0x1d) + QString::fromUtf16(u"A \U0001F600 ok")), QString::fromUtf16(u"A \U0001F600 ok"));
        QCOMPARE(clean(QString()), QString());

        auto rgb = [](const QColor& c) { return QList<int>{c.red(), c.green(), c.blue()}; };
        const QColor fb(1, 2, 3);
        QCOMPARE(rgb(cardkit::hexRgb("#d80018")), (QList<int>{216, 0, 24}));
        QCOMPARE(rgb(cardkit::hexRgb("abc")), (QList<int>{170, 187, 204}));
        QCOMPARE(rgb(cardkit::hexRgb(" #0B1220 ")), (QList<int>{11, 18, 32}));
        QCOMPARE(rgb(cardkit::hexRgb("##fff")), (QList<int>{255, 255, 255}));
        QCOMPARE(rgb(cardkit::hexRgb("12345")), (QList<int>{0x12, 0x34, 0x5}));       // s[4:6] is one digit
        QCOMPARE(rgb(cardkit::hexRgb("1 2 3 ")), (QList<int>{1, 2, 3}));              // int("1 ", 16): whitespace allowed
        QCOMPARE(rgb(cardkit::hexRgb("+f+f+f")), (QList<int>{15, 15, 15}));
        QCOMPARE(rgb(cardkit::hexRgb("-f0000")), (QList<int>{0, 0, 0}));             // -15, clipped as Pillow clips ink
        QCOMPARE(rgb(cardkit::hexRgb("1234", fb)), rgb(fb));                         // s[4:6] == "" -> ValueError
        QCOMPARE(rgb(cardkit::hexRgb("", fb)), rgb(fb));
        QCOMPARE(rgb(cardkit::hexRgb("zzzzzz", fb)), rgb(fb));
        QCOMPARE(rgb(cardkit::hexRgb("0x1234", fb)), rgb(fb));                       // "0x" alone has no digits
        QCOMPARE(rgb(cardkit::hexRgb(QString(QChar(0x1c)) + "ff0000", fb)), (QList<int>{255, 0, 0}));   // str.strip() takes \x1c ...
        QCOMPARE(rgb(cardkit::hexRgb("f" + QString(QChar(0x1c)) + "0000", fb)), rgb(fb));             // ... int() does not
        QCOMPARE(rgb(cardkit::hexRgb("f" + QString(QChar(0xa0)) + "0000", fb)), (QList<int>{15, 0, 0}));   // a non-ASCII space it does
        QCOMPARE(rgb(cardkit::hexRgb("")), (QList<int>{216, 0, 24}));                // the script's default accent
    }

    void cleanAndHexRgbVsScript() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        QRandomGenerator rng(20261026);
        const QList<char32_t> textPool = {'a', 'B', 'z', '0', '9', ' ', ' ', ' ', '\t', '\n', '\r', 0x0b, 0x0c, 0x1c, 0x1f, 0x7f, 0x85,
                                          0xa0, 0x2000, 0x2003, 0x2028, 0x3000, 0x2018, 0x2019, 0x201c, 0x201d, 0x2013, 0x2014,
                                          0x2026, 0xb7, 0x2022, '#', '_', '-', '+', '.', 0xe9, 0x0661, 0x1F600, 'f', 'x'};
        const QList<char32_t> hexPool = {'0', '1', '7', '9', 'a', 'c', 'f', 'A', 'D', 'F', '#', '#', ' ', '\t', '+', '-', '_', 'g', 'x',
                                         0x0661, 0xff11, 0xa0, 0x1c, 0x1F600};
        auto randomString = [&](int maxLen, const QList<char32_t>& pool) {
            QList<char32_t> cps;
            const int n = int(rng.bounded(maxLen + 1));
            for (int i = 0; i < n; ++i) cps << pool[int(rng.bounded(int(pool.size())))];
            return QString::fromUcs4(cps.constData(), cps.size());
        };
        QStringList texts, hexes;
        for (int i = 0; i < 400; ++i) texts << randomString(40, textPool);
        for (int i = 0; i < 600; ++i) hexes << randomString(i < 300 ? 7 : 4, hexPool);
        hexes << "#d80018" << "abc" << "  #0b1220\t" << QString::fromUtf16(u"\u0661\u0662\u0663");
        QVERIFY(writeJson(tmp("clean_in.json"), QJsonObject{{"clean", QJsonArray::fromStringList(texts)}, {"hex", QJsonArray::fromStringList(hexes)}}));
        const PyRun r = runPy(kPyClean, {scriptsDir(), tmp("clean_in.json"), tmp("clean_out.json")});
        QVERIFY2(r.ok, r.err.constData());
        const QJsonObject py = readJson(tmp("clean_out.json")).object();
        const QJsonArray pc = py["clean"].toArray(), ph = py["hex"].toArray();
        QCOMPARE(pc.size(), texts.size());
        QCOMPARE(ph.size(), hexes.size());
        int bad = 0;
        for (int i = 0; i < texts.size(); ++i)
            if (cardkit::clean(texts[i]) != pc[i].toString() && ++bad <= 3)
                qWarning("clean #%d: ours '%s' script '%s'", i, qPrintable(cardkit::clean(texts[i])), qPrintable(pc[i].toString()));
        for (int i = 0; i < hexes.size(); ++i) {
            const QColor c = cardkit::hexRgb(hexes[i]);
            const QJsonArray e = ph[i].toArray();
            if ((c.red() != e[0].toInt() || c.green() != e[1].toInt() || c.blue() != e[2].toInt()) && ++bad <= 6)
                qWarning("hex_rgb #%d '%s': ours %d,%d,%d script %d,%d,%d", i, qPrintable(hexes[i]), c.red(), c.green(), c.blue(),
                         e[0].toInt(), e[1].toInt(), e[2].toInt());
        }
        QCOMPARE(bad, 0);
    }

    // ------------------------------------------------------------------------------------------ wrap / ellipsize
    void wrapEllipsizeAlgorithm() {
        const cardkit::Measure m10 = [](const QString& s) { return int(s.toUcs4().size()) * 10; };
        QCOMPARE(cardkit::wrap("aaa bbb ccc ddd", m10, 75, 3), (QStringList{"aaa bbb", "ccc ddd"}));
        QCOMPARE(cardkit::wrap("aaa bbb ccc ddd eee fff ggg", m10, 75, 2), (QStringList{"aaa bbb", "ccc..."}));   // words, then fits
        QCOMPARE(cardkit::wrap("supercalifragilistic tiny", m10, 50, 3), (QStringList{"supercalifragilistic", "tiny"}));   // a long word stands alone
        QCOMPARE(cardkit::wrap("abcdefghij klm nop", m10, 60, 1), (QStringList{"abc..."}));                         // characters off a lone word
        QCOMPARE(cardkit::wrap("abc def", m10, 20, 1), (QStringList{"..."}));                                       // nothing fits beside the dots
        QCOMPARE(cardkit::wrap(QString::fromUtf16(u"  \u201cSmart\u201d\tquotes\u2026  "), m10, 1000, 3), (QStringList{"\"Smart\" quotes..."}));
        QCOMPARE(cardkit::wrap("abc", m10, 100, 0), QStringList());
        QCOMPARE(cardkit::wrap("   ", m10, 100, 3), QStringList());
        QCOMPARE(cardkit::ellipsize("short", m10, 100), QStringLiteral("short"));
        QCOMPARE(cardkit::ellipsize("one two three", m10, 100), QStringLiteral("one two..."));
        QCOMPARE(cardkit::ellipsize("abcdefghijkl", m10, 60), QStringLiteral("abc..."));
        QCOMPARE(cardkit::ellipsize("  ", m10, 0), QString());
        // properties on random text: every line but a one-word line fits; at most maxLines; untruncated = all words
        QRandomGenerator rng(99);
        for (int i = 0; i < 300; ++i) {
            QStringList words;
            for (int k = int(rng.bounded(30)); k > 0; --k) words << QString(1 + int(rng.bounded(12)), QChar('a' + int(rng.bounded(26))));
            const int maxW = 30 + int(rng.bounded(300)), maxLines = 1 + int(rng.bounded(4));
            const QStringList lines = cardkit::wrap(words.join(' '), m10, maxW, maxLines);
            const QStringList all = cardkit::wrap(words.join(' '), m10, maxW, 1000);
            QVERIFY(lines.size() <= maxLines);
            for (const QString& ln : lines) QVERIFY(!ln.contains(' ') || m10(ln) <= maxW);
            if (all.size() <= maxLines) QCOMPARE(lines.join(' '), words.join(' '));
            else QVERIFY(lines.last().endsWith("..."));
        }
    }

    void wrapEllipsizeVsScript() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        // a synthetic width per code point, the same on both sides, so only the algorithms are compared
        const cardkit::Measure synth = [](const QString& s) { int w = 0; for (uint c : s.toUcs4()) w += 5 + int((c * 7u) % 13u); return w; };
        QRandomGenerator rng(4242);
        const QString letters = QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,.'!?");
        QJsonArray cases;
        for (int i = 0; i < 400; ++i) {
            QString s;
            for (int k = int(rng.bounded(26)); k > 0; --k) {
                for (int j = 1 + int(rng.bounded(14)); j > 0; --j) s += letters.at(int(rng.bounded(int(letters.size()))));
                const int sep = int(rng.bounded(10));
                s += sep == 0 ? QStringLiteral("  ") : sep == 1 ? QStringLiteral("\t") : sep == 2 ? QString(QChar(0x2014))
                   : sep == 3 ? QStringLiteral("\n") : QStringLiteral(" ");
            }
            if (rng.bounded(5) == 0) s += QString::fromUtf16(u"\u2026\U0001F600");
            cases.append(QJsonObject{{"s", s}, {"w", 15 + int(rng.bounded(386))}, {"n", 1 + int(rng.bounded(4))}});
        }
        QVERIFY(writeJson(tmp("wrap_in.json"), cases));
        const PyRun r = runPy(kPyWrap, {scriptsDir(), tmp("wrap_in.json"), tmp("wrap_out.json")});
        QVERIFY2(r.ok, r.err.constData());
        const QJsonArray py = readJson(tmp("wrap_out.json")).array();
        QCOMPARE(py.size(), cases.size());
        int bad = 0;
        for (int i = 0; i < cases.size(); ++i) {
            const QJsonObject c = cases[i].toObject(), p = py[i].toObject();
            const QStringList ours = cardkit::wrap(c["s"].toString(), synth, c["w"].toInt(), c["n"].toInt());
            const QString ell = cardkit::ellipsize(c["s"].toString(), synth, c["w"].toInt());
            if ((ours != toStrings(p["wrap"]) || ell != p["ellipsize"].toString()) && ++bad <= 3)
                qWarning("case %d: ours %s | %s; script %s | %s", i, qPrintable(ours.join(" / ")), qPrintable(ell),
                         qPrintable(toStrings(p["wrap"]).join(" / ")), qPrintable(p["ellipsize"].toString()));
        }
        QCOMPARE(bad, 0);
    }

    // ------------------------------------------------------------------------------------------ fonts
    void fontsResolveAndMeasure() {
        if (!QFileInfo::exists(windowsFonts() + "/segoeuib.ttf")) QSKIP("Segoe UI is not installed here");
        const cardkit::Face hb = cardkit::loadFont(cardkit::boldCandidates(), 76);
        QCOMPARE(QFileInfo(hb.path()).fileName().toLower(), QStringLiteral("segoeuib.ttf"));
        QCOMPARE(hb.pixelSize(), 76);
        QVERIFY(hb.font().weight() >= QFont::DemiBold);
        const cardkit::Face hr = cardkit::loadFont(cardkit::regularCandidates(), 76);
        QCOMPARE(QFileInfo(hr.path()).fileName().toLower(), QStringLiteral("segoeui.ttf"));
        QVERIFY(hb.width("WEATHER") > hr.width("WEATHER"));
        // the next candidate when one is missing; a bare file name is looked up in %WINDIR%\Fonts (ImageFont.truetype)
        const cardkit::Face arial = cardkit::loadFont({"C:/no/such/dir/missing.ttf", "arialbd.ttf"}, 40);
        QCOMPARE(QFileInfo(arial.path()).fileName().toLower(), QStringLiteral("arialbd.ttf"));
        QString family, style;
        QVERIFY(cardkit::fontFileFace("arialbd.ttf", &family, &style));
        QCOMPARE(family, QStringLiteral("Arial"));
        QVERIFY(!cardkit::fontFileFace("C:/no/such.ttf", &family));
        // nothing loads: Segoe UI by family name
        const cardkit::Face fb = cardkit::loadFont({"C:/no/such.ttf"}, 40, true);
        QVERIFY(fb.path().isEmpty());
        QCOMPARE(fb.font().weight(), QFont::Bold);
        QCOMPARE(fb.pixelSize(), 40);
        QVERIFY(fb.width("Fallback") > 40);
        // the spec's own file first, then the script's candidates
        const QJsonObject spec{{"fonts", QJsonObject{{"bold", "X:/brand-bold.ttf"}}}};
        QCOMPARE(cardkit::fontPaths(spec, true), QStringList{"X:/brand-bold.ttf"} + cardkit::boldCandidates());
        QCOMPARE(cardkit::fontPaths(spec, false), cardkit::regularCandidates());
        QCOMPARE(cardkit::boldCandidates().first(), QStringLiteral("C:/Windows/Fonts/segoeuib.ttf"));
        // textbbox widths: empty 0; spaces = their advance (no ink); ink beyond the pen line counts
        QCOMPARE(hb.width(QString()), 0);
        QVERIFY(std::abs(hb.width("   ") - hb.length("   ")) <= 1.0 && hb.width("   ") > 0);
        QVERIFY(hb.width("j") > int(hb.length("j")));
        QCOMPARE(hb.height(), hb.ascent() + hb.descent());
        QVERIFY(hb.ascent() > 60 && hb.descent() > 10);
    }

    void fontMetricsVsPillow() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        struct FC { const char* file; int size; };
        const FC fcs[] = {{"segoeuib.ttf", 76}, {"segoeuib.ttf", 64}, {"segoeuib.ttf", 34}, {"segoeui.ttf", 40}, {"segoeui.ttf", 26},
                          {"arialbd.ttf", 46}, {"calibri.ttf", 32}};
        const QStringList texts = {"WEATHER", "Cooler Air Moves In Tonight", "Rain returns Sunday", "j", "   ",
                                   QString::fromUtf16(u"\u00dcn\u00efc\u00f6d\u00e9 fa\u00e7ade - 2026"), "HEADLINE NEWS", "GetStarNews Weather",
                                   "Thursday, October 22 - 6:05 PM", "1 / 5", "Wy AV To"};
        QJsonArray cases;
        QList<cardkit::Face> faces;
        for (const FC& fc : fcs) {
            const QString file = windowsFonts() + "/" + fc.file;
            if (!QFileInfo::exists(file)) continue;
            faces << cardkit::loadFont({file}, fc.size);
            cases.append(QJsonObject{{"file", file}, {"size", fc.size}, {"texts", QJsonArray::fromStringList(texts)}});
        }
        if (cases.isEmpty()) QSKIP("none of the script's fonts is installed here");
        QVERIFY(writeJson(tmp("fonts_in.json"), cases));
        const PyRun r = runPy(kPyFonts, {tmp("fonts_in.json"), tmp("fonts_out.json")});
        QVERIFY2(r.ok, r.err.constData());
        const QJsonArray py = readJson(tmp("fonts_out.json")).array();
        QCOMPARE(py.size(), cases.size());
        int worstW = 0, worstAsc = 0;
        double worstLen = 0;
        for (int i = 0; i < cases.size(); ++i) {
            const QJsonObject p = py[i].toObject();
            const cardkit::Face& f = faces[i];
            const QString what = cases[i].toObject()["file"].toString() + " @" + QString::number(f.pixelSize());
            worstAsc = std::max({worstAsc, std::abs(f.ascent() - p["ascent"].toInt()), std::abs(f.descent() - p["descent"].toInt())});
            QVERIFY2(std::abs(f.ascent() - p["ascent"].toInt()) <= 1, qPrintable(what + " ascent"));
            QVERIFY2(std::abs(f.descent() - p["descent"].toInt()) <= 1, qPrintable(what + " descent"));
            for (int k = 0; k < texts.size(); ++k) {
                const int pw = p["width"].toArray()[k].toInt(), ow = f.width(texts[k]);
                const double pl = p["length"].toArray()[k].toDouble(), ol = f.length(texts[k]);
                worstW = std::max(worstW, std::abs(pw - ow));
                worstLen = std::max(worstLen, std::abs(pl - ol));
                // DirectWrite and FreeType read the same outlines: a couple of pixels per line at most
                QVERIFY2(std::abs(pw - ow) <= 2 + pw / 100, qPrintable(QStringLiteral("%1 '%2': width ours %3, Pillow %4").arg(what, texts[k]).arg(ow).arg(pw)));
                QVERIFY2(std::abs(pl - ol) <= 2 + pl / 100, qPrintable(QStringLiteral("%1 '%2': length ours %3, Pillow %4").arg(what, texts[k]).arg(ol).arg(pl)));
            }
        }
        qInfo("fonts vs Pillow (%d faces x %d strings): worst width %d px, advance %.2f px, ascent/descent %d px",
              int(cases.size()), int(texts.size()), worstW, worstLen, worstAsc);
    }

    void wrapAndEllipsizeBoxes() {
        if (!QFileInfo::exists(windowsFonts() + "/segoeuib.ttf")) QSKIP("Segoe UI is not installed here");
        const cardkit::Face f = cardkit::loadFont(cardkit::boldCandidates(), 76);
        const int W = 1920, safeX = int(W * 0.058), colW = int(W * 0.56);     // render_furniture over a photo
        const QStringList heads = {
            "Cooler Air Moves In Tonight",
            "Martinsville City Council Approves New Budget After Lengthy Debate Over School Funding And Road Repairs Across The County",
            "Short",
            "Supercalifragilisticexpialidociousnessly Long Words Strike Again In Henry County",
            QString::fromUtf16(u"Weather Alert: Severe Thunderstorm Watch Issued For Patrick, Henry And Franklin Counties \u2014 Until 9 PM"),
        };
        for (const QString& h : heads) {
            const QStringList lines = cardkit::wrap(h, f, colW, 3), all = cardkit::wrap(h, f, colW, 1000);
            QVERIFY(!lines.isEmpty() && lines.size() <= 3);
            QCOMPARE(lines.last().endsWith("..."), all.size() > 3);
            QString joined = lines.join(' ');
            if (joined.endsWith("...")) joined.chop(3);
            QVERIFY2(cardkit::clean(h).startsWith(joined), qPrintable(joined));
            for (const QString& ln : lines) {
                const bool oneWord = !ln.contains(' ');
                QImage c = cardkit::newCanvas(W + 600, 200);   // wider, so an over-long single word shows all its ink
                cardkit::drawText(c, QPointF(safeX, 40), ln, f, Qt::white);
                const QRect ink = inkBox(c);
                const QString what = QStringLiteral("'%1': width %2, ink %3..%4 x %5..%6").arg(ln).arg(f.width(ln)).arg(ink.left())
                                         .arg(ink.right()).arg(ink.top()).arg(ink.bottom());
                QVERIFY2(!ink.isNull(), qPrintable(what));
                QVERIFY2(ink.right() <= safeX + f.width(ln), qPrintable(what));          // the measured box holds the ink
                QVERIFY2(ink.left() >= safeX - 8, qPrintable(what));
                QVERIFY2(ink.top() >= 40 - 1 && ink.bottom() <= 40 + f.height(), qPrintable(what));
                if (!oneWord) {
                    QVERIFY2(f.width(ln) <= colW, qPrintable(what));
                    QVERIFY2(ink.right() <= safeX + colW, qPrintable(what));              // inside the text column
                }
            }
        }
        // bullet lines: ellipsized to the column minus the bullet
        const cardkit::Face fr = cardkit::loadFont(cardkit::regularCandidates(), 40);
        const int bulletW = colW - 14 - 20;
        const QString e = cardkit::ellipsize("Friday: Sunny and warmer with a high near 76, light winds from the southwest, clouds increasing late", fr, bulletW);
        QVERIFY(e.endsWith("..."));
        QVERIFY(fr.width(e) <= bulletW);
        QImage c = cardkit::newCanvas(W, 120);
        cardkit::drawText(c, QPointF(safeX + 34, 20), e, fr, Qt::white);
        QVERIFY(inkBox(c).right() <= safeX + 34 + bulletW);
        QCOMPARE(cardkit::ellipsize("Tonight: Low 54", fr, bulletW), QStringLiteral("Tonight: Low 54"));
    }

    // ------------------------------------------------------------------------------------------ blur
    void blurVsPillow() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        QRandomGenerator rng(31337);
        struct Case { QString mode; QImage img; float radius; bool box; };
        QList<Case> cases;
        auto add = [&](const QString& mode, const QImage& img, std::initializer_list<float> radii, bool box = false) {
            for (float r : radii) cases.append({mode, img, r, box});
        };
        QImage text = cardkit::newCanvas(300, 90);   // black shadow text: premultiplied and straight are the same bytes
        cardkit::drawText(text, QPointF(10, 10), "Shadow 123 Wg", cardkit::loadFont(cardkit::boldCandidates(), 48), QColor(0, 0, 0, 170));
        text = text.convertToFormat(QImage::Format_RGBA8888);
        const QImage rgba = randomImage(97, 61, QImage::Format_RGBA8888, 4, rng);
        add("RGBA", rgba, {0.5f, 1, 2, 3, 5, 6, 7, 9, 22, 28});
        add("RGBA", text, {2, 5, 7, 9});
        add("L", randomImage(50, 33, QImage::Format_Grayscale8, 1, rng), {1, 4.5f, 30});
        add("RGB", randomImage(41, 29, QImage::Format_RGB888, 3, rng), {1, 4.5f, 30});
        add("RGBA", randomImage(5, 3, QImage::Format_RGBA8888, 4, rng), {1, 7});         // the radius beyond both ends
        add("RGBA", randomImage(1, 1, QImage::Format_RGBA8888, 4, rng), {3});
        add("L", randomImage(1, 9, QImage::Format_Grayscale8, 1, rng), {2, 12});
        add("RGBA", randomImage(200, 6, QImage::Format_RGBA8888, 4, rng), {5, 20});
        add("RGBA", rgba, {1, 2.5f, 6}, true);                                             // ImageFilter.BoxBlur: one pass
        QJsonArray manifest;
        for (int i = 0; i < cases.size(); ++i) {
            const Case& c = cases[i];
            const int bpp = c.mode == "RGBA" ? 4 : c.mode == "RGB" ? 3 : 1;
            QVERIFY(writeBytes(tmp(QStringLiteral("blur_in_%1.raw").arg(i)), rawBytes(c.img, bpp)));
            manifest.append(QJsonObject{{"mode", c.mode}, {"w", c.img.width()}, {"h", c.img.height()}, {"radius", double(c.radius)},
                                        {"box", c.box}, {"in", tmp(QStringLiteral("blur_in_%1.raw").arg(i))},
                                        {"out", tmp(QStringLiteral("blur_out_%1.raw").arg(i))}});
        }
        QVERIFY(writeJson(tmp("blur.json"), manifest));
        const PyRun r = runPy(kPyBlur, {tmp("blur.json")});
        QVERIFY2(r.ok, r.err.constData());
        int bad = 0;
        for (int i = 0; i < cases.size(); ++i) {
            const Case& c = cases[i];
            const int bpp = c.mode == "RGBA" ? 4 : c.mode == "RGB" ? 3 : 1;
            QImage ours = c.img;
            QVERIFY(c.box ? blur::boxBlur(ours, c.radius, c.radius, 1) : blur::gaussianBlur(ours, c.radius));
            const Diff d = diffBytes(rawBytes(ours, bpp), readAll(tmp(QStringLiteral("blur_out_%1.raw").arg(i))));
            if (d.n != 0) {
                ++bad;
                qWarning("%s %dx%d %s(%g): %lld bytes differ (max %d, first at byte %lld)", qPrintable(c.mode), c.img.width(), c.img.height(),
                         c.box ? "BoxBlur" : "GaussianBlur", double(c.radius), d.n, d.max, qlonglong(d.first));
            }
        }
        QCOMPARE(bad, 0);
        qInfo("blur: %d cases bit-identical to Pillow", int(cases.size()));
    }

    void blurSymmetricKeepsAlphaRange() {
        QRandomGenerator rng(7);
        // a premultiplied picture with alpha 40..200
        QImage img(120, 80, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < img.height(); ++y) {
            auto* p = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x)
                p[x] = qPremultiply(qRgba(int(rng.bounded(256)), int(rng.bounded(256)), int(rng.bounded(256)), 40 + int(rng.bounded(161))));
        }
        for (float r : {1.0f, 3.3f, 7.0f, 22.0f}) {
            const QImage b = blur::gaussianBlurred(img, r);
            QCOMPARE(blur::gaussianBlurred(mirror32(img, true, false), r), mirror32(b, true, false));
            QCOMPARE(blur::gaussianBlurred(mirror32(img, false, true), r), mirror32(b, false, true));
            int amin = 255, amax = 0;
            bool valid = true;
            for (int y = 0; y < b.height(); ++y)
                for (int x = 0; x < b.width(); ++x) {
                    const QRgb p = px(b, x, y);
                    amin = std::min(amin, qAlpha(p)); amax = std::max(amax, qAlpha(p));
                    if (qRed(p) > qAlpha(p) || qGreen(p) > qAlpha(p) || qBlue(p) > qAlpha(p)) valid = false;
                }
            QVERIFY2(amin >= 40 && amax <= 200, qPrintable(QStringLiteral("radius %1: alpha %2..%3").arg(r).arg(amin).arg(amax)));
            QVERIFY2(valid, "premultiplied colour above its alpha");
        }
        // flat stays flat (no drift), opaque stays opaque, clear stays clear
        QImage flat(64, 40, QImage::Format_RGBA8888);
        flat.fill(QColor(37, 90, 200, 255));
        QImage clear = cardkit::newCanvas(64, 40);
        for (float r : {0.5f, 2.0f, 9.0f, 40.0f}) {
            QImage a = flat, c = clear;
            QVERIFY(blur::gaussianBlur(a, r) && blur::gaussianBlur(c, r));
            QCOMPARE(a, flat);
            QCOMPARE(c, clear);
        }
        QImage opaque = photoLike(90, 60);
        QVERIFY(blur::gaussianBlur(opaque, 6));
        for (int y = 0; y < opaque.height(); ++y) for (int x = 0; x < opaque.width(); ++x) QCOMPARE(qAlpha(px(opaque, x, y)), 255);
        // a 9x9 block spreads with the Gaussian's variance: 6.67 (the block) + radius^2, and keeps its mass
        QImage blk(101, 101, QImage::Format_Grayscale8);
        blk.fill(0);
        for (int y = 46; y <= 54; ++y) std::memset(blk.scanLine(y) + 46, 255, 9);
        QVERIFY(blur::gaussianBlur(blk, 4));
        double mass = 0, mean = 0, var = 0;
        std::vector<double> col(101, 0.0);
        for (int y = 0; y < 101; ++y) for (int x = 0; x < 101; ++x) col[size_t(x)] += blk.constScanLine(y)[x];
        for (int x = 0; x < 101; ++x) { mass += col[size_t(x)]; mean += x * col[size_t(x)]; }
        mean /= mass;
        for (int x = 0; x < 101; ++x) var += (x - mean) * (x - mean) * col[size_t(x)];
        var /= mass;
        QVERIFY2(std::abs(mass - 81 * 255) <= 0.02 * 81 * 255, qPrintable(QString::number(mass)));
        QVERIFY2(std::abs(mean - 50) < 0.01, qPrintable(QString::number(mean)));
        QVERIFY2(std::abs(var - (80.0 / 12 + 16)) <= 0.03 * (80.0 / 12 + 16), qPrintable(QString::number(var)));
        for (int y = 0; y < 101; ++y)
            for (int x = 0; x < 101; ++x) {
                QCOMPARE(blk.constScanLine(y)[x], blk.constScanLine(y)[100 - x]);
                QCOMPARE(blk.constScanLine(y)[x], blk.constScanLine(100 - y)[x]);
            }
        // spread(): a crop with spread() + 1 pixels of margin (clamped to the frame) blurs to exactly the frame's pixels
        const float br = blur::gaussianBoxRadius(7, 3);
        const int margin = blur::spread(br, 3) + 1;
        for (const QRect& inkArea : {QRect(80, 60, 41, 31), QRect(2, 100, 19, 41)}) {
            QImage frame = cardkit::newCanvas(200, 150);
            {
                QPainter p(&frame);
                for (int k = 0; k < 12; ++k)
                    p.fillRect(QRect(inkArea.left() + int(rng.bounded(inkArea.width() - 4)), inkArea.top() + int(rng.bounded(inkArea.height() - 4)), 4, 4),
                               QColor(0, 0, 0, 60 + int(rng.bounded(190))));
            }
            const QRect area = inkArea.adjusted(-margin, -margin, margin, margin) & frame.rect();
            QImage crop = frame.copy(area), full = frame;
            blur::boxBlur(crop, br, br, 3);
            blur::boxBlur(full, br, br, 3);
            QCOMPARE(crop, full.copy(area));
            QCOMPARE(inkBox(full).united(area), area);                               // nothing reaches past the margin
        }
        QCOMPARE(blur::gaussianBoxRadius(0, 3), 0.0f);
        QCOMPARE(blur::spread(0, 3), 0);
        QImage rgb16(10, 10, QImage::Format_RGB16);
        rgb16.fill(Qt::red);
        const QImage before = rgb16;
        QVERIFY(!blur::gaussianBlur(rgb16, 3));
        QCOMPARE(rgb16, before);
        QCOMPARE(blur::gaussianBlurred(rgb16, 3).format(), QImage::Format_RGB32);
    }

    void shadowTextEqualsFullFrameBlur() {
        const cardkit::Face hb = cardkit::loadFont(cardkit::boldCandidates(), 76), rg = cardkit::loadFont(cardkit::regularCandidates(), 40);
        struct C { QPointF xy; QString s; const cardkit::Face* f; int blur; QPoint off; int alpha; };
        const C cs[] = {
            {{111, 300}, "Cooler Air Moves In Tonight", &hb, 7, {0, 3}, 170},
            {{1600, 1030}, "Clipped at the edge", &rg, 5, {0, 2}, 140},            // the crop is clamped to the frame
            {{-30, -10}, "Off the corner", &hb, 9, {0, 4}, 170},
            {{200, 500}, "No blur at all", &rg, 0, {0, 2}, 150},
            {{400, 600}, "Invisible shadow", &rg, 5, {0, 2}, 0},
        };
        const QImage bed = photoLike(1920, 1080).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        for (const C& c : cs) {
            for (const QImage& start : {cardkit::newCanvas(1920, 1080), bed}) {
                QImage ours = start, ref = start, layer = cardkit::newCanvas(1920, 1080);
                cardkit::shadowText(ours, c.xy, c.s, *c.f, Qt::white, c.blur, c.off, c.alpha);
                // the script's way: the whole frame as the shadow layer, blurred whole, composited, then the text
                cardkit::drawText(layer, c.xy + QPointF(c.off), c.s, *c.f, QColor(0, 0, 0, c.alpha));
                if (c.blur > 0) QVERIFY(blur::gaussianBlur(layer, float(c.blur)));
                { QPainter p(&ref); p.drawImage(0, 0, layer); }
                cardkit::drawText(ref, c.xy, c.s, *c.f, Qt::white);
                if (ours != ref) QFAIL(qPrintable(QStringLiteral("'%1': %2 bytes differ").arg(c.s).arg(diffBytes(rawBytes(ours, 4), rawBytes(ref, 4)).n)));
            }
        }
    }

    // ------------------------------------------------------------------------------------------ readability layers, rects
    void layerRampsAndRects() {
        const QColor ink(11, 18, 32);
        auto expectPm = [](const QImage& img, int x, int y, QRgb straight) {
            const QRgb got = px(img, x, y), want = qPremultiply(straight);
            return qAlpha(got) == qAlpha(want) && std::abs(qRed(got) - qRed(want)) <= 1 && std::abs(qGreen(got) - qGreen(want)) <= 1
                && std::abs(qBlue(got) - qBlue(want)) <= 1;
        };
        QImage s = cardkit::newCanvas(1920, 1080);
        cardkit::scrim(s, ink, 0.70, 225);
        const int n = int(1920 * 0.70);
        for (int x : {0, 1, 2, 100, 500, 1000, n - 2, n - 1, n, 1500, 1919}) {
            const int a = x < n ? int(225 * std::pow(1.0 - double(x) / n, 1.35)) : 0;
            for (int y : {0, 540, 1079}) QVERIFY2(expectPm(s, x, y, qRgba(11, 18, 32, a)), qPrintable(QStringLiteral("scrim x %1 y %2").arg(x).arg(y)));
        }
        QImage v = cardkit::newCanvas(1920, 1080);
        cardkit::bottomVignette(v);
        for (int yy : {0, 1, 100, 269, 270, 500, 1079}) {
            const int a = yy < 270 ? int(90 * (1.0 - yy / (1080 * 0.25))) : 0;
            for (int x : {0, 960, 1919}) QVERIFY2(expectPm(v, x, 1079 - yy, qRgba(0, 0, 0, a)), qPrintable(QStringLiteral("vignette yy %1").arg(yy)));
        }
        QImage w = cardkit::newCanvas(1920, 1080);
        cardkit::wash(w, ink, 40);
        for (int y : {0, 500, 755}) QVERIFY(expectPm(w, 7, y, qRgba(11, 18, 32, 40)));
        for (int yy : {0, 1, 200, 323}) {
            const int a = std::min(255, 40 + int(80 * (1.0 - yy / (1080 * 0.3))));
            QVERIFY2(expectPm(w, 7, 1079 - yy, qRgba(0, 0, 0, a)), qPrintable(QStringLiteral("wash yy %1").arg(yy)));
        }
        // setRect replaces (ImageDraw on an RGBA image), fillRect composites (a layer)
        QImage red(40, 30, QImage::Format_ARGB32_Premultiplied);
        red.fill(qRgba(255, 0, 0, 255));
        QImage a = red, b = red;
        const QRect box = cardkit::pilRect(5.7, 5.2, 14.9, 9.9);
        QCOMPARE(box, QRect(QPoint(5, 5), QPoint(14, 9)));
        cardkit::setRect(a, box, QColor(255, 255, 255, 90));
        cardkit::fillRect(b, box, QColor(255, 255, 255, 90));
        QVERIFY(expectPm(a, 5, 5, qRgba(255, 255, 255, 90)) && expectPm(a, 14, 9, qRgba(255, 255, 255, 90)));
        QCOMPARE(px(a, 4, 5), qRgba(255, 0, 0, 255));
        QCOMPARE(px(a, 15, 9), qRgba(255, 0, 0, 255));
        QCOMPARE(qAlpha(px(b, 10, 7)), 255);
        QVERIFY(std::abs(qGreen(px(b, 10, 7)) - 90) <= 1);
        // ring(): w pixels inside the box, each pixel drawn once
        QImage r1 = cardkit::newCanvas(40, 30);
        cardkit::ring(r1, cardkit::pilRect(2, 3, 21, 12), QColor(0, 0, 255, 100), 3);
        int ringPx = 0;
        for (int y = 0; y < 30; ++y) for (int x = 0; x < 40; ++x) if (qAlpha(px(r1, x, y))) { ++ringPx; QCOMPARE(qAlpha(px(r1, x, y)), 100); }
        QCOMPARE(ringPx, 20 * 10 - 14 * 4);
        QCOMPARE(qAlpha(px(r1, 12, 8)), 0);
        QImage r2 = cardkit::newCanvas(10, 10);
        cardkit::ring(r2, cardkit::pilRect(0, 0, 4, 3), QColor(0, 0, 255, 100), 3);   // thinner than two widths: all ring
        ringPx = 0;
        for (int y = 0; y < 10; ++y) for (int x = 0; x < 10; ++x) if (qAlpha(px(r2, x, y))) { ++ringPx; QCOMPARE(qAlpha(px(r2, x, y)), 100); }
        QCOMPARE(ringPx, 20);
        QImage r3 = red;
        cardkit::ring(r3, cardkit::pilRect(2, 3, 21, 12), QColor(255, 255, 255, 90), 2, true);
        QCOMPARE(qAlpha(px(r3, 2, 3)), 90);
        QCOMPARE(qAlpha(px(r3, 10, 7)), 255);
        // _chip(): the box, its outline and its label
        const cardkit::Face f = cardkit::loadFont(cardkit::boldCandidates(), 26);
        QImage c = cardkit::newCanvas(400, 120);
        const cardkit::Chip ch = cardkit::chip(c, 10, 20, "COMMUNITY", f, QColor(255, 255, 255, 40), QColor(255, 255, 255, 240), 1.0, QColor(255, 255, 255, 120));
        QCOMPARE(ch.right, 10 + f.width("COMMUNITY") + 40);
        QCOMPARE(ch.height, f.height() + 16);
        QCOMPARE(qAlpha(px(c, 10, 20)), 120);
        QCOMPARE(qAlpha(px(c, ch.right, 20 + ch.height)), 120);
        QCOMPARE(qAlpha(px(c, 12, 22)), 40);
        QCOMPARE(qAlpha(px(c, ch.right + 1, 20)), 0);
        QCOMPARE(inkBox(c), QRect(QPoint(10, 20), QPoint(ch.right, 20 + ch.height)));
        QVERIFY(inkBox(c, 200).width() > 50);                                          // the label
        // _spaced(): each code point advanced by its own advance + spacing
        QImage sp = cardkit::newCanvas(800, 80);
        const int adv = cardkit::spaced(sp, QPointF(10, 10), "ROANOKE", f, Qt::white, 4);
        double x = 10;
        for (QChar ch : QStringLiteral("ROANOKE")) x += f.length(QString(ch)) + 4;
        QCOMPARE(adv, int(x - 10));
        QVERIFY(inkBox(sp).right() < 10 + adv);
    }

    void layersVsScript() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        const PyRun r = runPy(kPyLayers, {scriptsDir(), m_tmp.path()});
        QVERIFY2(r.ok, r.err.constData());
        const QColor ink(11, 18, 32);
        struct L { const char* name; QImage ours; int tol; };
        QImage s70 = cardkit::newCanvas(1920, 1080), s62 = cardkit::newCanvas(1920, 1080), w40 = cardkit::newCanvas(1920, 1080),
               w150 = cardkit::newCanvas(1920, 1080), vig = cardkit::newCanvas(1920, 1080);
        cardkit::scrim(s70, ink, 0.70, 225);
        cardkit::scrim(s62, ink, 0.62, 205);
        cardkit::wash(w40, ink, 40);
        cardkit::wash(w150, ink, 150);
        cardkit::bottomVignette(vig);
        QImage opaque(1920, 1080, QImage::Format_ARGB32_Premultiplied), half(1920, 1080, QImage::Format_ARGB32_Premultiplied);
        opaque.fill(qRgba(200, 150, 90, 255));
        half.fill(qPremultiply(qRgba(200, 150, 90, 128)));
        cardkit::scrim(opaque, ink, 0.74, 236);
        cardkit::wash(half, ink, 120);
        const L ls[] = {{"scrim70", s70, 1}, {"scrim62", s62, 1}, {"wash40", w40, 1}, {"wash150", w150, 1}, {"vignette", vig, 1},
                        {"scrim74_opaque", opaque, 2}, {"wash120_half", half, 2}};
        for (const L& l : ls) {
            const QImage py(tmp(QStringLiteral("py_%1.png").arg(QLatin1String(l.name))));
            QVERIFY2(!py.isNull(), l.name);
            int alphaMax = 0;
            const Diff d = diffPremultiplied(l.ours, py, &alphaMax);
            qInfo("%s: %lld pixels off by up to %d (alpha %d)", l.name, d.n, d.max, alphaMax);
            QVERIFY2(d.max <= l.tol, l.name);
            if (l.tol == 1) QCOMPARE(alphaMax, 0);                                     // over transparent: alpha exact
        }
    }

    // ------------------------------------------------------------------------------------------ gradient bed
    void gradientBed() {
        const QImage b = cardkit::gradientBed(1920, 1080, QColor(216, 0, 24), QColor(11, 18, 32));
        QCOMPARE(b.size(), QSize(1920, 1080));
        QCOMPARE(b.format(), QImage::Format_RGB888);
        QCOMPARE(b, cardkit::gradientBed(1920, 1080, QColor(216, 0, 24), QColor(11, 18, 32)));
        const QImage c = b.convertToFormat(QImage::Format_RGB32);
        for (int y : {6, 600, 1074}) {                                                 // the +10 scanline on every 6th row
            const int d = qBlue(c.pixel(960, y)) - qBlue(c.pixel(960, y + 1));
            QVERIFY2(d >= 9 && d <= 11, qPrintable(QStringLiteral("row %1: %2").arg(y).arg(d)));
        }
        QVERIFY(qRed(c.pixel(1650, 130)) > qRed(c.pixel(100, 131)) + 30);              // accent glow top right
        QVERIFY(qBlue(c.pixel(150, 1031)) > qBlue(c.pixel(1800, 1031)) + 10);          // cool glow bottom left
        QCOMPARE(c.pixel(0, 0), qRgb(43, 54, 76));                                     // ink + (22, 26, 34) + the row-0 scanline
        QCOMPARE(cardkit::gradientBed(1, 1, Qt::red, Qt::black).size(), QSize(1, 1));
        QCOMPARE(cardkit::gradientBed(0, 0, Qt::red, Qt::black).size(), QSize(1, 1));
    }

    void gradientBedVsNumpy() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        struct B { int w, h; QColor accent, ink; };
        const B bs[] = {{1920, 1080, QColor(216, 0, 24), QColor(11, 18, 32)}, {641, 359, QColor(40, 200, 90), QColor(250, 240, 230)},
                        {320, 181, QColor(255, 255, 255), QColor(3, 2, 1)}};
        QJsonArray manifest;
        for (int i = 0; i < 3; ++i)
            manifest.append(QJsonObject{{"w", bs[i].w}, {"h", bs[i].h}, {"accent", QJsonArray{bs[i].accent.red(), bs[i].accent.green(), bs[i].accent.blue()}},
                                        {"ink", QJsonArray{bs[i].ink.red(), bs[i].ink.green(), bs[i].ink.blue()}}, {"out", tmp(QStringLiteral("py_bed%1.png").arg(i))}});
        QVERIFY(writeJson(tmp("bed.json"), manifest));
        const PyRun r = runPy(kPyBed, {scriptsDir(), tmp("bed.json")});
        QVERIFY2(r.ok, r.err.constData());
        for (int i = 0; i < 3; ++i) {
            const QImage py = QImage(tmp(QStringLiteral("py_bed%1.png").arg(i))).convertToFormat(QImage::Format_RGB888);
            const QImage ours = cardkit::gradientBed(bs[i].w, bs[i].h, bs[i].accent, bs[i].ink);
            QCOMPARE(py.size(), ours.size());
            const Diff d = diffBytes(rawBytes(ours, 3), rawBytes(py, 3));
            const double frac = double(d.n) / (3.0 * ours.width() * ours.height());
            qInfo("bed %dx%d: %lld of %d bytes differ (%.4f %%), max %d", ours.width(), ours.height(), d.n, 3 * ours.width() * ours.height(), 100 * frac, d.max);
            QVERIFY(d.max <= 1);
            QVERIFY(frac < 0.005);
        }
    }

    // ------------------------------------------------------------------------------------------ image I/O
    void exifAndTruncationParsing() {
        const QByteArray jpeg = encode(quadrants(160, 80), "jpeg", 95);
        QVERIFY(jpeg.startsWith("\xff\xd8"));
        QCOMPARE(imageio::exifOrientation(jpeg), 0);
        for (quint16 o = 1; o <= 8; ++o) QCOMPARE(imageio::exifOrientation(withExif(jpeg, o)), int(o));
        QCOMPARE(imageio::exifOrientation(withExif(jpeg, 6, true)), 6);                 // big-endian TIFF
        QCOMPARE(imageio::exifOrientation(withExif(jpeg, 8, false, true)), 8);          // LONG instead of SHORT
        const QByteArray xmp = QByteArray("http://ns.adobe.com/xap/1.0/") + '\0' + "<x:xmpmeta/>";
        QCOMPARE(imageio::exifOrientation(withApp1(withExif(jpeg, 3), xmp)), 3);        // an XMP APP1 before the EXIF one
        QCOMPARE(imageio::exifOrientation(withExif(jpeg, 9)), 0);
        QCOMPARE(imageio::exifOrientation(withExif(jpeg, 0)), 0);
        const QByteArray png = encode(quadrants(160, 80), "png", -1);
        QCOMPARE(imageio::exifOrientation(png), 0);
        QCOMPARE(imageio::exifOrientation(pngWithExif(png, 7)), 7);
        QCOMPARE(imageio::exifOrientation(QByteArray("Exif\0\0", 6) + tiffOrientation(5)), 5);
        QCOMPARE(imageio::exifOrientation(tiffOrientation(2, true)), 2);
        QCOMPARE(imageio::exifOrientation(QByteArray()), 0);
        QCOMPARE(imageio::exifOrientation("garbage that is not a picture at all"), 0);
        const bool webp = QImageWriter::supportedImageFormats().contains("webp");
        if (webp) QCOMPARE(imageio::exifOrientation(webpWithExif(encode(quadrants(160, 80), "webp", 100), 160, 80, 6)), 6);

        // truncation: JPEG needs its EOI after the scan; an EXIF thumbnail's EOI does not count
        QCOMPARE(imageio::truncation(jpeg), QString());
        QVERIFY(!imageio::truncation(jpeg.left(jpeg.size() * 7 / 10)).isEmpty());
        QVERIFY(!imageio::truncation(jpeg.left(jpeg.size() - 2)).isEmpty());          // only the EOI missing
        QCOMPARE(imageio::truncation(jpeg + "trailing bytes a phone appends"), QString());
        const QByteArray thumbed = withApp1(jpeg, QByteArray("Exif\0\0", 6) + tiffOrientation(1) + encode(quadrants(16, 8), "jpeg", 80));
        QCOMPARE(imageio::truncation(thumbed), QString());
        QVERIFY(!imageio::truncation(thumbed.left(thumbed.size() - 300)).isEmpty());
        // PNG: a cut inside a chunk, or right after image data, is truncated; a missing IEND after other chunks is not
        const QByteArray big = encode(photoLike(200, 120), "png", -1);
        const qsizetype iend = big.lastIndexOf("IEND") - 4;
        QCOMPARE(imageio::truncation(big), QString());
        QVERIFY(!imageio::truncation(big.left(iend)).isEmpty());
        QVERIFY(!imageio::truncation(big.left(iend - 40)).isEmpty());
        QVERIFY(!imageio::truncation(big.left(20)).isEmpty());
        QVERIFY(!imageio::truncation(big.left(8)).isEmpty());
        QCOMPARE(imageio::truncation(big.left(iend) + pngChunk("tEXt", QByteArray("Comment\0late", 12))), QString());
        QCOMPARE(imageio::truncation(big + "junk"), QString());
        if (webp) {
            const QByteArray w = encode(photoLike(120, 80), "webp", 100);
            QCOMPARE(imageio::truncation(w), QString());
            QVERIFY(!imageio::truncation(w.left(w.size() - 7)).isEmpty());
        }
        QCOMPARE(imageio::truncation("GIF89a and whatever follows"), QString());           // not checked
    }

    void orientationFromDisplayMatrix() {
        // av_display_rotation_set takes a clockwise angle; ffmpeg's autorotate then picks the filters this maps to
        struct M { double angle; int hflip, vflip; int expect; };
        const M ms[] = {{0, 0, 0, 1}, {90, 0, 0, 6}, {180, 0, 0, 3}, {270, 0, 0, 8}, {-90, 0, 0, 8}, {0, 1, 0, 2}, {0, 0, 1, 4},
                        {0, 1, 1, 3}, {90, 1, 0, 5}, {270, 1, 0, 7}, {45, 0, 0, 0}, {-45, 0, 0, 0}};
        for (const M& m : ms) {
            int32_t mat[9];
            av_display_rotation_set(mat, m.angle);
            av_display_matrix_flip(mat, m.hflip, m.vflip);
            QVERIFY2(imageio::orientationFromMatrix(mat) == m.expect,
                     qPrintable(QStringLiteral("angle %1 hflip %2 vflip %3 -> %4").arg(m.angle).arg(m.hflip).arg(m.vflip).arg(imageio::orientationFromMatrix(mat))));
        }
        const int32_t zero[9] = {};
        QCOMPARE(imageio::orientationFromMatrix(zero), 0);
        // orient() itself on the quadrant picture
        const QImage q = quadrants(160, 80);
        for (int o = 1; o <= 8; ++o) {
            const QImage t = imageio::orient(q, o);
            QCOMPARE(t.size(), o >= 5 ? QSize(80, 160) : QSize(160, 80));
            QCOMPARE(quadrantCode(t), QString::fromLatin1(kUpright[o - 1]));
        }
        QCOMPARE(imageio::orient(q, 9), q);
    }

    void exifRotatedJpegLoadsUpright() {
        const QByteArray jpeg = encode(quadrants(160, 80), "jpeg", 95);
        for (int o = 1; o <= 8; ++o) {
            const QString f = tmp(QStringLiteral("exif%1.jpg").arg(o));
            QVERIFY(writeBytes(f, withExif(jpeg, quint16(o))));
            for (bool useQt : {true, false}) {
                imageio::LoadOptions opt;
                opt.useQt = useQt;
                const imageio::Loaded l = imageio::load(f, opt);
                const QString what = QStringLiteral("orientation %1 via %2: %3").arg(o).arg(QLatin1String(useQt ? "qt" : "libav"), l.error);
                QVERIFY2(l.ok(), qPrintable(what));
                QCOMPARE(l.via, useQt ? QStringLiteral("qt") : QStringLiteral("libav"));
                QCOMPARE(l.format, useQt ? QStringLiteral("jpeg") : QStringLiteral("mjpeg"));
                QVERIFY2(l.orientation == o, qPrintable(what + " -> " + QString::number(l.orientation)));
                QCOMPARE(l.storedSize, QSize(160, 80));
                QCOMPARE(l.image.size(), o >= 5 ? QSize(80, 160) : QSize(160, 80));
                QVERIFY2(quadrantCode(l.image) == QLatin1String(kUpright[o - 1]), qPrintable(what + " shows " + quadrantCode(l.image)));
                QCOMPARE(l.image.format(), QImage::Format_RGB32);
            }
        }
        // autoTransform off: as stored
        for (bool useQt : {true, false}) {
            imageio::LoadOptions raw;
            raw.autoTransform = false;
            raw.useQt = useQt;
            const imageio::Loaded l = imageio::load(tmp("exif6.jpg"), raw);
            QVERIFY2(l.ok(), qPrintable(l.error));
            QCOMPARE(l.orientation, 1);
            QCOMPARE(quadrantCode(l.image), QStringLiteral("RGBY"));
        }
        // a PNG eXIf: Qt's PNG reader does not turn it, exif_transpose does, so the Qt path applies the file's block
        QVERIFY(writeBytes(tmp("exif8.png"), pngWithExif(encode(quadrants(160, 80), "png", -1), 8)));
        for (bool useQt : {true, false}) {
            imageio::LoadOptions opt;
            opt.useQt = useQt;
            const imageio::Loaded l = imageio::load(tmp("exif8.png"), opt);
            QVERIFY2(l.ok(), qPrintable(l.error));
            QCOMPARE(l.orientation, 8);
            QCOMPARE(l.storedSize, QSize(160, 80));
            QCOMPARE(quadrantCode(l.image), QStringLiteral("GYRB"));
        }
    }

    void exifRotatedVsPillow() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        const QImage photo = photoLike(320, 200);
        const QByteArray jpeg = encode(photo, "jpeg", 92);
        QJsonArray manifest;
        QStringList files;
        for (int o : {1, 3, 5, 6, 7, 8}) files << tmp(QStringLiteral("pexif%1.jpg").arg(o));
        for (int i = 0; i < files.size(); ++i) QVERIFY(writeBytes(files[i], withExif(jpeg, quint16(QList<int>{1, 3, 5, 6, 7, 8}[i]))));
        files << tmp("pexif6.png");
        QVERIFY(writeBytes(files.last(), pngWithExif(encode(photo, "png", -1), 6)));
        if (QImageWriter::supportedImageFormats().contains("webp")) {
            files << tmp("pexif6.webp");
            QVERIFY(writeBytes(files.last(), webpWithExif(encode(photo, "webp", 100), photo.width(), photo.height(), 6)));
        }
        for (const QString& f : files) manifest.append(QJsonObject{{"in", f}, {"out", f + ".pil.png"}});
        QVERIFY(writeJson(tmp("exif.json"), manifest));
        const PyRun r = runPy(kPyExif, {tmp("exif.json")});
        QVERIFY2(r.ok, r.err.constData());
        for (const QString& f : files) {
            const QImage pil(f + ".pil.png");
            QVERIFY2(!pil.isNull(), qPrintable(f));
            const bool lossless = !f.endsWith(".jpg");
            for (bool useQt : {true, false}) {
                imageio::LoadOptions opt;
                opt.useQt = useQt;
                const imageio::Loaded l = imageio::load(f, opt);
                QVERIFY2(l.ok(), qPrintable(l.error));
                QCOMPARE(l.image.size(), pil.size());
                const double db = psnrRgb(l.image, pil);
                qInfo("%s via %s: %.1f dB against Pillow", qPrintable(QFileInfo(f).fileName()), qPrintable(l.via), db);
                // the same libjpeg family under Qt and Pillow; libav's JPEG decoder rounds its IDCT / upsampling differently
                QVERIFY2(db >= (lossless ? 99.0 : useQt ? 40.0 : 30.0), qPrintable(QStringLiteral("%1 via %2: %3 dB").arg(f, l.via).arg(db)));
            }
        }
    }

    void webpThroughLibav() {
        if (!QImageWriter::supportedImageFormats().contains("webp")) QSKIP("no qwebp here to write the WebP fixtures");
        // lossless with alpha: exact through both decoders
        QImage src(64, 48, QImage::Format_ARGB32);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                src.setPixel(x, y, qRgba((x * 37 + y * 11) & 255, (x * 5 + y * 23) & 255, (x * y) & 255, x == 0 ? 0 : x == 63 ? 255 : (x * 4 + y) & 255));
        const QByteArray ll = encode(src, "webp", 100);
        QCOMPARE(ll.mid(12, 4), QByteArray("VP8L"));
        QVERIFY(writeBytes(tmp("alpha.webp"), ll));
        imageio::LoadOptions lib;
        lib.useQt = false;
        const imageio::Loaded lv = imageio::load(tmp("alpha.webp"), lib);
        QVERIFY2(lv.ok(), qPrintable(lv.error));
        QCOMPARE(lv.via, QStringLiteral("libav"));
        QCOMPARE(lv.format, QStringLiteral("webp"));
        QCOMPARE(lv.image.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(lv.image, src.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        const imageio::Loaded qv = imageio::load(tmp("alpha.webp"));
        QCOMPARE(qv.via, QStringLiteral("qt"));
        QCOMPARE(qv.image, lv.image);
        // lossy and opaque: libav's and libwebp's YUV -> RGB round differently
        const QImage photo = photoLike(320, 200);
        QVERIFY(writeBytes(tmp("lossy.webp"), encode(photo, "webp", 80)));
        const imageio::Loaded la = imageio::load(tmp("lossy.webp"), lib), lq = imageio::load(tmp("lossy.webp"));
        QVERIFY2(la.ok() && lq.ok(), qPrintable(la.error + lq.error));
        QCOMPARE(la.image.format(), QImage::Format_RGB32);
        QCOMPARE(la.image.size(), photo.size());
        const double between = psnrRgb(la.image, lq.image), toSource = psnrRgb(la.image, photo);
        qInfo("lossy WebP: libav vs libwebp %.1f dB, libav vs source %.1f dB", between, toSource);
        QVERIFY(between >= 30.0);
        QVERIFY(toSource >= 28.0);
        // EXIF orientation 6 in an extended (VP8X) file: upright through both
        QVERIFY(writeBytes(tmp("exif6.webp"), webpWithExif(encode(quadrants(160, 80), "webp", 100), 160, 80, 6)));
        for (bool useQt : {false, true}) {
            imageio::LoadOptions opt;
            opt.useQt = useQt;
            const imageio::Loaded l = imageio::load(tmp("exif6.webp"), opt);
            QVERIFY2(l.ok(), qPrintable(l.error));
            QCOMPARE(l.via, useQt ? QStringLiteral("qt") : QStringLiteral("libav"));
            QCOMPARE(l.orientation, 6);
            QCOMPARE(l.storedSize, QSize(160, 80));
            QCOMPARE(l.image.size(), QSize(80, 160));
            QCOMPARE(quadrantCode(l.image), QStringLiteral("BRYG"));
        }
    }

    void webpWithoutQwebpStaged() {
        if (!QImageWriter::supportedImageFormats().contains("webp")) QSKIP("no qwebp here to write the WebP fixtures");
        const QImage photo = photoLike(240, 160);
        QVERIFY(writeBytes(tmp("child_photo.webp"), encode(photo, "webp", 100)));
        QVERIFY(writeBytes(tmp("child_exif6.webp"), webpWithExif(encode(quadrants(160, 80), "webp", 100), 160, 80, 6)));
        QVERIFY(writeBytes(tmp("child_photo.jpg"), encode(photo, "jpeg", 92)));
        // the plugin folder the package stages: imageformats/qjpeg.dll and nothing else
        const QString plugins = tmp("child_plugins");
        QVERIFY(QDir().mkpath(plugins + "/imageformats"));
        QString qjpeg;
        for (const QString& d : QCoreApplication::libraryPaths())
            if (QFileInfo::exists(d + "/imageformats/qjpeg.dll")) { qjpeg = d + "/imageformats/qjpeg.dll"; break; }
        const bool stagedJpeg = !qjpeg.isEmpty() && QFile::copy(qjpeg, plugins + "/imageformats/qjpeg.dll");
        QProcess p;
        p.start(QCoreApplication::applicationFilePath(),
                {"--load-child", plugins, m_tmp.path(), tmp("child_photo.webp"), tmp("child_exif6.webp"), tmp("child_photo.jpg")});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(p.exitCode(), 0);
        const QList<QByteArray> lines = p.readAllStandardOutput().trimmed().split('\n');
        const QJsonObject res = QJsonDocument::fromJson(lines.last().trimmed()).object();
        const QStringList formats = toStrings(res["formats"]);
        qInfo("child: formats %s, plugin paths %s", qPrintable(formats.join(' ')), qPrintable(toStrings(res["libraryPaths"]).join(';')));
        QVERIFY2(!formats.contains("webp"), "the child still sees a WebP plugin");
        QCOMPARE(formats.contains("jpeg"), stagedJpeg);
        const QJsonArray rs = res["results"].toArray();
        QCOMPARE(rs.size(), 3);
        const QJsonObject a = rs[0].toObject(), b = rs[1].toObject(), c = rs[2].toObject();
        QVERIFY2(a["error"].toString().isEmpty(), qPrintable(a["error"].toString()));
        QCOMPARE(a["via"].toString(), QStringLiteral("libav"));
        QCOMPARE(a["format"].toString(), QStringLiteral("webp"));
        QCOMPARE(QSize(a["width"].toInt(), a["height"].toInt()), photo.size());
        QCOMPARE(QImage(a["png"].toString()).convertToFormat(QImage::Format_RGB32), photo);    // lossless: every pixel
        QCOMPARE(b["via"].toString(), QStringLiteral("libav"));
        QCOMPARE(b["orientation"].toInt(), 6);
        QCOMPARE(QSize(b["width"].toInt(), b["height"].toInt()), QSize(80, 160));
        QCOMPARE(quadrantCode(QImage(b["png"].toString())), QStringLiteral("BRYG"));
        QCOMPARE(c["via"].toString(), stagedJpeg ? QStringLiteral("qt") : QStringLiteral("libav"));
        QCOMPARE(QSize(c["width"].toInt(), c["height"].toInt()), photo.size());
    }

    void refusesUrlsTruncatedOversized() {
        QElapsedTimer t;
        t.start();
        imageio::Loaded l = imageio::load("http://127.0.0.1:9/photo.jpg");
        QVERIFY(!l.ok() && l.error.contains("not a local file"));
        l = imageio::load("file:///C:/Windows/win.ini");
        QVERIFY(!l.ok() && l.error.contains("not a local file"));
        QVERIFY(t.elapsed() < 2000);
        l = imageio::load(tmp("missing.jpg"));
        QVERIFY(!l.ok() && l.error.contains("no such file"));
        l = imageio::load(m_tmp.path());
        QVERIFY(!l.ok() && l.error.contains("not a file"));
        QVERIFY(!imageio::load(QString()).ok());
        QVERIFY(writeBytes(tmp("text.jpg"), QByteArray("this is not a picture\n").repeated(40)));
        l = imageio::load(tmp("text.jpg"));
        QVERIFY(!l.ok() && !l.error.isEmpty());
        // truncated: refused like Pillow (the scene then gets the gradient bed), unless allowed
        const QByteArray jpeg = encode(photoLike(320, 200), "jpeg", 90);
        QVERIFY(writeBytes(tmp("cut.jpg"), jpeg.left(jpeg.size() * 6 / 10)));
        for (bool useQt : {true, false}) {
            imageio::LoadOptions opt;
            opt.useQt = useQt;
            l = imageio::load(tmp("cut.jpg"), opt);
            QVERIFY(!l.ok() && l.truncated);
            QVERIFY2(l.error.startsWith("truncated:"), qPrintable(l.error));
        }
        imageio::LoadOptions lax;
        lax.allowTruncated = true;
        l = imageio::load(tmp("cut.jpg"), lax);
        QVERIFY2(l.ok() && l.via == "qt" && l.image.size() == QSize(320, 200), qPrintable(l.error));
        QVERIFY(writeBytes(tmp("junk.jpg"), jpeg + "JUNKJUNK"));
        QVERIFY(imageio::load(tmp("junk.jpg")).ok());
        const QByteArray png = encode(photoLike(200, 120), "png", -1);
        QVERIFY(writeBytes(tmp("cut.png"), png.left(png.size() / 2)));
        l = imageio::load(tmp("cut.png"));
        QVERIFY(!l.ok() && l.truncated);
        if (QImageWriter::supportedImageFormats().contains("webp")) {
            const QByteArray w = encode(photoLike(200, 120), "webp", 90);
            QVERIFY(writeBytes(tmp("cut.webp"), w.left(w.size() - 50)));
            l = imageio::load(tmp("cut.webp"));
            QVERIFY(!l.ok() && l.truncated);
        }
        // too many pixels: refused before decoding, on both paths
        QImage big(3000, 3000, QImage::Format_RGB32);
        big.fill(qRgb(10, 20, 30));
        QVERIFY(imageio::save(big, tmp("big.png")));
        for (bool useQt : {true, false}) {
            imageio::LoadOptions small;
            small.maxPixels = 4000000;
            small.useQt = useQt;
            QElapsedTimer bt;
            bt.start();
            l = imageio::load(tmp("big.png"), small);
            QVERIFY2(!l.ok() && l.error.contains("too large"), qPrintable(l.error));
            QVERIFY(bt.elapsed() < 2000);
        }
        l = imageio::load(tmp("big.png"));
        QVERIFY(l.ok() && l.image.size() == QSize(3000, 3000));
        // a cancel flag stops libav at once
        std::atomic<bool> stop{true};
        imageio::LoadOptions c;
        c.useQt = false;
        c.cancel = &stop;
        l = imageio::load(tmp("junk.jpg"), c);
        QVERIFY(!l.ok() && l.cancelled);
    }

    void resizeCoverBrightness() {
        const QImage photo = photoLike(600, 400);
        QImage flat(600, 400, QImage::Format_RGB32);
        flat.fill(qRgb(77, 140, 201));
        for (const QSize& s : {QSize(200, 133), QSize(450, 300), QSize(1200, 800), QSize(600, 400), QSize(1, 1)}) {
            const QImage r = imageio::resizeHQ(photo, s);
            QCOMPARE(r.size(), s);
            QCOMPARE(r.format(), QImage::Format_RGB32);
            const QImage f = imageio::resizeHQ(flat, s);
            for (int y = 0; y < f.height(); ++y)
                for (int x = 0; x < f.width(); ++x) {
                    const QRgb p = px(f, x, y);
                    QVERIFY(std::abs(qRed(p) - 77) <= 1 && std::abs(qGreen(p) - 140) <= 1 && std::abs(qBlue(p) - 201) <= 1 && qAlpha(p) == 255);
                }
        }
        // premultiplied: Lanczos ringing never pushes colour above alpha
        QImage pm = photo.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&pm);
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.fillRect(QRect(0, 0, 300, 400), Qt::transparent);
        }
        for (const QSize& s : {QSize(200, 133), QSize(1200, 800)}) {
            const QImage r = imageio::resizeHQ(pm, s);
            QCOMPARE(r.format(), QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < r.height(); ++y)
                for (int x = 0; x < r.width(); ++x) {
                    const QRgb p = px(r, x, y);
                    QVERIFY(qRed(p) <= qAlpha(p) && qGreen(p) <= qAlpha(p) && qBlue(p) <= qAlpha(p));
                }
        }
        // _cover(): scale by the larger ratio, crop centred across and at 0.4 down
        QImage across(400, 100, QImage::Format_RGB32), down(100, 400, QImage::Format_RGB32);
        for (int y = 0; y < 100; ++y) for (int x = 0; x < 400; ++x) across.setPixel(x, y, qRgb(x * 255 / 399, 0, 0));
        for (int y = 0; y < 400; ++y) for (int x = 0; x < 100; ++x) down.setPixel(x, y, qRgb(0, y * 255 / 399, 0));
        const QImage ca = imageio::cover(across, 288, 162), cd = imageio::cover(down, 288, 162);
        QCOMPARE(ca.size(), QSize(288, 162));
        QCOMPARE(cd.size(), QSize(288, 162));
        // k = 1.62: 648 wide, crop from x 180 -> source x 111.1 at the left edge, 288.3 at the right
        QVERIFY2(std::abs(qRed(ca.pixel(0, 80)) - 111.1 * 255 / 399) <= 4, qPrintable(QString::number(qRed(ca.pixel(0, 80)))));
        QVERIFY2(std::abs(qRed(ca.pixel(287, 80)) - 288.3 * 255 / 399) <= 4, qPrintable(QString::number(qRed(ca.pixel(287, 80)))));
        // k = 2.88: 1152 high, crop from y int(990 * 0.4) = 396 -> source y 137.5
        QVERIFY2(std::abs(qGreen(cd.pixel(144, 0)) - 137.5 * 255 / 399) <= 4, qPrintable(QString::number(qGreen(cd.pixel(144, 0)))));
        QCOMPARE(imageio::cover(photo, 600, 400), photo);
        // ImageEnhance.Brightness: (uint8)(factor * v) per colour byte, alpha kept
        QImage ramp(256, 1, QImage::Format_ARGB32);
        for (int x = 0; x < 256; ++x) ramp.setPixel(x, 0, qRgba(x, 255 - x, x / 2, 255));
        for (double f : {0.42, 0.55, 0.5, 1.0, 0.0}) {
            const QImage b = imageio::brightness(ramp, f);
            for (int x = 0; x < 256; ++x) {
                const QRgb p = px(b, x, 0);
                const float ff = float(f);
                QVERIFY(qRed(p) == int(ff * float(x)) && qGreen(p) == int(ff * float(255 - x)) && qBlue(p) == int(ff * float(x / 2)) && qAlpha(p) == 255);
            }
        }
    }

    void resizeCoverBrightnessVsPillow() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        const QImage photo = photoLike(600, 400);
        QVERIFY(imageio::save(photo, tmp("rs_photo.png")));
        struct R { QString op; int w, h; double f; QImage ours; double minDb; };
        const QList<R> rs = {
            {"cover", 288, 162, 0, imageio::cover(photo, 288, 162), 30.0},           // _backdrop's small cover (8x down: Lanczos)
            {"cover", 2880, 1620, 0, imageio::cover(photo, 2880, 1620), 30.0},       // the photo bed (4.8x up: Lanczos)
            {"resize", 200, 133, 0, imageio::resizeHQ(photo, QSize(200, 133)), 30.0},
            {"resize", 450, 300, 0, imageio::resizeHQ(photo, QSize(450, 300)), 30.0}, // 1.33x down: Qt's smooth scaling
            {"brightness", 0, 0, 0.42, imageio::brightness(photo, 0.42), 99.0},
        };
        QJsonArray manifest;
        for (int i = 0; i < rs.size(); ++i)
            manifest.append(QJsonObject{{"op", rs[i].op}, {"w", rs[i].w}, {"h", rs[i].h}, {"f", rs[i].f}, {"ybias", 0.4},
                                        {"in", tmp("rs_photo.png")}, {"out", tmp(QStringLiteral("rs_pil%1.png").arg(i))}});
        QVERIFY(writeJson(tmp("rs.json"), manifest));
        const PyRun r = runPy(kPyResize, {scriptsDir(), tmp("rs.json")});
        QVERIFY2(r.ok, r.err.constData());
        for (int i = 0; i < rs.size(); ++i) {
            const QImage pil(tmp(QStringLiteral("rs_pil%1.png").arg(i)));
            QCOMPARE(rs[i].ours.size(), pil.size());
            const double db = psnrRgb(rs[i].ours, pil);
            qInfo("%s %dx%d: %.1f dB against Pillow", qPrintable(rs[i].op), pil.width(), pil.height(), db);
            QVERIFY2(db >= rs[i].minDb, qPrintable(QStringLiteral("%1: %2 dB").arg(rs[i].op).arg(db)));
        }
    }

    void saveWritesPngLevel3AndJpeg90() {
        const QString dir = tmp(QString::fromUtf16(u"out \u00fc\u58f0/nested deeper"));
        QImage pm = photoLike(160, 100).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&pm);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(QRect(0, 0, 40, 100), QColor(10, 200, 30, 120));
        }
        QString err;
        QVERIFY2(imageio::save(pm, dir + "/f.png", &err), qPrintable(err));
        const QByteArray png = readAll(dir + "/f.png");
        const qsizetype idat = png.indexOf("IDAT");
        QVERIFY(idat > 0);
        QCOMPARE(uchar(png[idat + 4]), uchar(0x78));
        QCOMPARE(uchar(png[idat + 5]) >> 6, 1);                                    // zlib FLEVEL 1 = levels 2..5 (compress_level=3)
        QCOMPARE(QImage(dir + "/f.png").convertToFormat(QImage::Format_ARGB32), pm.convertToFormat(QImage::Format_ARGB32));
        const QImage bed = cardkit::gradientBed(320, 180, QColor(216, 0, 24), QColor(11, 18, 32));
        QVERIFY(imageio::save(bed, dir + "/bed.png"));
        QCOMPARE(QImage(dir + "/bed.png").convertToFormat(QImage::Format_RGB888), bed);
        QVERIFY(imageio::save(pm, dir + "/f.jpg"));
        const QByteArray jpg = readAll(dir + "/f.jpg");
        QVERIFY(jpg.startsWith("\xff\xd8"));
        const qsizetype dqt = jpg.indexOf("\xff\xdb");
        QVERIFY(dqt > 0);
        QCOMPARE(int(uchar(jpg[dqt + 4])), 0);                                     // table 0, 8-bit
        QCOMPARE(int(uchar(jpg[dqt + 5])), 3);                                     // DC step at quality 90: (16 * 20 + 50) / 100
        const QImage back(dir + "/f.jpg");
        QCOMPARE(back.size(), pm.size());
        QVERIFY(!back.hasAlphaChannel());
        QCOMPARE(QDir(dir).entryList(QDir::Files | QDir::Hidden, QDir::Name), (QStringList{"bed.png", "f.jpg", "f.png"}));   // no temp files left
        // overwrite in place; refusals
        QVERIFY(imageio::save(bed, dir + "/f.png"));
        QCOMPARE(QImage(dir + "/f.png").size(), bed.size());
        QVERIFY(!imageio::save(bed, dir + "/f.bmp", &err) && err.contains("not a .png / .jpg"));
        QVERIFY(!imageio::save(QImage(), dir + "/g.png", &err));
        QVERIFY(!QFileInfo::exists(dir + "/g.png"));
    }

    // ------------------------------------------------------------------------------------------ threads (DEP-09 renders frames in parallel)
    void threadedRenderingIsDeterministic() {
        auto render = [] {
            const cardkit::Face hb = cardkit::loadFont(cardkit::boldCandidates(), 64), rg = cardkit::loadFont(cardkit::regularCandidates(), 36);
            QImage img = cardkit::newCanvas(1280, 720);
            cardkit::scrim(img, QColor(11, 18, 32), 0.62, 205);
            cardkit::chip(img, 74, 69, "HEADLINE NEWS", hb, QColor(216, 0, 24), Qt::white, 0.667);
            int y = 200;
            for (const QString& ln : cardkit::wrap("A long headline that wraps over a couple of lines for the thread test, and then some more", hb, 700, 3)) {
                cardkit::shadowText(img, QPointF(74, y), ln, hb, Qt::white, 5, QPoint(0, 2));
                y += 70;
            }
            cardkit::shadowText(img, QPointF(74, y), cardkit::ellipsize("A summary line long enough that it has to be cut off before the end", rg, 700), rg,
                                QColor(226, 233, 243), 3, QPoint(0, 1), 150);
            QImage bed = cardkit::gradientBed(640, 360, QColor(216, 0, 24), QColor(11, 18, 32)).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            blur::gaussianBlur(bed, 5);
            QPainter(&img).drawImage(600, 340, bed);
            return img;
        };
        const QImage ref = render();
        std::vector<QImage> out(4);
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i) ts.emplace_back([&out, &render, i] { for (int k = 0; k < 3; ++k) out[size_t(i)] = render(); });
        for (std::thread& t : ts) t.join();
        for (const QImage& o : out) QVERIFY(o == ref);
    }

    // ------------------------------------------------------------------------------------------ DEP-08b: spot_card layouts
    // `card` per fixture, through the exe and in process: sizes, the JSON contract, headline lines, safe areas and
    // transparency outside the chrome.
    void cardFixtures() {
        QVERIFY2(QFileInfo::exists(kExe), qPrintable("missing " + kExe));
        QVERIFY2(QFileInfo::exists(QDir(kCards).filePath("landscape.jpg")), "missing fixtures (tests/inhouse/data/cards)");
        const QString dir = tmp(QString::fromUtf16(u"cards ü声"));
        for (const char* name : kCardSpecs) {
            const QString spec = QDir(kCards).filePath(QLatin1String(name));
            const QString base = QDir(dir).filePath(QFileInfo(spec).completeBaseName());
            const ExeRun r = runExe({"card", "--spec", spec, "--out", base + ".png", "--bed", base + "_bed.png"});
            QVERIFY2(r.code == 0, r.out + r.err);
            QCOMPARE(r.lines, 1);
            QCOMPARE(keysOf(r.json), (QStringList{"bed", "furniture", "headlineLines", "lines", "ok"}));
            QCOMPARE(r.json.value("furniture").toString(), QDir::toNativeSeparators(QFileInfo(base + ".png").canonicalFilePath()));
            QCOMPARE(r.json.value("bed").toString(), QDir::toNativeSeparators(QFileInfo(base + "_bed.png").canonicalFilePath()));
            const QJsonObject js = readJson(spec).object();
            const QSize frame(js.value("width").toInt(1920), js.value("height").toInt(1080));
            const QImage furniture(base + ".png"), bed(base + "_bed.png");
            QCOMPARE(furniture.size(), frame);
            QVERIFY(furniture.hasAlphaChannel());
            QCOMPARE(bed.size(), frame);
            QVERIFY(!bed.hasAlphaChannel());
            QCOMPARE(QByteArray(readAll(base + "_bed.png").left(8)), QByteArray("\x89PNG\r\n\x1a\n", 8));
            // the same render in process, with its trace: identical pixels and result
            spotcard::Trace t;
            const QJsonObject ip = spotcard::runCard(spec, base + "_ip.png", QString(), &t);
            QVERIFY(ip.value("ok").toBool());
            QCOMPARE(ip.value("headlineLines"), r.json.value("headlineLines"));
            QCOMPARE(ip.value("lines"), r.json.value("lines"));
            QCOMPARE(ip.value("bed").toString(), QString());
            QVERIFY(QImage(base + "_ip.png") == furniture);
            const QStringList head = toStrings(r.json.value("headlineLines"));
            QVERIFY(head.size() <= 3);
            const QString violations = safeAreaViolations(t, frame);
            QVERIFY2(violations.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(name, violations)));
            QPoint at;
            const qint64 stray = inkOutsideChrome(furniture, t, &at);
            QVERIFY2(stray == 0, qPrintable(QStringLiteral("%1: %2 pixels outside the chrome, first at %3,%4").arg(name).arg(stray).arg(at.x()).arg(at.y())));
            const double s = frame.height() / 1080.0;
            const int strapTop = frame.height() - int(84 * s) - int(26 * s);
            QVERIFY(!t.box("strap").isEmpty() && t.box("strap").top() == strapTop);
            if (js.value("photo").toBool()) {                  // right of the scrim and above the strap: the photograph shows
                const QImage a = furniture.convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < strapTop; y += 3)
                    for (int x = int(frame.width() * 0.70); x < frame.width(); x += 3)
                        QVERIFY2(qAlpha(a.pixel(x, y)) == 0, qPrintable(QStringLiteral("%1: ink at %2,%3").arg(name).arg(x).arg(y)));
            }
            if (QLatin1String(name) == QLatin1String("card_nophoto.json")) {   // 3 lines, shrunk once, ellipsized
                QCOMPARE(head.size(), 3);
                QVERIFY(head.last().endsWith("..."));
                QCOMPARE(r.json.value("lines").toInt(), 4);
                const cardkit::Face f = cardkit::loadFont(cardkit::boldCandidates(), 64, true);
                for (const QString& l : head) QVERIFY2(f.width(l) <= int(1920 * 0.80), qPrintable(l));
                for (const spotcard::TextItem& it : t.texts) if (head.contains(it.text)) QCOMPARE(it.size, 64);
                QVERIFY(!t.box("kicker").isEmpty() && !t.box("bullet").isEmpty() && !t.box("vignette").isEmpty());
            }
            if (QLatin1String(name) == QLatin1String("card_small.json")) QCOMPARE(head, QStringList{"Short headline"});
        }
    }

    // `scenes` per fixture: the JSON contract, every bed (2880x1620 JPEG for a photo, the WxH gradient PNG without),
    // furniture sizes, safe areas and transparency; the inset / cover beds hold the picture where the layout puts it.
    void scenesFixtures() {
        struct Want { const char* layout; int w, h; };
        const QList<Want> box = {{"backdrop", 1600, 900}, {"box", 1600, 900}, {"box", 800, 1200}, {"box", 300, 1500}, {"gradient", 0, 0}, {"gradient", 0, 0}, {"backdrop", 1600, 900}};
        const QList<Want> full = {{"gradient", 0, 0}, {"cover", 1600, 900}, {"inset", 800, 1200}, {"inset", 900, 900}, {"inset", 800, 1200}, {"cover", 1400, 800},
                                  {"gradient", 0, 0}, {"gradient", 0, 0}, {"gradient", 0, 0}, {"gradient", 0, 0}, {"backdrop", 900, 900}};
        for (const char* name : kSceneSpecs) {
            const QList<Want>& want = QLatin1String(name) == QLatin1String("scenes_box.json") ? box : full;
            const QString dir = tmp(QStringLiteral("exe_%1").arg(QFileInfo(QLatin1String(name)).completeBaseName()));
            const QString spec = prepareScenes(QLatin1String(name), dir);
            QVERIFY(!spec.isEmpty());
            QElapsedTimer timer;
            timer.start();
            const ExeRun r = runExe({"scenes", spec});
            qInfo("%s: %lld ms through the exe", name, timer.elapsed());
            QVERIFY2(r.code == 0, r.out + r.err);
            QCOMPARE(r.lines, 1);
            QCOMPARE(keysOf(r.json), (QStringList{"ok", "scenes"}));
            const QJsonArray made = r.json.value("scenes").toArray();
            QCOMPARE(made.size(), want.size());
            const QJsonArray scenes = readJson(spec).object().value("scenes").toArray();
            for (int i = 0; i < made.size(); ++i) {
                const QJsonObject m = made[i].toObject(), sc = scenes[i].toObject();
                const QString tag = QStringLiteral("%1 scene %2").arg(name).arg(i);
                QCOMPARE(keysOf(m), (QStringList{"bed", "furniture", "kind", "layout", "photo", "photoSize"}));
                QCOMPARE(m.value("kind").toString(), sc.value("kind").toString());
                QVERIFY2(m.value("layout").toString() == QLatin1String(want[i].layout), qPrintable(tag + " " + m.value("layout").toString()));
                const bool photo = want[i].w > 0;
                QCOMPARE(m.value("photo").toBool(), photo);
                QCOMPARE(m.value("photoSize"), photo ? QJsonValue(QJsonArray{want[i].w, want[i].h}) : QJsonValue(QJsonValue::Null));
                QCOMPARE(m.value("furniture").toString(), QDir::toNativeSeparators(QFileInfo(sc.value("furniture").toString()).canonicalFilePath()));
                const QString bedPath = sc.value("bedBase").toString() + (photo ? ".jpg" : ".png");
                QCOMPARE(m.value("bed").toString(), QDir::toNativeSeparators(QFileInfo(bedPath).canonicalFilePath()));
                const QImage bed(bedPath), furniture(sc.value("furniture").toString());
                QCOMPARE(bed.size(), photo ? QSize(spotcard::kBedW, spotcard::kBedH) : QSize(1920, 1080));
                QCOMPARE(furniture.size(), QSize(1920, 1080));
                QVERIFY(furniture.hasAlphaChannel());
                const QByteArray head = readAll(bedPath).left(3);
                QVERIFY2(photo ? head.startsWith("\xff\xd8") : head.startsWith("\x89PN"), qPrintable(tag));
            }
            // in process with traces (to its own folder): the same results, safe areas, nothing outside the chrome
            const QString ipDir = tmp(QStringLiteral("ip_%1").arg(QFileInfo(QLatin1String(name)).completeBaseName()));
            const QString ipSpec = prepareScenes(QLatin1String(name), ipDir);
            QList<spotcard::Trace> traces;
            const QJsonObject ip = spotcard::runScenes(ipSpec, &traces);
            QVERIFY(ip.value("ok").toBool());
            QCOMPARE(traces.size(), want.size());
            QCOMPARE(swapDir(ip, QDir::toNativeSeparators(ipDir), QDir::toNativeSeparators(dir)), QJsonValue(r.json));
            for (int i = 0; i < traces.size(); ++i) {
                const QString tag = QStringLiteral("%1 scene %2").arg(name).arg(i);
                const QJsonObject sc = scenes[i].toObject();
                const QImage exeFurniture(sc.value("furniture").toString());
                const QImage furniture(QDir(ipDir).filePath(QFileInfo(sc.value("furniture").toString()).fileName()));
                QVERIFY2(furniture == exeFurniture, qPrintable(tag + ": threads changed the pixels"));
                const QString violations = safeAreaViolations(traces[i], furniture.size());
                QVERIFY2(violations.isEmpty(), qPrintable(tag + ": " + violations));
                QPoint at;
                const qint64 stray = inkOutsideChrome(furniture, traces[i], &at);
                QVERIFY2(stray == 0, qPrintable(QStringLiteral("%1: %2 pixels outside the chrome, first at %3,%4").arg(tag).arg(stray).arg(at.x()).arg(at.y())));
                if (QLatin1String(want[i].layout) == QLatin1String("box")) {   // the picture is framed whole on the right
                    const QRect b = traces[i].box("storyBox");
                    QVERIFY2(!b.isEmpty() && b.right() <= 1920 - int(1920 * 0.058) && b.left() > 1920 / 3, qPrintable(tag));
                    const QImage a = furniture.convertToFormat(QImage::Format_ARGB32);
                    QCOMPARE(qAlpha(a.pixel(b.center())), 255);
                }
            }
        }
        // inset / cover beds hold the picture: compare against the picture resized as the layout says
        const QString dir = tmp("exe_scenes_full");
        struct Inset { const char* bed; const char* photo; QRect at; };
        const Inset insets[] = {
            {"full_02_bed.jpg", "portrait.jpg", QRect(2880 - 993 - 288, (1620 - 1490) / 2, 993, 1490)},   // fh = 1490, fw = round(800 * 1490 / 1200)
            {"full_03_bed.jpg", "square.png", QRect(2880 - 1440 - 288, (1620 - 1440) / 2, 1440, 1440)},   // capped at half the width
            {"full_04_bed.jpg", "exif6.jpg", QRect(2880 - 993 - 288, (1620 - 1490) / 2, 993, 1490)},      // EXIF 6: upright 800x1200
        };
        for (const Inset& in : insets) {
            const QImage bed(QDir(dir).filePath(QLatin1String(in.bed)));
            const imageio::Loaded l = imageio::load(QDir(kCards).filePath(QLatin1String(in.photo)));
            QVERIFY(l.ok());
            const double db = psnrRgb(bed.copy(in.at), imageio::resizeHQ(l.image, in.at.size()));
            qInfo("%s inset: %.1f dB against the picture", in.bed, db);
            QVERIFY2(db >= 30.0, in.bed);
            QVERIFY(psnrRgb(bed.copy(in.at.translated(-200, 0)), imageio::resizeHQ(l.image, in.at.size())) < 20.0);   // and not elsewhere
        }
        const QImage cover(QDir(dir).filePath("full_01_bed.jpg"));
        const imageio::Loaded land = imageio::load(QDir(kCards).filePath("landscape.jpg"));
        const QImage want = imageio::cover(land.image, spotcard::kBedW, spotcard::kBedH, 0.4);
        QVERIFY(psnrRgb(cover, want) >= 30.0);
        QVERIFY(psnrRgb(QImage(QDir(dir).filePath("full_05_bed.jpg")), imageio::cover(imageio::load(QDir(kCards).filePath("photo.webp")).image, 2880, 1620, 0.4)) >= 30.0);
    }

    // photo beds in process: the four layouts, the 80 px floor, unreadable pictures, the dimmed backdrops
    void photoBedLayouts() {
        const QString land = QDir(kCards).filePath("landscape.jpg"), port = QDir(kCards).filePath("portrait.jpg");
        struct Case { QString photo, kind, story, layout; double dimMax; };
        const QList<Case> cases = {
            {land, "story", "full", "cover", 9.0}, {port, "story", "full", "inset", 9.0}, {land, "intro", "full", "backdrop", 0.45},
            {land, "outro", "box", "backdrop", 0.45}, {land, "story", "box", "box", 0.58}, {port, "story", "box", "box", 0.58},
        };
        const imageio::Loaded src = imageio::load(land);
        auto mean = [](const QImage& img) {
            const QImage c = img.scaled(96, 54, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
            double sum = 0;
            for (int y = 0; y < c.height(); ++y) for (int x = 0; x < c.width(); ++x) { const QRgb p = c.pixel(x, y); sum += qRed(p) + qGreen(p) + qBlue(p); }
            return sum / (3.0 * c.width() * c.height());
        };
        const double srcMean = mean(src.image);
        for (const Case& c : cases) {
            const spotcard::PhotoBed b = spotcard::photoBed(c.photo, c.kind, c.story);
            QCOMPARE(b.layout, c.layout);
            QCOMPARE(b.image.size(), QSize(spotcard::kBedW, spotcard::kBedH));
            QVERIFY(!b.image.hasAlphaChannel());
            if (c.photo == land && c.layout != QLatin1String("inset"))
                QVERIFY2(mean(b.image) <= srcMean * c.dimMax + 2, qPrintable(c.layout));   // backdrops are dimmed by 0.42 / 0.55
        }
        QCOMPARE(spotcard::photoBed(QDir(kCards).filePath("exif6.jpg"), "story", "full").photoSize, QSize(800, 1200));
        for (const char* bad : {"tiny.jpg", "truncated.jpg", "make_fixtures.py", "missing.jpg"}) {
            bool threw = false;
            QString msg;
            try { spotcard::photoBed(QDir(kCards).filePath(QLatin1String(bad)), "story", "full"); }
            catch (const std::exception& e) { threw = true; msg = QString::fromUtf8(e.what()); }
            QVERIFY2(threw, bad);
            if (QLatin1String(bad) == QLatin1String("tiny.jpg")) QCOMPARE(msg, QStringLiteral("photo too small (50x50)"));
        }
        // an unreadable boxed picture: the story falls back to the plain photo layout, as _story_box returning None does
        spotcard::Trace t;
        const QJsonObject spec{{"kicker", "News"}, {"brand", "WSTR"}};
        const QImage f = spotcard::renderStory(spec, QJsonObject{{"headline", "Box picture gone"}}, true, "box", QDir(kCards).filePath("truncated.jpg"), &t);
        QCOMPARE(f.size(), QSize(1920, 1080));
        QVERIFY(t.box("storyBox").isEmpty());
        QVERIFY(!t.box("scrim").isEmpty());
    }

    // An animated (weather) scene: exit 64 with one unsupported line and nothing written, so the Supervisor runs the
    // script for the whole spot.
    void animatedScenesAreUnsupported() {
        const QString dir = tmp("anim");
        const QString spec = prepareScenes("scenes_anim.json", dir);
        const ExeRun r = runExe({"scenes", spec});
        QCOMPARE(r.code, 64);
        QCOMPARE(r.lines, 1);
        QVERIFY(r.json.value("unsupported").toBool());
        QCOMPARE(r.json.value("scene").toInt(), 1);
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{"scenes_anim.json"});
        const ExeRun c = runExe({"card", "--scenes", spec});
        QCOMPARE(c.code, 64);
        QCOMPARE(QDir(dir).entryList(QDir::Files), QStringList{"scenes_anim.json"});
    }

    // The command line: the script's argparse forms, its error strings, the 0 / 2 / 64 exit codes, one JSON line.
    void cliContract() {
        const QString dir = tmp("cli");
        QDir().mkpath(dir);
        const QString spec = QDir(kCards).filePath("card_small.json");
        auto expectFail = [](const ExeRun& r, int code, const QString& error) {
            QVERIFY2(r.code == code, r.out + r.err);
            QCOMPARE(r.lines, 1);
            QCOMPARE(r.json.value("ok").toBool(true), false);
            if (!error.isEmpty()) QVERIFY2(r.json.value("error").toString().contains(error), qPrintable(r.json.value("error").toString()));
        };
        expectFail(runExe({"card"}), 2, "--spec and --out are required (or --scenes)");
        expectFail(runExe({"card", "--spec", spec}), 2, "--spec and --out are required");
        expectFail(runExe({"card", "--spec", spec, "--out", dir + "/x.png", "--bogus", "1"}), 64, "unsupported: --bogus");
        expectFail(runExe({"card", "--spec", spec, "--out", dir + "/x.png", "stray"}), 64, "unsupported: stray");
        expectFail(runExe({"card", "--s", spec, "--out", dir + "/x.png"}), 2, "ambiguous option");
        expectFail(runExe({"card", "--spec"}), 2, "expected one argument");
        expectFail(runExe({"card", "--spec", dir + "/missing.json", "--out", dir + "/x.png"}), 2, "bad spec: ");
        QVERIFY(writeBytes(dir + "/broken.json", "{\"headline\": "));
        expectFail(runExe({"card", "--spec", dir + "/broken.json", "--out", dir + "/x.png"}), 2, "bad spec: ");
        QVERIFY(writeBytes(dir + "/badsize.json", "{\"headline\": \"x\", \"width\": \"wide\"}"));
        expectFail(runExe({"card", "--spec", dir + "/badsize.json", "--out", dir + "/x.png"}), 2, "invalid literal for int()");
        QVERIFY(!QFileInfo::exists(dir + "/x.png"));
        // prefixes and --opt=value, as argparse takes them; a BOM-prefixed spec (utf-8-sig); a .jpg-named bed is PNG
        QVERIFY(writeBytes(dir + "/bom.json", "\xef\xbb\xbf" + readAll(spec)));
        const ExeRun ok = runExe({"card", "--sp=" + dir + "/bom.json", "--o", dir + "/x.png", "--be", dir + "/bed.jpg"});
        QVERIFY2(ok.code == 0, ok.out + ok.err);
        QCOMPARE(QImage(dir + "/x.png").size(), QSize(1280, 720));
        QVERIFY(readAll(dir + "/bed.jpg").startsWith("\x89PNG"));
        // relative paths resolve against the working directory, as Path.resolve() does
        const ExeRun rel = runExe({"card", "--spec", spec, "--out", "rel/out.png"}, dir);
        QVERIFY2(rel.code == 0, rel.out + rel.err);
        QCOMPARE(rel.json.value("furniture").toString(), QDir::toNativeSeparators(QFileInfo(dir + "/rel/out.png").canonicalFilePath()));
        QCOMPARE(rel.json.value("bed").toString(), QString());
        // scenes: a missing key is the script's KeyError text; no file / options
        QVERIFY(writeJson(dir + "/nobed.json", QJsonObject{{"scenes", QJsonArray{QJsonObject{{"kind", "story"}, {"furniture", dir + "/nb.png"}}}}}));
        expectFail(runExe({"scenes", dir + "/nobed.json"}), 2, "'bedBase'");
        expectFail(runExe({"scenes"}), 2, "scenes.json is required");
        expectFail(runExe({"scenes", "--frames", "3"}), 64, "unsupported: --frames");
        const ExeRun sc = runExe({"scenes", "--scenes=" + prepareScenes("scenes_anim.json", dir + "/a")});
        QCOMPARE(sc.code, 64);
        // help and version
        const ExeRun help = runExe({"card", "--help"});
        QCOMPARE(help.code, 0);
        QVERIFY(help.out.contains("usage: PlayPlaySpotRender card"));
        const ExeRun v = runExe({"version"});
        QCOMPARE(v.code, 0);
        QVERIFY(toStrings(v.json.value("commands")).contains("card"));
        QVERIFY(toStrings(v.json.value("commands")).contains("scenes"));
        QVERIFY(toStrings(v.json.value("commands")).contains("music"));
    }

    // Against the script (Python + Pillow): the same stdout JSON (keys and values, paths swapped), every text drawn in
    // the same order and size within ±8 px, beds equal, furniture close.
    void layoutsVsScript() {
        if (m_py.isEmpty()) QSKIP(kNoPy);
        const QString py = tmp("vs/py"), cc = tmp("vs/cc");
        QDir().mkpath(py);
        QDir().mkpath(cc);
        struct Case { QString kind, name; QStringList pyArgs, ccArgs; };
        QList<Case> cases;
        for (const char* n : kCardSpecs) {
            const QString spec = QDir(kCards).filePath(QLatin1String(n)), b = QFileInfo(spec).completeBaseName();
            cases.append({"card", b, {"--spec", spec, "--out", py + "/" + b + ".png", "--bed", py + "/" + b + "_bed.png"},
                          {"--spec", spec, "--out", cc + "/" + b + ".png", "--bed", cc + "/" + b + "_bed.png"}});
        }
        for (const char* n : kSceneSpecs) {
            const QString b = QFileInfo(QLatin1String(n)).completeBaseName();
            cases.append({"scenes", b, {"--scenes", prepareScenes(QLatin1String(n), py)}, {"--scenes", prepareScenes(QLatin1String(n), cc)}});
        }
        QJsonArray pc;
        for (const Case& c : cases) pc.append(QJsonObject{{"args", QJsonArray::fromStringList(c.pyArgs)}});
        QVERIFY(writeJson(tmp("vs/cases.json"), pc));
        const PyRun pr = runPy(kPySpotCard, {scriptsDir(), tmp("vs/cases.json"), tmp("vs/py_out.json")}, 240000);
        QVERIFY2(pr.ok, pr.err.constData());
        const QJsonArray pyOut = readJson(tmp("vs/py_out.json")).array();
        QCOMPARE(pyOut.size(), cases.size());
        qInfo("the script: %lld ms for %d runs", pr.ms, int(cases.size()));
        int texts = 0, maxD = 0;
        for (int i = 0; i < cases.size(); ++i) {
            const Case& c = cases[i];
            const QJsonObject p = pyOut[i].toObject();
            QCOMPARE(p.value("code").toInt(), 0);
            const QJsonObject pj = ppcli::parseResult(p.value("stdout").toString().toUtf8());
            const ExeRun r = runExe(QStringList{"card"} + c.ccArgs);
            QVERIFY2(r.code == 0, r.out + r.err);
            QCOMPARE(QJsonValue(r.json), swapDir(pj, QDir::toNativeSeparators(py), QDir::toNativeSeparators(cc)));   // keys and values
            // traces in process (a third folder, same inputs)
            QList<spotcard::Trace> traces;
            if (c.kind == QLatin1String("card")) {
                spotcard::Trace t;
                QVERIFY(spotcard::runCard(c.ccArgs[1], tmp("vs/ip_" + c.name + ".png"), QString(), &t).value("ok").toBool());
                traces << t;
            } else {
                QVERIFY(spotcard::runScenes(prepareScenes(c.name + ".json", tmp("vs/ip_" + c.name)), &traces).value("ok").toBool());
            }
            const QJsonArray groups = p.value("texts").toArray();
            QCOMPARE(groups.size(), traces.size());
            for (int g = 0; g < groups.size(); ++g) {
                const QJsonArray want = groups[g].toArray();
                const QList<spotcard::TextItem>& got = traces[g].texts;
                const QString tag = QStringLiteral("%1 render %2").arg(c.name).arg(g);
                QStringList wantText, gotText;
                for (const QJsonValue& v : want) wantText << v.toObject().value("text").toString();
                for (const spotcard::TextItem& it : got) gotText << it.text;
                QVERIFY2(wantText == gotText, qPrintable(tag + ":\n  script: " + wantText.join(" | ") + "\n  native: " + gotText.join(" | ")));
                for (int k = 0; k < want.size(); ++k) {
                    const QJsonObject w = want[k].toObject();
                    const int dx = std::abs(w.value("x").toInt() - got[k].at.x()), dy = std::abs(w.value("y").toInt() - got[k].at.y());
                    maxD = std::max({maxD, dx, dy});
                    QVERIFY2(dx <= 8 && dy <= 8 && w.value("size").toInt() == got[k].size,
                             qPrintable(QStringLiteral("%1 '%2': script %3,%4 @%5, native %6,%7 @%8").arg(tag, got[k].text).arg(w.value("x").toInt()).arg(w.value("y").toInt())
                                            .arg(w.value("size").toInt()).arg(got[k].at.x()).arg(got[k].at.y()).arg(got[k].size)));
                    ++texts;
                }
            }
            // pictures: beds equal (photo beds go through Pillow's own resampling), furniture close over grey
            const QJsonArray pyScenes = c.kind == QLatin1String("card") ? QJsonArray{pj} : pj.value("scenes").toArray();
            const QJsonArray ccScenes = c.kind == QLatin1String("card") ? QJsonArray{r.json} : r.json.value("scenes").toArray();
            for (int k = 0; k < pyScenes.size(); ++k) {
                const QJsonObject a = pyScenes[k].toObject(), b = ccScenes[k].toObject();
                const QImage pf(a.value("furniture").toString()), cf(b.value("furniture").toString());
                const double fdb = psnrRgb(overGrey(pf), overGrey(cf));
                const double bdb = psnrRgb(QImage(a.value("bed").toString()), QImage(b.value("bed").toString()));
                qInfo("%s %d: furniture %.1f dB, bed %.1f dB", qPrintable(c.name), k, fdb, bdb);
                QVERIFY2(fdb >= 28.0, qPrintable(QStringLiteral("%1 %2 furniture %3 dB").arg(c.name).arg(k).arg(fdb)));
                QVERIFY2(bdb >= 45.0, qPrintable(QStringLiteral("%1 %2 bed %3 dB").arg(c.name).arg(k).arg(bdb)));
            }
        }
        qInfo("%d texts placed within %d px of the script's", texts, maxD);
    }
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--load-child")) {
            QCoreApplication app(argc, argv);                  // no platform plugin: only the image plugins matter here
            return loadChild();
        }
    QGuiApplication app(argc, argv);                           // fonts: the native platform, as the card commands will run
    TestInhouseCard tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "test_inhouse_card.moc"
