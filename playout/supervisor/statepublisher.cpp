#include "statepublisher.h"
#include "../core/loopwatchdog.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

namespace statepub {

namespace {
// Drops `key` from the object under `parent` (when there is one) without touching anything else.
void dropNested(QJsonObject& o, const QString& parent, const QString& key) {
    auto it = o.find(parent);
    if (it == o.end() || !it.value().isObject()) return;
    QJsonObject inner = it.value().toObject();
    if (!inner.contains(key)) return;
    inner.remove(key);
    it.value() = inner;
}
} // namespace

QJsonObject stableView(const QJsonObject& snapshot) {
    QJsonObject v = snapshot;                               // implicitly shared: only the parts edited below are copied
    v.remove(QStringLiteral("utcMs"));
    v.remove(QStringLiteral("uptimeMs"));
    dropNested(v, QStringLiteral("mux"), QStringLiteral("muxBitrate"));
    dropNested(v, QStringLiteral("mesh"), QStringLiteral("lastOkMs"));
    auto ch = v.find(QStringLiteral("channels"));
    if (ch != v.end() && ch.value().isArray()) {
        QJsonArray out;
        const QJsonArray in = ch.value().toArray();
        for (const QJsonValue& cv : in) {
            if (!cv.isObject()) { out.append(cv); continue; }
            QJsonObject c = cv.toObject();
            c.remove(QStringLiteral("engine"));
            dropNested(c, QStringLiteral("process"), QStringLiteral("lastHeartbeatMs"));
            auto ex = c.find(QStringLiteral("executor"));
            if (ex != c.end() && ex.value().isObject()) {
                QJsonObject e = ex.value().toObject();
                // Whole seconds as an integer: qRound64 never yields -0, which would serialize differently from 0.
                if (e.contains(QStringLiteral("driftSec"))) e[QStringLiteral("driftSec")] = qint64(qRound64(e.value(QStringLiteral("driftSec")).toDouble()));
                ex.value() = e;
            }
            out.append(c);
        }
        ch.value() = out;
    }
    return v;
}

QByteArray stableHash(const QJsonObject& snapshot) {
    return QCryptographicHash::hash(QJsonDocument(stableView(snapshot)).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
}

Verdict decide(bool publishedBefore, bool sameHash, qint64 sinceLastPublishMs, int keepaliveMs, int tickMs, bool forced) {
    if (!publishedBefore) return Verdict::First;
    if (forced) return Verdict::Forced;
    if (!sameHash) return Verdict::Changed;
    if (keepaliveMs <= 0) return Verdict::Keepalive;        // every tick: the behaviour before PS-6
    if (sinceLastPublishMs >= qint64(keepaliveMs) - tickMs / 2) return Verdict::Keepalive;
    return Verdict::Skip;
}

const char* verdictName(Verdict v) {
    switch (v) {
    case Verdict::Skip: return "skip";
    case Verdict::First: return "first";
    case Verdict::Changed: return "changed";
    case Verdict::Keepalive: return "keepalive";
    case Verdict::Forced: return "forced";
    }
    return "?";
}

} // namespace statepub

StatePublisher::StatePublisher(Builder build, Sink publish) : m_build(std::move(build)), m_publish(std::move(publish)) {
    m_clock.start();
}

qint64 StatePublisher::nowMs() const { return m_now ? m_now() : m_clock.elapsed(); }

QJsonObject StatePublisher::build() {
    ++m_n.built;
    return m_build ? m_build() : QJsonObject();
}

statepub::Verdict StatePublisher::tick() {
    using statepub::Verdict;
    if (m_listening && !m_listening()) { ++m_n.idleTicks; return Verdict::Skip; }
    WatchdogScope tag("ui-snapshot");                      // STB-4: a slow tick names itself in the stall line
    const QJsonObject snap = build();
    const QByteArray h = statepub::stableHash(snap);
    const qint64 now = nowMs();
    const Verdict v = statepub::decide(m_published, h == m_publishedHash, now - m_publishedAtMs, m_keepaliveMs, m_tickMs, m_force);
    switch (v) {
    case Verdict::Skip: ++m_n.skipped; return v;
    case Verdict::First: ++m_n.first; break;
    case Verdict::Changed: ++m_n.changed; break;
    case Verdict::Keepalive: ++m_n.keepalives; break;
    case Verdict::Forced: ++m_n.forced; break;
    }
    m_force = false;
    m_published = true; m_publishedHash = h; m_publishedAtMs = now; m_last = snap;
    ++m_n.published;
    if (m_publish) m_publish(snap);
    return v;
}

QJsonObject StatePublisher::replySnapshot(bool firstForClient) {
    ++m_n.replies;
    QJsonObject snap = build();
    if (!firstForClient && m_published && statepub::stableHash(snap) == m_publishedHash) { ++m_n.repliesSame; return m_last; }
    return snap;
}

StatePublisher::Reply StatePublisher::queryReply(const QString& clientId, const QStringList& runlistChannels) {
    Reply r;
    const bool first = noteClient(clientId);
    r.snapshot = replySnapshot(first);
    r.telemetry = first;                                     // afterwards the engines' 1 Hz telemetry broadcast keeps it current
    r.runlists = runlistsDue(clientId, runlistChannels);
    return r;
}

bool StatePublisher::noteClient(const QString& clientId) {
    if (m_clients.contains(clientId)) return false;
    m_clients.insert(clientId, {});
    return true;
}

void StatePublisher::forgetClient(const QString& clientId) { m_clients.remove(clientId); }

void StatePublisher::noteRunlistBroadcast(const QString& channelId, const QSet<QString>& recipients) {
    const quint64 rev = ++m_runlistRev[channelId];
    ++m_n.runlistBroadcasts;
    for (const QString& id : recipients) {
        auto it = m_clients.find(id);
        if (it != m_clients.end()) it->insert(channelId, rev);
    }
}

QStringList StatePublisher::runlistsDue(const QString& clientId, const QStringList& channels) {
    QHash<QString, quint64>& has = m_clients[clientId];
    QStringList due;
    for (const QString& ch : channels) {
        const quint64 rev = m_runlistRev.value(ch, 0);
        auto it = has.find(ch);
        if (it != has.end() && *it == rev) continue;
        has.insert(ch, rev);
        due << ch;
    }
    m_n.runlistsSent += quint64(due.size());
    return due;
}

QJsonObject StatePublisher::stats() const {
    QJsonObject o;
    o["tickMs"] = m_tickMs; o["keepaliveMs"] = m_keepaliveMs;
    o["built"] = double(m_n.built); o["published"] = double(m_n.published); o["first"] = double(m_n.first);
    o["changed"] = double(m_n.changed); o["keepalives"] = double(m_n.keepalives); o["forced"] = double(m_n.forced);
    o["skipped"] = double(m_n.skipped); o["idleTicks"] = double(m_n.idleTicks);
    o["replies"] = double(m_n.replies); o["repliesSame"] = double(m_n.repliesSame);
    o["runlistBroadcasts"] = double(m_n.runlistBroadcasts); o["runlistsSent"] = double(m_n.runlistsSent);
    o["clients"] = int(m_clients.size());
    o["lastPublishAgeMs"] = m_published ? double(nowMs() - m_publishedAtMs) : -1.0;
    o["lastHash"] = QString::fromLatin1(m_publishedHash.toHex().left(12));
    return o;
}
