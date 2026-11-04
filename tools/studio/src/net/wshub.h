#pragma once
// WebSocket event hub (ST-04, docs/plans/starmedia-studio.md §4.9): the Studio's live event stream on its own port
// (7761; tests ephemeral). Clients connect to ws://127.0.0.1:<port>/ws[?since=<seq>&epoch=<e>][&project=<id>]
// [&types=job,run] with the token (Authorization / X-Studio-Token header, or ?token= from a browser).
//
// Every event has a seq. A client that reconnects with ?since=<last seq it saw> first gets every later event it missed
// (oldest first), then the live stream, with no gap and no duplicate:
//   - with a store attached (attachStore), publish() appends the event to the `events` table and broadcasts it from the
//     commit callback: the seq is the table's, a broadcast event is always on disk, and replay reads the table, so it
//     survives a restart. Callbacks arrive in commit order; a per-client high-water mark drops the one overlap there can
//     be (an event committed, replayed from the table, and then broadcast by its callback);
//   - without a store (the service before ST-14 wires the database, tests) seqs count from 1 in memory and replay
//     comes from the in-memory ring only.
// Recent events are kept in a ring, so most reconnects never touch the database.
//
// The first message is always {"type":"hello", lastSeq, epoch, since, replay: <events that follow>, mode} ; when the
// client cannot be brought up to date it says "reset": "epoch" (the client's seqs are of another stream), "ahead"
// (since is newer than anything here: a new database), "gap" (events since then are gone) or "too_far" (more than
// maxReplay behind), nothing is replayed, and the client must reload its state over REST.
// Epoch: a seq means something only within one numbering. Memory mode numbers from 1 again in every process, and a
// recreated studio.db numbers from 1 again too, so a bare ?since=N could replay unrelated events as if they continued
// the client's stream. Clients keep the hello's `epoch` with their last seq and reconnect with ?since=N&epoch=<it>; an
// epoch that differs gives reset "epoch". Memory mode: a random id per hub. Store mode: derived from the table's first
// event row (seq, time, type, project), so it survives a restart over the same studio.db and changes with a new one
// (empty until the first event; future pruning of `events` must keep the first row or persist the epoch). A client
// that sends no epoch (or an empty one) is replayed as before.
// Events: {"seq", "ts", "type", "projectId", "data"}. A project filter also passes events without a project (budget,
// gates). Client messages: {"op":"ping"} -> {"type":"pong", ts, lastSeq, epoch}.
//
// The handshake is checked before any upgrade, on the raw request (peeked, not consumed, then handed to
// QWebSocketServer): path, Host (allowedHosts, the DNS-rebinding guard), Origin (a browser page of another site gets
// 403), the token (401), ?since (400) and the client limit (503). A client whose unsent backlog passes maxBufferedBytes
// is closed with 4008: it reconnects with ?since= and loses nothing.
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <deque>

class QTcpServer;
class QTcpSocket;
class QTimer;
class QWebSocket;
class QWebSocketServer;

namespace studio {
class Clock;
}
namespace studio::store {
class Db;
}

namespace studio::net {

class Auth;

struct HubOptions {
    int ringSize = 2048;                   // recent events kept for replay without the database
    int maxReplay = 5000;                  // a client further behind gets reset "too_far"
    int maxClients = 256;
    qint64 maxBufferedBytes = 16LL * 1024 * 1024;  // unsent bytes per client before it is dropped (4008)
    int pingIntervalMs = 30000;
    int pongTimeoutMs = 75000;             // no pong or message for this long closes the client
    int handshakeTimeoutMs = 10000;
    int maxHeadBytes = 16 * 1024;
    qint64 maxIncomingMessageBytes = 64 * 1024;
};

class WsHub : public QObject {
    Q_OBJECT
public:
    explicit WsHub(QObject* parent = nullptr);
    ~WsHub() override;

    void setOptions(const HubOptions& o) { m_opt = o; }
    void setAuth(const Auth* auth) { m_auth = auth; }
    /// Host names served (lower-case); empty = any. Also the allowed hosts of a browser Origin.
    void setAllowedHosts(const QStringList& hosts) { m_allowedHosts = hosts; }
    void setClock(const Clock* clock) { m_clock = clock; }

    /// Persists events from now on. `db` must be open, on this hub's thread, and outlive the hub or detachStore().
    /// lastSeq() moves up to the table's last seq. Every event must then go through publish(), never straight into the
    /// table, or the ring would miss it.
    void attachStore(store::Db* db);
    /// Back to memory mode; seqs go on from lastSeq() under the same epoch.
    void detachStore();
    bool hasStore() const { return m_db != nullptr; }
    /// The numbering the seqs belong to (see the header comment); empty in store mode until the table has an event.
    QString epoch() const { return m_epoch; }

    bool listen(const QHostAddress& address, quint16 port, QString* error = nullptr);
    /// Stops listening and closes every client (1001 going away).
    void close();
    bool isListening() const;
    quint16 port() const;

    /// Publishes an event. Any thread (marshalled to the hub's). False when the store refused it (queue full); with a
    /// store the event is broadcast once committed, and dropped (counted, logged) if its write fails.
    bool publish(const QString& type, const QString& projectId, const QJsonObject& data);
    /// The newest broadcast seq (or the table's last seq after attachStore).
    qint64 lastSeq() const { return m_lastSeq; }
    int clientCount() const { return int(m_clients.size()); }
    QJsonObject stats() const;

signals:
    void broadcasted(qint64 seq);          // after an event went out (tests, metrics)
    void clientCountChanged(int clients);

private:
    struct Event {
        qint64 seq = 0, ts = 0;
        QString type, projectId;
        QString text;                      // the encoded message, built once
    };
    struct Client {
        QWebSocket* ws = nullptr;
        qint64 hwm = 0;                    // highest seq considered for this client (sent or filtered out)
        QString project;
        QSet<QString> types;
        qint64 lastSeenMs = 0;
    };
    struct Pending {                       // what the handshake check parsed, picked up by onUpgraded()
        bool hasSince = false;
        qint64 since = 0;
        QString epoch;                     // the numbering the client's since belongs to (empty: not given)
        QString project;
        QSet<QString> types;
        qint64 atMs = 0;
    };

    void onTcpConnection();
    void inspect(QTcpSocket* s);
    void refuse(QTcpSocket* s, int status, const QString& why, const QByteArray& extraHeaders = {});
    void onUpgraded();
    void greet(Client& c, const Pending& p);
    void broadcast(Event e);
    bool wants(const Client& c, const Event& e) const;
    bool sendTo(Client& c, const QString& text, bool live);
    void dropClient(QWebSocket* ws);
    void onMessage(QWebSocket* ws, const QString& text);
    void onPingTimer();
    qint64 nowMs() const;
    static QString storeEpoch(qint64 seq, qint64 ts, const QString& type, const QString& projectId);
    static QString encode(qint64 seq, qint64 ts, const QString& type, const QString& projectId, const QJsonObject& data);

    HubOptions m_opt;
    const Auth* m_auth = nullptr;
    const Clock* m_clock = nullptr;
    QStringList m_allowedHosts;
    store::Db* m_db = nullptr;
    QTcpServer* m_tcp = nullptr;
    QWebSocketServer* m_wss = nullptr;
    QTimer* m_pingTimer = nullptr;
    QHash<QWebSocket*, Client> m_clients;
    QHash<QString, Pending> m_pending;     // "<peer address>|<peer port>" -> parsed handshake
    std::deque<Event> m_ring;
    qint64 m_lastSeq = 0;
    QString m_epoch;

    struct Stats {
        quint64 published = 0, broadcast = 0, replayed = 0, resets = 0, refusedHandshakes = 0, slowDropped = 0,
                persistFailed = 0, persistRefused = 0, connected = 0;
    } m_stats;
};

} // namespace studio::net
