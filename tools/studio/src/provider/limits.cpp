#include "provider/limits.h"

#include "core/datafiles.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <cmath>
#include <mutex>
#include <vector>

namespace studio::provider::limits {

namespace {

struct Data {
    QHash<QString, QJsonObject> byId;
    QJsonObject platform;
    QHash<QString, QString> retired;
    std::vector<std::pair<QRegularExpression, qint64>> requestCaps;
    QRegularExpression rejectsVerbose;
    QString error;
};

const Data& data() {
    static const Data d = [] {
        Data x;
        const QJsonObject f = studio::data::load(u"model-limits", &x.error);
        for (const QJsonValue& v : f.value(QLatin1StringView("models")).toArray()) {
            const QJsonObject e = v.toObject();
            const QString id = e.value(QLatin1StringView("id")).toString();
            if (!id.isEmpty()) x.byId.insert(id, e);   // the last wins, as in new Map(entries) (ids are unique anyway)
        }
        x.platform = QJsonObject{{QStringLiteral("platform"), f.value(QLatin1StringView("platform"))}};
        const QJsonObject r = f.value(QLatin1StringView("retired")).toObject();
        for (auto it = r.begin(); it != r.end(); ++it) x.retired.insert(it.key(), it.value().toString());
        for (const QJsonValue& v : f.value(QLatin1StringView("requestCaps")).toArray()) {
            const QJsonObject c = v.toObject();
            QString err;
            QRegularExpression re = studio::data::regex(c.value(QLatin1StringView("pattern")), &err);
            const qint64 bytes = qint64(c.value(QLatin1StringView("bytes")).toDouble());
            if (re.isValid() && bytes > 0) x.requestCaps.emplace_back(std::move(re), bytes);
            else if (x.error.isEmpty()) x.error = QStringLiteral("requestCaps: ") + err;
        }
        // rejectsVerboseJson in openrouter.ts, verbatim
        x.rejectsVerbose = studio::data::regex(u"reject\\w*\\s+(?:response_format\\s*=\\s*)?verbose_json|json\\/text only", u"i");
        return x;
    }();
    return d;
}

// ECMAScript WhiteSpace + LineTerminator (String.prototype.trim)
bool jsSpace(char16_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000
        || c == 0xFEFF;
}
QString jsTrim(QStringView s) {
    qsizetype a = 0, b = s.size();
    while (a < b && jsSpace(s[a].unicode())) ++a;
    while (b > a && jsSpace(s[b - 1].unicode())) --b;
    return s.mid(a, b - a).toString();
}

// a researched number limit, 0 when absent / null / not a number (the TS `n && ...` falsy cases)
double numberLimit(QStringView id, QLatin1StringView key) {
    const QJsonValue v = researched(id).value(QLatin1StringView("limits")).toObject().value(key);
    return v.isDouble() && std::isfinite(v.toDouble()) ? v.toDouble() : 0;
}

} // namespace

QJsonObject researched(QStringView id) {
    if (id.isEmpty()) return {};
    const Data& d = data();
    const QString key = id.toString();
    if (auto it = d.byId.constFind(key); it != d.byId.constEnd()) return *it;
    if (key.endsWith(QLatin1StringView(":free"))) {
        if (auto it = d.byId.constFind(key.chopped(5)); it != d.byId.constEnd()) return *it;
    }
    return {};
}

QJsonObject platformLimits() { return data().platform; }

QJsonObject limitsFor(QStringView id, const QJsonObject& catalog) {
    const QJsonObject r = researched(id);
    QJsonObject out = r.value(QLatin1StringView("limits")).toObject();
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        const QJsonValue v = it.value();
        if (v.isNull() || v.isUndefined() || (v.isString() && v.toString().isEmpty())) continue;
        out.insert(it.key(), v);
    }
    out.insert(QStringLiteral("sources"), r.isEmpty() ? QJsonArray() : r.value(QLatin1StringView("sources")).toArray());
    out.insert(QStringLiteral("confidence"), r.isEmpty() ? QStringLiteral("api") : r.value(QLatin1StringView("confidence")).toString());
    return out;
}

payload::ImageLimit imageLimitFor(QStringView id, bool firstFrame) {
    payload::ImageLimit base = firstFrame ? payload::kFirstFrame : payload::kReferenceImage;
    const double mb = numberLimit(id, QLatin1StringView("maxImageMB"));
    if (mb > 0) base.maxBytes = std::min<qint64>(base.maxBytes, qint64(std::floor(mb * double(payload::kMB) * 0.9)));
    return base;
}

std::optional<qint64> requestCapFor(QStringView id) {
    if (id.isEmpty()) return std::nullopt;
    for (const auto& [re, bytes] : data().requestCaps)
        if (re.matchView(id).hasMatch()) return bytes;
    return std::nullopt;
}

QString retiredReason(QStringView id) {
    const Data& d = data();
    const QString key = id.toString();
    if (auto it = d.retired.constFind(key); it != d.retired.constEnd()) return *it;
    if (key.endsWith(QLatin1StringView(":free"))) return d.retired.value(key.chopped(5));
    return {};
}

VideoFit videoRules(QStringView id, VideoFit fit, const QList<int>& durations, const QStringList& resolutions) {
    VideoFit out = fit;
    if (id.startsWith(u"google/veo-3.1") && out.resolution != u"720p" && durations.contains(8)) out.seconds = 8;
    if (id.startsWith(u"minimax/hailuo-2.3") && out.resolution.contains(u"1080") && out.seconds != 6) {
        if (durations.contains(6)) {
            out.seconds = 6;
        } else {
            for (const QString& r : resolutions)
                if (r.contains(u"768") || r.contains(u"720")) { out.resolution = r; break; }
        }
    }
    return out;
}

int ttsCharsFor(QStringView id, int fallback) {
    const double n = numberLimit(id, QLatin1StringView("maxTextChars"));
    return n > 50 ? std::min(fallback, int(std::floor(n * 0.95))) : fallback;
}

std::optional<int> promptCharsFor(QStringView id) {
    const double n = numberLimit(id, QLatin1StringView("maxPromptChars"));
    if (n > 50) return int(n);
    return std::nullopt;
}

QString fitPrompt(const QString& prompt, QStringView id) {
    const auto max = promptCharsFor(id);
    if (!max || prompt.size() <= *max) return prompt;
    const QString cut = prompt.left(*max);
    const qsizetype at = std::max(cut.lastIndexOf(QLatin1StringView(". ")), cut.lastIndexOf(QLatin1StringView("; ")));
    if (at > *max * 0.6) return jsTrim(QStringView(cut).left(at + 1));
    const qsizetype sp = cut.lastIndexOf(QLatin1Char(' '));
    return jsTrim(QStringView(cut).left(sp > 0 ? sp : *max));
}

SttLimits sttFor(QStringView id) {
    const double minutes = numberLimit(id, QLatin1StringView("maxAudioMinutes"));
    const double mb = numberLimit(id, QLatin1StringView("maxAudioMB"));
    // 16 kHz mono WAV = 32,000 bytes a second
    const double byMb = mb > 0 ? std::floor(mb * double(payload::kMB) * 0.9 / 32000) : INFINITY;
    const double byMin = minutes > 0 ? std::floor(minutes * 60 * 0.95) : INFINITY;
    SttLimits s;
    s.pieceSeconds = int(std::max(30.0, std::min({double(payload::kSttPieceSeconds), byMb, byMin})));
    s.maxSingleBytes = mb > 0 ? std::min<qint64>(payload::kSttMaxSingle, qint64(std::floor(mb * double(payload::kMB) * 0.95)))
                              : payload::kSttMaxSingle;
    return s;
}

bool rejectsVerboseJson(QStringView id) {
    const QJsonObject e = researched(id);
    if (e.isEmpty()) return false;
    auto text = [](const QJsonValue& v) { return v.isString() ? v.toString() : QString(); };   // `?? ''` on null / absent
    const QString words = text(e.value(QLatin1StringView("notes"))) + QLatin1Char(' ')
        + text(e.value(QLatin1StringView("limits")).toObject().value(QLatin1StringView("other")));
    return data().rejectsVerbose.isValid() && data().rejectsVerbose.matchView(words).hasMatch();
}

QString loadError() { return data().error; }

} // namespace studio::provider::limits
