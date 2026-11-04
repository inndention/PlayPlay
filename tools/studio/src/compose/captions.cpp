#include "compose/captions.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace studio::compose {

namespace {

/// media.ts pad(n, w): String(Math.floor(n)).padStart(w, '0'), so "-1" stays "-1" at width 2 and becomes "0-1" at 3.
QString pad(double n, int w = 2) {
    QString s = num(std::floor(n));
    if (s.size() < w) s.prepend(QString(w - s.size(), QLatin1Char('0')));
    return s;
}

/// One cue's text for a sidecar: lines trimmed with their whitespace collapsed, control characters and blank lines
/// dropped, no "-->".
QString sidecarText(const QString& text) {
    QStringList lines;
    for (QString l : text.split(QRegularExpression(QStringLiteral("\\r\\n|[\\r\\n\\x{2028}\\x{2029}]")))) {
        l.removeIf([](QChar c) { return c.unicode() < 0x20 ? c != QLatin1Char('\t') : c.unicode() == 0x7F; });
        l = l.simplified();
        if (!l.isEmpty()) lines << l;
    }
    QString s = lines.join(QLatin1Char('\n'));
    while (s.contains(QLatin1String("-->"))) s.replace(QLatin1String("-->"), QLatin1String("->"));
    return s;
}

bool saveText(const QString& path, const QString& text, QString* error) {
    if (path.isEmpty()) return true;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("could not create the folder of %1").arg(path);
        return false;
    }
    QSaveFile f(path);
    const QByteArray data = text.toUtf8();
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error) *error = QStringLiteral("could not write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

} // namespace

QString subtitleTime(double sec, QChar sep) {
    // JS % is fmod (sign of the dividend; NaN for an infinite one)
    const double h = sec / 3600, m = std::fmod(sec, 3600) / 60, s = std::fmod(sec, 60);
    return pad(h) + QLatin1Char(':') + pad(m) + QLatin1Char(':') + pad(s) + sep + pad((s - std::floor(s)) * 1000, 3);
}

QString toSrt(const QVector<CaptionCue>& cues) {
    QStringList parts;
    for (qsizetype i = 0; i < cues.size(); ++i)
        parts << QString::number(i + 1) + QLatin1Char('\n') + subtitleTime(cues[i].start, QLatin1Char(',')) + QStringLiteral(" --> ")
                     + subtitleTime(cues[i].end, QLatin1Char(',')) + QLatin1Char('\n') + cues[i].text + QLatin1Char('\n');
    return parts.join(QLatin1Char('\n'));
}

QString toVtt(const QVector<CaptionCue>& cues) {
    // cue text is markup in WebVTT: a bare "<" would start a tag and hide the rest of the cue, so &, < and > are
    // escaped (SRT has no markup)
    QStringList parts;
    for (const CaptionCue& c : cues) {
        QString t = c.text;
        t.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;")).replace(QLatin1Char('>'), QLatin1String("&gt;"));
        parts << subtitleTime(c.start, QLatin1Char('.')) + QStringLiteral(" --> ") + subtitleTime(c.end, QLatin1Char('.')) + QLatin1Char('\n') + t + QLatin1Char('\n');
    }
    return QStringLiteral("WEBVTT\n\n") + parts.join(QLatin1Char('\n'));
}

QVector<CaptionCue> sidecarCues(const Timeline& t) {
    const double total = t.totalSeconds();
    QVector<CaptionCue> out;
    for (const CaptionCue& c : t.captions) {
        // the composer's rule for a drawn cue (filtergraph.cpp captionFilters): it ends after it starts, starts before
        // the end, and has text left once cleaned for drawing
        if (!(c.end > c.start) || c.start >= total || cleanText(c.text).isEmpty()) continue;
        const QString text = sidecarText(c.text);
        if (text.isEmpty()) continue;
        out.push_back({c.start, std::min(c.end, total), text});
    }
    std::stable_sort(out.begin(), out.end(), [](const CaptionCue& a, const CaptionCue& b) { return a.start < b.start; });
    return out;
}

QVector<CaptionCue> shiftCues(const QVector<CaptionCue>& cues, double offset, double from, double to) {
    QVector<CaptionCue> out;
    for (const CaptionCue& c : cues) {
        const double s = std::max(c.start + offset, from), e = std::min(c.end + offset, to);
        if (e > s && !c.text.isEmpty()) out.push_back({s, e, c.text});
    }
    return out;
}

bool writeSidecars(const QVector<CaptionCue>& cues, const QString& srtPath, const QString& vttPath, QString* error) {
    return saveText(srtPath, toSrt(cues), error) && saveText(vttPath, toVtt(cues), error);
}

} // namespace studio::compose
