#include "safety/redact.h"

#include "core/json.h"
#include "core/log.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace studio::safety {
namespace {

QJsonValue redactAt(const QJsonValue& v, const RedactOptions& o, int depth) {
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
    case QJsonValue::Bool:
    case QJsonValue::Double: return v;
    case QJsonValue::String: return truncate(redactText(v.toString(), o.extraSecrets), o.maxString);
    default: break;
    }
    if (depth > 6) return QStringLiteral("[nested too deep]");
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        QJsonArray out;
        for (qsizetype i = 0; i < a.size() && i < 50; ++i) out.append(redactAt(a.at(i), o, depth + 1));
        if (a.size() > 50) out.append(QStringLiteral("[%1 more items]").arg(a.size() - 50));
        return out;
    }
    const QJsonObject obj = v.toObject();
    QJsonObject out;
    int n = 0;
    for (auto it = obj.begin(); it != obj.end() && n < 80; ++it, ++n) {
        const QJsonValue x = it.value();
        const bool blank = x.isNull() || x.isUndefined() || x.isBool() || (x.isString() && x.toString().isEmpty());
        out.insert(it.key(), log::isSecretKey(it.key()) && !blank ? QJsonValue(kRedacted.toString()) : redactAt(x, o, depth + 1));
    }
    if (obj.size() > 80) out.insert(QStringLiteral("…"), QStringLiteral("%1 more fields").arg(obj.size() - 80));
    return out;
}

} // namespace

QString redactText(const QString& s, const QStringList& extraSecrets) {
    QString t = s;
    for (const QString& sec : extraSecrets)
        if (sec.size() >= 8) t.replace(sec, kRedacted.toString());
    return log::scrub(t);
}

QString truncate(const QString& s, int max) {
    if (max < 0 || s.size() <= max) return s;
    return s.left(max) + QStringLiteral("…[truncated %1 chars, sha256:%2]")
                             .arg(s.size() - max)
                             .arg(QString::fromLatin1(json::sha256Hex(s.toUtf8()).left(12)));
}

QJsonValue redactValue(const QJsonValue& v, const RedactOptions& o) { return redactAt(v, o, 0); }

QJsonObject redactObject(const QJsonObject& o, const RedactOptions& opt) { return redactAt(o, opt, 0).toObject(); }

QString redactBinary(const QByteArray& b) { return QStringLiteral("[binary %1 bytes]").arg(b.size()); }

QList<QPair<QByteArray, QByteArray>> redactHeaders(const QList<QPair<QByteArray, QByteArray>>& headers) {
    QList<QPair<QByteArray, QByteArray>> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        const QString name = QString::fromLatin1(k);
        const bool secret = log::isSecretKey(name) || name.compare(u"x-api-key", Qt::CaseInsensitive) == 0 ||
                            name.compare(u"proxy-authorization", Qt::CaseInsensitive) == 0 || name.compare(u"set-cookie", Qt::CaseInsensitive) == 0;
        out.append({k, secret && !v.isEmpty() ? kRedacted.toString().toLatin1() : redactText(QString::fromUtf8(v)).toUtf8()});
    }
    return out;
}

QString forLog(const QJsonObject& body, int maxString) {
    RedactOptions o;
    o.maxString = maxString;
    return QString::fromUtf8(QJsonDocument(redactObject(body, o)).toJson(QJsonDocument::Compact));
}

} // namespace studio::safety
