#pragma once
// Live-streaming helpers shared by the Supervisor and the engine (header-only, Qt Core only).
//
//  * redactUrl:  rtmp/rtmps URLs carry the stream key as their last path segment; logs, errors and telemetry show it masked.
//  * hydrate:    an output definition with `keyRef` ("live.facebook") is stored WITHOUT its key, so channel JSON, REST replies,
//                session bundles and the database never hold it. The Supervisor puts the key back (server + "/" + key) only
//                in the message that goes to the engine. No key saved = the output stays disabled.
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace livestream {

inline QString redactUrl(const QString& url) {
    const QString low = url.toLower();
    if (!low.startsWith("rtmp://") && !low.startsWith("rtmps://") && !low.startsWith("rtmpt://")) return url;
    QString base = url, query; const int q = base.indexOf('?'); if (q >= 0) { query = "?…"; base = base.left(q); }
    const int slash = base.lastIndexOf('/');
    const int schemeEnd = base.indexOf("://") + 3;
    if (slash < schemeEnd) return url;                         // rtmp://host without an app / key
    const QString key = base.mid(slash + 1);
    if (key.isEmpty()) return url;
    return base.left(slash + 1) + (key.size() > 8 ? QString("…") + key.right(3) : QString("…")) + query;
}

/// The stream-key settings that back a `keyRef` ("live.facebook" -> settings JSON {serverUrl, streamKey, ...}).
inline QString joinKeyUrl(const QString& server, const QString& key) {
    QString s = server.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s + "/" + key.trimmed();
}

/// Resolve every output that has a `keyRef` using `lookup(keyRef)`. Others pass through untouched.
template <typename Lookup>
QJsonArray hydrate(const QJsonArray& outs, Lookup lookup) {
    QJsonArray r;
    for (const QJsonValue& v : outs) {
        QJsonObject o = v.toObject();
        const QString ref = o.value("keyRef").toString();
        if (!ref.isEmpty()) {
            const QJsonObject cfg = lookup(ref);
            const QString key = cfg.value("streamKey").toString().trimmed();
            const QString server = o.value("url").toString(cfg.value("serverUrl").toString());
            if (key.isEmpty() || server.isEmpty()) o["enabled"] = false;      // nothing to publish to yet
            else o["url"] = joinKeyUrl(server, key);
        }
        r.append(o);
    }
    return r;
}

} // namespace livestream
