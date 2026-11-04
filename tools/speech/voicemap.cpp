#include "voicemap.h"

#include <cmath>
#include <functional>

namespace voicemap {

bool isPySpace(char32_t c) {
    // the 29 code points CPython 3.11's str.isspace() accepts (bidi WS/B/S or category Zs); re's \s uses the same table
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

QString pyStrip(const QString& s) {
    // every Python whitespace character is in the BMP, so checking UTF-16 units never splits a surrogate pair
    qsizetype a = 0, b = s.size();
    while (a < b && isPySpace(s[a].unicode())) ++a;
    while (b > a && isPySpace(s[b - 1].unicode())) --b;
    return s.mid(a, b - a);
}

QString sapiVoiceFor(const QString& voice, const QStringList& installed) {
    const QString v = pyStrip(voice);
    if (v.isEmpty()) return {};
    const QString low = v.toLower();
    for (const QString& name : installed)
        if (name.toLower() == low) return name;                 // a real SAPI name passes through, any case
    auto starts = [&](std::initializer_list<const char*> pre) {
        for (const char* p : pre) if (low.startsWith(QLatin1String(p))) return true;
        return false;
    };
    const bool female = starts({"af_", "bf_", "ef_", "ff_", "f"}) && !low.startsWith(QLatin1String("fm"));
    const bool male = starts({"am_", "bm_", "em_", "fm_", "m"});
    const bool british = starts({"bf_", "bm_"});
    auto pick = [&](const std::function<bool(const QString&)>& pred) {
        for (const QString& name : installed) if (pred(name.toLower())) return name;
        return QString();
    };
    auto has = [](const QString& n, const char* s) { return n.contains(QLatin1String(s)); };
    QString r;
    if (female) {
        if (british) r = pick([&](const QString& n) { return has(n, "hazel"); });
        if (r.isEmpty()) r = pick([&](const QString& n) { return has(n, "zira"); });
        if (r.isEmpty()) r = pick([&](const QString& n) { return has(n, "eva") || has(n, "aria") || has(n, "jenny"); });
        return r;
    }
    if (male) {
        if (british) r = pick([&](const QString& n) { return has(n, "george"); });
        if (r.isEmpty()) r = pick([&](const QString& n) { return has(n, "david"); });
        if (r.isEmpty()) r = pick([&](const QString& n) { return has(n, "mark") || has(n, "guy"); });
        return r;
    }
    return {};
}

const QStringList& kokoroVoices() {
    static const QStringList v = {"af", "af_bella", "af_nicole", "af_sarah", "af_sky", "am_adam", "am_michael",
                                  "bf_emma", "bf_isabella", "bm_george", "bm_lewis"};
    return v;
}

int kokoroSpeakerId(const QString& name) { return int(kokoroVoices().indexOf(name.trimmed().toLower())); }

QString kokoroPickVoice(const QString& want) {
    const QString w = want.trimmed().toLower();                 // JS String.trim() / toLowerCase()
    if (kokoroSpeakerId(w) >= 0) return w;
    if (w.startsWith(QLatin1String("af"))) return QStringLiteral("af_bella");
    if (w.startsWith(QLatin1String("am"))) return QStringLiteral("am_michael");
    if (w.startsWith(QLatin1String("bf"))) return QStringLiteral("bf_emma");
    if (w.startsWith(QLatin1String("bm"))) return QStringLiteral("bm_george");
    return QStringLiteral("am_michael");
}

double clampSpeed(double speed) {
    if (speed == 0.0) speed = 1.0;                              // `a.speed or 1.0`: 0.0 (and -0.0) is falsy, NaN is not
    const double lo = std::isnan(speed) ? 2.0 : (2.0 < speed ? 2.0 : speed);   // min(2.0, x) keeps 2.0 unless x < 2.0
    return 0.5 < lo ? lo : 0.5;                                 // max(0.5, y)
}

int sapiRate(double speed) {
    const double r = std::nearbyint((speed - 1.0) * 10.0);     // round-half-even like Python's round()
    return int(r < -10.0 ? -10.0 : (r > 10.0 ? 10.0 : r));
}

}   // namespace voicemap
