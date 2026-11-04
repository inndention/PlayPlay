#ifndef IPCTRANSPORT_H
#define IPCTRANSPORT_H

#include <QObject>
#include <QLocalSocket>
#include <QLocalServer>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <memory>
#include "ipcprotocol.h"

/**
 * IpcTransport — Qt-based IPC over named pipes (Windows) or Unix domain sockets.
 *
 * Two roles:
 *   - SERVER (Supervisor): accepts connections from engines + UI, broadcasts time sync
 *   - CLIENT (Engine / UI): connects to supervisor, sends heartbeats, receives commands
 *
 * Backpressure (perf-supervisor PS-1 / W7, PE-20): a peer that stops reading must not grow this process's memory or
 * stall its event loop. Every write first looks at the peer's queued bytes (QLocalSocket::bytesToWrite, Qt's own
 * buffer plus the pending overlapped write): at or above Caps::softBytes Droppable messages are skipped for that peer;
 * at or above Caps::hardBytes the peer is disconnected (deferred to the next loop turn, so callers iterating over
 * clients never see the set change under them). Writes never block.
 *
 * Read side. Qt's Windows pipe reader keeps reading on a thread-pool thread whether or not this thread's event loop
 * runs, and without a limit it would drain a peer into this process's memory while the loop is stuck: the peer's
 * bytesToWrite stays near 0 and its caps never trigger (a hung Supervisor, app or engine just grows). So every socket
 * gets QLocalSocket::setReadBufferSize(readBufferBytes()) (1 MB): a stuck process holds at most that much per peer and
 * the backlog stays in the sender's queue, where its caps apply. Frames bigger than that are collected in a per-peer
 * buffer (at most maxMessageBytes() + 4). One readyRead hands out messages for at most readSliceMs() and re-queues
 * the rest, so catching up after a stall is a series of short loop turns. A length header above maxMessageBytes()
 * (64 MB) closes the connection instead of waiting for, or allocating, a bogus 4 GB frame. A peer that disconnects
 * still has everything it sent before leaving handed out first (an engine's last as-run / fatal message).
 *
 * Default caps by role: the server applies both caps to every client; setPeerCaps() overrides them for one client
 * (the Supervisor's ChannelManager turns the hard cap off for engine links, whose frozen engines the heartbeat
 * watchdog restarts). A client has no hard cap by default (past the soft cap a client queues only its small Normal
 * traffic); engines, which reconnect since PS-3, set both caps on their link in SupervisorLink.
 *
 * Binary frames (PS-5 / W8, ipcprotocol.h kBinaryFrameFlag): a message with Message::binary set (a preview JPEG) goes
 * out as a binary frame (JSON head + raw bytes) to a peer that announced it accepts them, and as the pre-PS-5 JSON form
 * (base64 under payload[binaryKey]) to every other peer; each form is built at most once per send, whatever the number
 * of recipients. Negotiation (MSG_IPC_CAPS, handled here, never handed out): a client sends its caps first on every
 * connection; a server answers only clients that did, so old clients see nothing new and old servers ignore the
 * unknown type. acceptBinary defaults to on for the server (the Supervisor relays msg.binary) and off for clients (the
 * app reads payload["jpegBase64"]; a client that turns it on must read msg.binary). A binary frame that reaches an
 * owner that did not accept binary is inlined back to the JSON shape first. MSG_UI_PREVIEW_SUBSCRIBE from a client sets
 * that client's preview filter (keys + fps cap) for MSG_PREVIEW_FRAME sends; without one it gets every preview.
 */
class IpcTransport : public QObject {
    Q_OBJECT

public:
    enum Role { ServerRole, ClientRole };

    // Send classes. Droppable = state the next message replaces (previews, telemetry, time sync): skipped while the peer
    // is behind. Normal = everything else (commands, playlists, runlists, alerts, as-run): never skipped. Auto picks the
    // class from the message type (defaultPriority), so callers that pass nothing keep working.
    enum Priority { Auto = -1, Normal = 0, Droppable = 1 };
    enum class Verdict { Send, Drop, Disconnect };
    struct Caps {
        qint64 softBytes = qint64(4) << 20;    // Droppable skipped from here (0 = never)
        qint64 hardBytes = qint64(64) << 20;   // peer disconnected from here (0 = never)
    };
    static constexpr qint64 kDefaultMaxMessageBytes = qint64(64) << 20;
    static constexpr qint64 kDefaultReadBufferBytes = qint64(1) << 20;
    static constexpr int kDefaultReadSliceMs = 10;

    static Priority defaultPriority(IpcProtocol::MessageType type);
    // Pure policy (table-tested): what to do with one message for a peer that has queuedBytes pending. p is resolved.
    static Verdict decide(qint64 queuedBytes, Priority p, const Caps& caps);
    static Caps defaultCaps(Role role);   // server: 4 MB / 64 MB; client: 4 MB / no hard cap (see above)

    // Preview subscription of one client (PS-5). all = every key; else exact keys and "prefix*" patterns; maxFps 0 = no cap.
    struct PreviewSub {
        bool all = true;
        QStringList keys;
        double maxFps = 0;
    };
    // Pure (table-tested): does a preview for `key` at nowMs pass, given when that key last went to this client (-1 =
    // never)? The fps gap allows 15 % early so a source a little faster than the cap is not halved.
    static bool previewPass(const PreviewSub& sub, const QString& key, qint64 nowMs, qint64 lastMs);

    explicit IpcTransport(Role role, QObject* parent = nullptr);
    ~IpcTransport();

    void setCaps(const Caps& caps) { m_caps = caps; }
    Caps caps() const { return m_caps; }
    void setMaxMessageBytes(qint64 n) { m_maxMessageBytes = n > 0 ? n : kDefaultMaxMessageBytes; }
    qint64 maxMessageBytes() const { return m_maxMessageBytes; }
    // How much Qt reads ahead per socket while this thread is not reading (applied to current and future sockets).
    void setReadBufferBytes(qint64 n);
    qint64 readBufferBytes() const { return m_readBufferBytes; }
    // Longest stretch one readyRead spends handing out messages before re-queueing the rest (at least one message).
    void setReadSliceMs(int ms) { m_readSliceMs = qMax(0, ms); }
    int readSliceMs() const { return m_readSliceMs; }
    // Counters: dropped (Droppable skipped), slowDisconnects, oversizeCloses, readYields, and per-peer queued bytes.
    QJsonObject stats() const;

    // Binary frames (PS-5). Turning it on/off later re-announces to every peer that speaks caps.
    void setAcceptBinary(bool on);
    bool acceptsBinary() const { return m_acceptBinary; }
    bool peerAcceptsBinary(const QString& clientId = QString()) const;   // client role: the server (clientId ignored)

    // Server API
    bool startServer(const QString& serverName);
    void broadcast(const IpcProtocol::Message& msg, Priority p = Auto);                          // serialized once
    bool sendTo(const QString& clientId, const IpcProtocol::Message& msg, Priority p = Auto);    // false: unknown, dropped or disconnecting
    int sendTo(const QSet<QString>& clientIds, const IpcProtocol::Message& msg, Priority p = Auto);   // serialized once; returns how many got it
    qint64 queuedBytes(const QString& clientId) const;                                           // -1 for an unknown client
    QStringList clientIds() const { return m_clients.keys(); }
    // Caps for one connected client instead of caps() (until it disconnects). False for an unknown client.
    bool setPeerCaps(const QString& clientId, const Caps& caps);

    // Client API
    bool connectToServer(const QString& serverName);   // blocks up to 5 s (engine startup; unchanged)
    // Never blocks the calling thread: the pipe is opened on a short-lived worker (waiting up to timeoutMs while every
    // pipe instance is busy, i.e. the server's loop is not accepting) and handed back to this thread. Ends with
    // connectionStateChanged(true) or connectFailed(reason). A later connect or disconnect() cancels a pending one.
    void connectToServerAsync(const QString& serverName, int timeoutMs = 3000);
    bool isConnecting() const { return bool(m_pending); }
    bool send(const IpcProtocol::Message& msg, Priority p = Auto);   // false: not connected, dropped or disconnecting
    bool isConnected() const;
    qint64 queuedBytes() const;   // client role: bytes waiting for the server to read
    // Client role: ask the server for only these previews (MSG_UI_PREVIEW_SUBSCRIBE). An old server ignores it.
    bool subscribePreviews(bool all, const QStringList& keys = {}, double maxFps = 0);

    // Process id at the other end of a connected pipe (Windows; 0 when unknown): client role = the server's (clientId
    // ignored), server role = that client's. Engines learn their Supervisor's pid from it (PS-3); the Supervisor can
    // tell which process a client id belongs to (PS-4b binding guard).
    qint64 peerProcessId(const QString& clientId = QString()) const;

    // Common
    void disconnect();
    Role role() const { return m_role; }

signals:
    void messageReceived(const IpcProtocol::Message& msg, const QString& clientId);
    void clientConnected(const QString& clientId);
    void clientDisconnected(const QString& clientId);
    void connectionStateChanged(bool connected);
    void connectFailed(const QString& reason);   // connectToServerAsync only

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();
    void onSocketReadyRead();
    void onSocketStateChanged(QLocalSocket::LocalSocketState state);

private:
    struct Peer {
        QString id;
        bool doomed = false;          // disconnect queued: no more reads or writes
        bool dropping = false;        // inside a "behind" episode (logged once per episode, ends below softBytes / 2)
        quint64 droppedEpisode = 0;
        quint64 dropped = 0;
        bool warned = false;          // this episode was logged, so its end is logged too
        qint64 lastWarnMs = -1;       // "behind" warnings at most every 30 s per peer (a peer near the cap would flap)
        qint64 peakQueued = 0;
        bool hasCaps = false;         // setPeerCaps: caps below instead of m_caps
        Caps caps;
        QByteArray rx;                // the frame being collected, header included (at most maxMessageBytes + 4)
        bool drainPosted = false;     // a continuation of processBuffer is queued
        // PS-5
        bool capsSeen = false;        // the peer sent MSG_IPC_CAPS (it understands negotiation)
        int peerBinary = 0;           // highest binary frame version the peer accepts
        bool hasSub = false;          // server role: the client sent MSG_UI_PREVIEW_SUBSCRIBE
        PreviewSub sub;
        QHash<QString, qint64> subLastMs;
    };
    // Builds each wire form of one message at most once, however many peers get it.
    struct Framer {
        const IpcProtocol::Message& msg;
        QByteArray json, bin;
        const QByteArray& get(bool binary);
    };
    struct PendingConnect;

    Role m_role;
    QLocalServer* m_server;
    QLocalSocket* m_clientSocket;
    QMap<QString, QLocalSocket*> m_clients;
    QHash<const QLocalSocket*, Peer> m_peers;
    Caps m_caps;
    qint64 m_maxMessageBytes = kDefaultMaxMessageBytes;
    qint64 m_readBufferBytes = kDefaultReadBufferBytes;
    int m_readSliceMs = kDefaultReadSliceMs;
    quint64 m_droppedTotal = 0, m_slowDisconnects = 0, m_oversizeCloses = 0, m_readYields = 0;
    bool m_acceptBinary = false;
    quint64 m_binarySent = 0, m_binaryFallbacks = 0, m_binaryReceived = 0, m_unknownFrames = 0, m_subscriptionSkips = 0;
    std::shared_ptr<PendingConnect> m_pending;
    QElapsedTimer m_clock;   // monotonic, for log rate limits

    // drainAll: hand out everything already received without a time slice (the peer is leaving).
    void processBuffer(QLocalSocket* socket, bool drainAll = false);
    void postDrain(QLocalSocket* socket);
    bool fitsFrame(const QByteArray& framed, IpcProtocol::MessageType type) const;
    bool writeFramed(QLocalSocket* socket, const QByteArray& framed, Priority p);
    // Picks the peer's form (binary or JSON), applies its preview filter, checks the frame limit, then writeFramed.
    bool writeMessage(QLocalSocket* socket, Framer& f, Priority p);
    bool handleControl(QLocalSocket* socket, const IpcProtocol::Message& msg);   // true: a transport control message
    void sendCaps(QLocalSocket* socket);
    void dropClientSocket();
    void dropConnection(QLocalSocket* socket, const QString& why);
    void cancelPendingConnect();
    void finishAsyncConnect(const std::shared_ptr<PendingConnect>& st);
    void wireClientSocket(QLocalSocket* s);
};

#endif // IPCTRANSPORT_H
