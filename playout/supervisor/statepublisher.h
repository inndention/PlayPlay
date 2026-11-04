#ifndef PLAYPLAY_STATEPUBLISHER_H
#define PLAYPLAY_STATEPUBLISHER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <functional>

/**
 * StatePublisher — the Supervisor's UI snapshot, built once per tick and sent only when it changed
 * (perf-supervisor PS-6 / W12).
 *
 * Before: every 2 s the Supervisor built fullSnapshot() and broadcast it to every UI and dashboard whether or not anything
 * had changed, and every status query of the app (every 4 s) got the snapshot plus every channel's last telemetry plus
 * both hours of every runlist. Now:
 *  - tick() (the 2 s timer) builds the snapshot once, when anyone listens, and hashes its stable view (stableView: the
 *    snapshot without its fast gauges, which have their own streams or only move a clock). It is published (the sink
 *    serializes it once per transport) when that hash changed, and otherwise every keepaliveMs (default 10 s; <= 0 =
 *    every tick, the old behaviour) so the gauges and a client that skipped a snapshot while behind still catch up.
 *  - queryReply() answers one client's MSG_UI_QUERY_STATUS: its first query gets the snapshot, the last telemetry and
 *    every runlist (as before); a repeated query gets the snapshot (the published one, identical to what the client
 *    has, when nothing meaningful changed since) and only the runlists that were not broadcast to it since.
 * The published object is exactly what the builder returned: the hash only decides whether to send.
 */
namespace statepub {

/// The snapshot as the change test sees it. Dropped: utcMs, uptimeMs (clocks), mux.muxBitrate (the mux pushes its own
/// status), mesh.lastOkMs, and per channel process.lastHeartbeatMs (moves with every heartbeat) and engine (the engine's
/// telemetry, broadcast at 1 Hz on its own); executor.driftSec counts in whole seconds (it jitters by a frame on every
/// telemetry; a real drift still shows). Everything else — states, armed, emergency, on-air item, pushes, profiles,
/// alerts, health, mux state — is compared as is. Pure.
QJsonObject stableView(const QJsonObject& snapshot);
/// SHA-1 of the compact stableView (keys are sorted, so equal content gives equal bytes).
QByteArray stableHash(const QJsonObject& snapshot);

enum class Verdict { Skip, First, Changed, Keepalive, Forced };
/// Whether a tick publishes. keepaliveMs <= 0: every tick. A keepalive within half a tick of due goes out now (timer
/// jitter must not make it a whole tick late). Pure, table-tested.
Verdict decide(bool publishedBefore, bool sameHash, qint64 sinceLastPublishMs, int keepaliveMs, int tickMs, bool forced);
const char* verdictName(Verdict v);

} // namespace statepub

class StatePublisher {
public:
    using Builder = std::function<QJsonObject()>;
    using Sink = std::function<void(const QJsonObject& snapshot)>;
    using Clock = std::function<qint64()>;                 // monotonic milliseconds (tests drive time with it)

    static constexpr int kDefaultTickMs = 2000;
    static constexpr int kDefaultKeepaliveMs = 10000;

    StatePublisher(Builder build, Sink publish);

    void setListenerCheck(std::function<bool()> anyoneListening) { m_listening = std::move(anyoneListening); }
    void setTickMs(int ms) { m_tickMs = ms > 0 ? ms : kDefaultTickMs; }
    int tickMs() const { return m_tickMs; }
    void setKeepaliveMs(int ms) { m_keepaliveMs = ms; }
    int keepaliveMs() const { return m_keepaliveMs; }
    void setClock(Clock now) { m_now = std::move(now); }

    /// One tick: nothing when nobody listens, else build once and publish on change / keepalive.
    statepub::Verdict tick();
    /// The next tick publishes whatever it builds.
    void forcePublish() { m_force = true; }
    QJsonObject lastPublished() const { return m_last; }

    /// A snapshot for one client. A client that has nothing yet gets a fresh build. Otherwise, when a fresh build has the
    /// published stable hash, the published object (byte-identical to what the client got) so it sees no change.
    QJsonObject replySnapshot(bool firstForClient);

    struct Reply {
        QJsonObject snapshot;
        bool telemetry = false;     // the client's first query: send every channel's last telemetry too
        QStringList runlists;       // channels whose runlist this client has not got since it last changed
    };
    /// MSG_UI_QUERY_STATUS from an IPC client; runlistChannels = the executor's channels.
    Reply queryReply(const QString& clientId, const QStringList& runlistChannels);

    // Per-client runlist bookkeeping (IPC UI clients).
    bool noteClient(const QString& clientId);              // true on the client's first query
    void forgetClient(const QString& clientId);
    /// A runlist for channelId was just sent to every client in `recipients`.
    void noteRunlistBroadcast(const QString& channelId, const QSet<QString>& recipients);
    /// The channels whose runlist `clientId` has not got since it last changed (all of them on its first query); they
    /// count as sent from here on.
    QStringList runlistsDue(const QString& clientId, const QStringList& channels);

    /// {tickMs, keepaliveMs, built, published, first, changed, keepalives, forced, skipped, idleTicks, replies,
    ///  repliesSame, runlistBroadcasts, runlistsSent, clients, lastPublishAgeMs, lastHash}
    QJsonObject stats() const;

private:
    Builder m_build;
    Sink m_publish;
    std::function<bool()> m_listening;
    Clock m_now;
    QElapsedTimer m_clock;
    int m_tickMs = kDefaultTickMs;
    int m_keepaliveMs = kDefaultKeepaliveMs;

    bool m_published = false, m_force = false;
    QByteArray m_publishedHash;
    qint64 m_publishedAtMs = 0;
    QJsonObject m_last;

    QHash<QString, quint64> m_runlistRev;                  // channel -> runlist broadcasts so far
    QHash<QString, QHash<QString, quint64>> m_clients;     // client -> channel -> runlist revision it has

    struct Counters { quint64 built = 0, published = 0, first = 0, changed = 0, keepalives = 0, forced = 0, skipped = 0,
                      idleTicks = 0, replies = 0, repliesSame = 0, runlistBroadcasts = 0, runlistsSent = 0; } m_n;

    qint64 nowMs() const;
    QJsonObject build();
};

#endif // PLAYPLAY_STATEPUBLISHER_H
