#include "core/ids.h"

#include "core/clock.h"
#include "core/json.h"

#include <QJsonArray>
#include <QRandomGenerator>

namespace studio::ids {

QString randomHex(int bytes) {
    bytes = qBound(1, bytes, 64);
    QByteArray b(bytes, Qt::Uninitialized);
    QRandomGenerator* g = QRandomGenerator::system();   // OS entropy (BCryptGenRandom on Windows), thread-safe
    for (int i = 0; i < bytes; i += 4) {
        const quint32 v = g->generate();
        for (int k = 0; k < 4 && i + k < bytes; ++k) b[i + k] = char((v >> (8 * k)) & 0xFF);
    }
    return QString::fromLatin1(b.toHex());
}

namespace {
QString safePrefix(QStringView prefix) {
    if (isSafe(prefix)) return prefix.toString();
    Q_ASSERT_X(false, "ids", "unsafe id prefix");
    return QStringLiteral("id");
}
} // namespace

QString random(QStringView prefix, int bytes) { return safePrefix(prefix) + QLatin1Char('_') + randomHex(bytes); }

QString timeOrdered(QStringView prefix, const Clock& clock, int bytes) {
    const QString stamp = clock.now().toString(QStringLiteral("yyyyMMddHHmmss"));   // now() is UTC
    return safePrefix(prefix) + QLatin1Char('_') + stamp + QLatin1Char('_') + randomHex(bytes);
}

QString approvalToken() { return QStringLiteral("apv_") + randomHex(12); }

QString idemKey(QStringView nodeId, QStringView inputHash, int attempt) {
    const QJsonArray parts{nodeId.toString(), inputHash.toString(), attempt};
    return QString::fromLatin1(json::stableHash(parts));
}

bool isSafe(QStringView id) {
    if (id.isEmpty() || id.size() > 128) return false;
    for (qsizetype i = 0; i < id.size(); ++i) {
        const char16_t c = id[i].unicode();
        const bool alnum = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9');
        if (i == 0 && !alnum) return false;
        if (!alnum && c != u'_' && c != u'-' && c != u'.') return false;
    }
    if (id.contains(u"..") || id.endsWith(u'.')) return false;   // Windows drops a trailing dot
    // Reserved device names (Microsoft "Naming Files, Paths, and Namespaces"), also with an extension ("nul.txt" opens
    // the device). The superscript COM¹ / LPT² forms are already refused as non-ASCII above.
    const QStringView stem = id.left(id.indexOf(u'.') < 0 ? id.size() : id.indexOf(u'.'));
    static const char* const kDevices[] = {"con", "prn", "aux", "nul", "com0", "com1", "com2", "com3", "com4", "com5",
                                           "com6", "com7", "com8", "com9", "lpt0", "lpt1", "lpt2", "lpt3", "lpt4",
                                           "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* d : kDevices)
        if (stem.compare(QLatin1StringView(d), Qt::CaseInsensitive) == 0) return false;
    return true;
}

} // namespace studio::ids
