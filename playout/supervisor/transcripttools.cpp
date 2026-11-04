#include "transcripttools.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace TranscriptTools {

QVector<Word> words(const QJsonObject& transcript) {
    QVector<Word> out;
    for (const QJsonValue& sv : transcript.value("segments").toArray()) {
        for (const QJsonValue& wv : sv.toObject().value("words").toArray()) {
            const QJsonObject o = wv.toObject();
            Word w;
            w.text = o.value("word").toString().trimmed();
            w.start = o.value("start").toDouble();
            w.end = std::max(w.start, o.value("end").toDouble());
            if (!w.text.isEmpty()) out.append(w);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Word& a, const Word& b) { return a.start < b.start; });
    return out;
}

double cutBefore(const QVector<Word>& w, int i, double maxPad) {
    if (i < 0 || i >= w.size()) return 0.0;
    const double gap = i > 0 ? w[i].start - w[i - 1].end : 2.0 * maxPad;
    return std::max(0.0, w[i].start - std::min(maxPad, std::max(0.0, gap / 2.0)));
}

double cutAfter(const QVector<Word>& w, int i, double maxPad) {
    if (i < 0 || i >= w.size()) return 0.0;
    const double gap = i + 1 < w.size() ? w[i + 1].start - w[i].end : 2.0 * maxPad;
    return w[i].end + std::min(maxPad, std::max(0.0, gap / 2.0));
}

QVector<double> cutPoints(const QVector<Word>& w, double maxPad) {
    QVector<double> p;
    p.reserve(w.size() * 2);
    for (int i = 0; i < w.size(); ++i) {
        p.append(cutBefore(w, i, maxPad));
        p.append(cutAfter(w, i, maxPad));
    }
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), p.end());
    return p;
}

// Joins the parts with spaces, stopping at a word boundary before maxChars (an ellipsis marks the cut).
static QString joinCapped(const QStringList& parts, int maxChars) {
    QString s;
    for (const QString& t : parts) {
        if (maxChars > 0 && s.size() + 1 + t.size() > maxChars) {
            if (s.isEmpty()) s = t.left(maxChars);
            s += QChar(0x2026);
            break;
        }
        if (!s.isEmpty()) s += QLatin1Char(' ');
        s += t;
    }
    return s;
}

QString textBetween(const QVector<Word>& w, double a, double b, int maxChars) {
    QStringList parts;
    for (const Word& x : w) {
        const double mid = (x.start + x.end) / 2.0;
        if (mid >= a && mid < b) parts << x.text;
    }
    return joinCapped(parts, maxChars);
}

QString sentenceTextBetween(const QJsonObject& transcript, double a, double b, int maxChars) {
    QStringList parts;
    for (const QJsonValue& sv : transcript.value("segments").toArray()) {
        const QJsonObject s = sv.toObject();
        const double mid = (s.value("start").toDouble() + s.value("end").toDouble()) / 2.0;
        const QString t = s.value("text").toString().trimmed();
        if (!t.isEmpty() && mid >= a && mid < b) parts << t;
    }
    return joinCapped(parts, maxChars);
}

int fillSegmentText(QJsonArray& segments, const QJsonObject& transcript, int maxChars) {
    const QVector<Word> w = words(transcript);
    bool anySentence = false;
    for (const QJsonValue& sv : transcript.value("segments").toArray())
        if (!sv.toObject().value("text").toString().trimmed().isEmpty()) { anySentence = true; break; }
    if (w.isEmpty() && !anySentence) return 0;     // nothing spoken (music, or an empty file): keep what is there
    int n = 0;
    for (int i = 0; i < segments.size(); ++i) {
        QJsonObject s = segments[i].toObject();
        const double a = s.value("startTime").toDouble(), b = s.value("endTime").toDouble();
        const QString t = w.isEmpty() ? sentenceTextBetween(transcript, a, b, maxChars) : textBetween(w, a, b, maxChars);
        if (t.isEmpty()) s.remove("text");
        else { s["text"] = t; ++n; }
        segments[i] = s;
    }
    return n;
}

}   // namespace TranscriptTools
