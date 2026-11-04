#include "provider/payload.h"

#include <QtEndian>
#include <cmath>
#include <cstring>

namespace studio::provider::payload {

namespace {

// "/videos/x" from "/videos/x", "https://openrouter.ai/api/v1/videos/x" or "http://127.0.0.1:9/api/v1/videos/x?a=1"
QStringView apiPath(QStringView p) {
    if (p.startsWith(u"http://", Qt::CaseInsensitive) || p.startsWith(u"https://", Qt::CaseInsensitive)) {
        const qsizetype slash = p.indexOf(u'/', p.indexOf(u"//") + 2);
        p = slash < 0 ? QStringView() : p.mid(slash);
        if (p.startsWith(u"/api/v1")) p = p.mid(7);
    }
    return p;
}

} // namespace

BodyCap bodyCap(QStringView path) {
    if (apiPath(path).startsWith(u"/videos")) return {kVideosMaxBody, kVideosSafeBody};
    return {kOpenRouterMaxBody, kSafeBody};
}

QString fmtMB(qint64 n) {
    // exact decimal rounding on integers (MB is a power of two, so n / MB is exact in binary and JS toFixed rounds a
    // tie up); a double and %.1f would round ties to even
    const bool tenth = n < 10 * kMB;
    const qint64 neg = n < 0 ? -1 : 1;
    const qint64 a = n < 0 ? -n : n;
    const qint64 scaled = tenth ? a * 10 : a;
    qint64 q = scaled / kMB;
    if ((scaled % kMB) * 2 >= kMB) ++q;
    const QString num = tenth ? QStringLiteral("%1.%2").arg(q / 10).arg(q % 10) : QString::number(q);
    return (neg < 0 && q ? QStringLiteral("-") : QString()) + num + QStringLiteral(" MB");
}

int fitBudget(const QList<qint64>& sizes, qint64 budget) {
    qint64 total = 0;
    for (qint64 s : sizes) total += s;
    int kept = int(sizes.size());
    while (kept > 0 && total > budget) total -= sizes[--kept];
    return kept;
}

std::optional<WavInfo> wavInfo(QByteArrayView b) {
    if (b.size() < 44) return std::nullopt;
    const auto* d = reinterpret_cast<const uchar*>(b.data());
    auto tag = [&](qint64 o, const char* t) { return std::memcmp(d + o, t, 4) == 0; };
    if (!tag(0, "RIFF") || !tag(8, "WAVE")) return std::nullopt;
    qint64 o = 12;
    std::optional<WavInfo> fmt;
    while (o + 8 <= b.size()) {
        const quint32 len = qFromLittleEndian<quint32>(d + o + 4);
        if (tag(o, "fmt ")) {
            if (o + 24 > b.size()) return std::nullopt;   // TS DataView would throw: not a WAV this can read
            WavInfo w;
            w.channels = qFromLittleEndian<quint16>(d + o + 10);
            w.sampleRate = int(qFromLittleEndian<quint32>(d + o + 12));
            w.bitsPerSample = qFromLittleEndian<quint16>(d + o + 22);
            fmt = w;
        }
        if (tag(o, "data") && fmt) {
            WavInfo w = *fmt;
            w.dataOffset = o + 8;
            w.dataBytes = std::min<qint64>(len, b.size() - o - 8);
            return w;
        }
        o += 8 + qint64(len) + (len & 1);
    }
    return std::nullopt;
}

namespace {

QByteArray wavHeader(qint64 dataBytes, const WavInfo& w) {
    QByteArray h(44, '\0');
    char* p = h.data();
    const int blockAlign = w.channels * w.bitsPerSample / 8;
    std::memcpy(p, "RIFF", 4);
    qToLittleEndian<quint32>(quint32(36 + dataBytes), p + 4);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, p + 16);
    qToLittleEndian<quint16>(1, p + 20);
    qToLittleEndian<quint16>(quint16(w.channels), p + 22);
    qToLittleEndian<quint32>(quint32(w.sampleRate), p + 24);
    qToLittleEndian<quint32>(quint32(w.sampleRate * blockAlign), p + 28);
    qToLittleEndian<quint16>(quint16(blockAlign), p + 32);
    qToLittleEndian<quint16>(quint16(w.bitsPerSample), p + 34);
    std::memcpy(p + 36, "data", 4);
    qToLittleEndian<quint32>(quint32(dataBytes), p + 40);
    return h;
}

} // namespace

QList<WavPiece> splitWav(const QByteArray& b, double seconds) {
    const auto w = wavInfo(b);
    const int blockAlign = w ? w->channels * w->bitsPerSample / 8 : 0;
    if (!w || blockAlign <= 0 || w->sampleRate <= 0) return {WavPiece{b, 0}};
    const qint64 bps = qint64(w->sampleRate) * blockAlign;
    const qint64 piece = std::max<qint64>(blockAlign, qint64(std::floor(seconds * double(bps) / blockAlign)) * blockAlign);
    if (w->dataBytes <= piece) return {WavPiece{b, 0}};
    QList<WavPiece> out;
    for (qint64 at = 0; at < w->dataBytes; at += piece) {
        const qint64 n = std::min(piece, w->dataBytes - at);
        QByteArray part = wavHeader(n, *w);
        part.append(b.constData() + w->dataOffset + at, n);
        out.push_back({part, double(at) / double(bps)});
    }
    return out;
}

} // namespace studio::provider::payload
