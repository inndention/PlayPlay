#ifndef WEBSOCKETBROKER_H
#define WEBSOCKETBROKER_H

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QTimer>
#include <QElapsedTimer>
#include <QHostAddress>

// Per-client send policy (pure, unit-tested). The queue is what the client's socket still holds (QWebSocket::bytesToWrite):
// at or above the hard cap the client is stalled and gets disconnected (it reconnects and gets a fresh snapshot); at or
// above the soft cap a droppable message (state a later one supersedes: telemetry, snapshot, heartbeat) is skipped.
namespace wsbroker {
enum class Send { Now, Skip, Drop };
Send decide(qint64 queuedBytes, bool droppable, qint64 softCapBytes, qint64 hardCapBytes);
}

/**
 * WebSocketBroker — real-time telemetry push server for UI clients.
 *
 * Broadcasts channel state changes, heartbeats, AS-RUN events,
 * and time sync to all connected WebSocket clients.
 *
 * Cost and backpressure (perf-supervisor W16 / PS-10):
 *  - a message is serialized and converted to QString once, not once per client;
 *  - "low" clients (every client that did not send {"type":"subscribe","rate":"high"}) get heartbeat / telemetry / snapshot
 *    at most once per interval per (type, channel); a newer message replaces the held one and goes out when the interval
 *    ends (latest wins, never lost). A client's first snapshot is never held back;
 *  - a per-client pending cap (see wsbroker::decide) keeps one stalled client from growing the Supervisor's memory.
 */
class WebSocketBroker : public QObject {
    Q_OBJECT

public:
    explicit WebSocketBroker(QObject* parent = nullptr);
    ~WebSocketBroker();

    bool start(quint16 port = 8081, const QHostAddress& address = QHostAddress::Any);
    void stop();
    quint16 port() const;     // the bound port (start(0) picks a free one)

    void broadcast(const QJsonObject& message);
    void broadcastToChannel(const QString& channelId, const QJsonObject& message);
    /// One client only (the snapshot a dashboard gets when it connects, PS-6): the same pending cap, never held by the
    /// low-rate fold. False when the client is unknown, being dropped, or over its hard cap (then it is disconnected).
    bool sendTo(const QString& clientId, const QJsonObject& message);

    int clientCount() const;

    // Tuning (defaults: soft 1 MB, hard 8 MB; low-rate heartbeat 1000 ms, telemetry 900 ms, snapshot 1000 ms: below the
    // engines' 1 Hz telemetry and the 2 s snapshot tick, so today's steady streams pass untouched and only bursts fold).
    void setPendingCaps(qint64 softCapBytes, qint64 hardCapBytes);
    void setLowRateInterval(const QString& type, int ms);        // 0 = never throttled
    static bool isDroppable(const QString& type);

    // {softCapBytes, hardCapBytes, droppedSlowClients, coalesced, clients:[{clientId, rate, queuedBytes, sent, skipped}]}
    QJsonObject stats() const;

signals:
    void clientConnected(const QString& clientId);
    void clientDisconnected(const QString& clientId);
    void messageReceived(const QString& clientId, const QJsonObject& message);

private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onTextMessage(const QString& message);

public:
    void setAuthToken(const QString& t) { m_token = t; }
    void setLanPassword(const QString& password);
private:
    struct Client {
        QWebSocket* socket = nullptr;
        QString id;
        bool high = false;            // subscribed with rate "high": never throttled
        bool gotSnapshot = false;     // the first snapshot is never held back
        bool dropping = false;        // over the hard cap: being disconnected
        bool skipping = false;        // over the soft cap (logged once per episode)
        qint64 sent = 0, skipped = 0;
    };
    struct LowSlot {                  // low-rate coalescing per (type, channel)
        qint64 lastSentMs = -1;
        int intervalMs = 0;
        QString pending;              // newest held message (already serialized)
        bool droppable = false, snapshot = false, hasPending = false;
    };

    QString m_token;
    QString m_lanPassword;
    QWebSocketServer* m_server;
    QList<Client*> m_clients;
    mutable QMutex m_clientsMutex;
    int m_nextClientId;
    qint64 m_softCap = 1 << 20, m_hardCap = 8 << 20;
    QHash<QString, int> m_lowInterval;
    QHash<QString, LowSlot> m_low;
    QTimer m_flushTimer;
    QElapsedTimer m_clock;            // monotonic: wall-clock steps must not open or shut the throttle
    qint64 m_droppedSlow = 0, m_coalesced = 0;

    QString generateClientId();
    Client* findClient(QWebSocket* s) const;          // callers hold m_clientsMutex
    bool removeClient(QWebSocket* s);                  // takes the lock; true when s was still listed
    // Sends text to one client under the pending cap; returns false when the client must be disconnected.
    bool deliver(Client* c, const QString& text, bool droppable, bool snapshot);
    void dropStalled(const QList<QWebSocket*>& victims);
    void flushLow();
    void scheduleFlush(qint64 nowMs);
};

#endif // WEBSOCKETBROKER_H
