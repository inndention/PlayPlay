#ifndef SUPERVISORCLIENTWORKER_H
#define SUPERVISORCLIENTWORKER_H

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QElapsedTimer>
#include <QHash>
#include <atomic>
#include <deque>
#include "ui_ipc/ipcprotocol.h"

class IpcTransport;
class QTimer;

/**
 * SupervisorClientWorker — the operator app's IPC link to the Supervisor, on a thread of its own (perf-app PA-1 / W9).
 *
 * Before, SupervisorClient ran the IpcTransport on the GUI thread: a connect sat in WaitNamedPipe for up to 5 s while
 * the Supervisor's loop was hung (the app went "Not Responding"), and every message was JSON-parsed and every preview
 * frame base64- and JPEG-decoded there. Now this object owns the transport on the "supervisor-ipc" thread:
 *
 *  - connects with IpcTransport::connectToServerAsync (the pipe is opened on a short-lived helper; nothing blocks);
 *  - frames and parses messages here; MSG_TIME_SYNC and the MSG_SUP_ALIVE liveness broadcast are consumed here (the
 *    app has no use for them beyond "the Supervisor is alive") and never wake the GUI thread;
 *  - preview frames (PA-2 / W8 + W11): the transport announces that we accept binary frames, so a PS-5 Supervisor
 *    relays each engine JPEG as raw bytes (no base64, no JSON escaping: ~25 % fewer bytes and no pass over the
 *    picture's text); an older Supervisor still sends the JSON/base64 shape, and both land in Delivery::jpeg as the
 *    JPEG bytes. Nothing decodes the picture here: the QML image provider decodes it on its loader thread at the size
 *    of the tile that asks (QImageReader::setScaledSize, libjpeg DCT scaling), so a picture nobody shows costs nothing
 *    and a bank tile never decodes 1080p;
 *  - counts what arrives (stats(): messages and pipe bytes by kind), so the app can tell what its link costs (PA-4);
 *  - hands the rest to the GUI through an inbox: one queued inboxReady() per batch, the GUI pops with takeNext().
 *    State that the next message replaces (telemetry, previews, the snapshot, per-channel status, runlists) is
 *    coalesced while it waits, so a GUI thread that is busy for seconds finds the latest state plus every event
 *    (alerts, as-run, schedule changes) in order, never a pile of decoded pictures;
 *  - stall detection: every message stamps the receive time; with the pipe up and nothing received for stallMs (3 s;
 *    the Supervisor sends time sync at 1 Hz and a snapshot every 2 s, later MSG_SUP_ALIVE) stalledChanged(true) fires,
 *    the next message clears it. A connect that times out because every pipe instance stays busy (a Supervisor whose
 *    loop no longer accepts) is reported the same way.
 *
 * connectTo / send / setStallMs / shutdown run on the worker thread (queue them with QMetaObject::invokeMethod on this
 * object); takeNext, inboxSize, silentMs, isStalled and stats may be called from any thread.
 */
class SupervisorClientWorker : public QObject {
    Q_OBJECT
public:
    struct Delivery {
        IpcProtocol::Message msg;
        QByteArray jpeg;   // MSG_PREVIEW_FRAME: the JPEG as sent (either wire form); msg carries neither the base64 key nor the bytes
    };
    struct Stats {
        quint64 received = 0;        // every message, time sync included
        quint64 timeSyncs = 0;       // consumed here
        quint64 queued = 0;          // put into the inbox
        quint64 coalesced = 0;       // replaced by a newer message of the same key before the GUI took it
        quint64 overflowDropped = 0; // oldest entries dropped at kMaxInbox (GUI hung for a very long time)
        quint64 badPreviews = 0;     // preview frames without a usable JPEG (empty, or not starting with the SOI marker)
        quint64 binaryPreviews = 0;  // preview frames that arrived as binary frames (PS-5 Supervisor); the rest were JSON/base64
        int inboxPeak = 0;
        // What the Supervisor sent us (PA-4): pipe bytes per message = the 4-byte length header + its JSON (or the binary
        // body), re-measured here (a preview's picture by length). Every message counts, before coalescing.
        quint64 bytes = 0;
        quint64 snapshotBytes = 0, runlistBytes = 0, telemetryBytes = 0, previewBytes = 0, timeSyncBytes = 0;   // other = the rest
        quint64 snapshots = 0, runlists = 0, events = 0;   // whole snapshots, runlists, MSG_UI_SCHEDULE events received
        quint64 alive = 0;           // MSG_SUP_ALIVE liveness (consumed here like time sync)
    };
    static constexpr int kDefaultStallMs = 3000;
    static constexpr int kMaxInbox = 5000;     // live entries; events only reach it if the GUI is stuck for many minutes

    explicit SupervisorClientWorker(QObject* parent = nullptr);
    ~SupervisorClientWorker() override;

    // Worker thread.
    void connectTo(const QString& ipcName, int timeoutMs);
    // Announce to the Supervisor that preview JPEGs may come as binary frames (default on; PA-2). Takes effect at the
    // next connect (or at once on a live link). Off = the pre-PS-5 JSON/base64 form for every picture.
    void setAcceptBinary(bool on);
    void send(const IpcProtocol::Message& msg);
    void setStallMs(int ms) { m_stallMs.store(ms > 0 ? ms : kDefaultStallMs); }
    void shutdown();   // drops the link and the inbox; call before quitting the thread

    // Any thread.
    bool takeNext(Delivery* out);   // the oldest live entry; false = empty, and the next arrival signals inboxReady again
    int inboxSize() const;
    qint64 silentMs() const;        // since the last message while connected; -1 when not connected
    bool isStalled() const { return m_stalled.load(); }
    Stats stats() const;

    // Non-empty: a newer message with the same key replaces a queued one (latest state wins). Events get no key.
    static QString coalesceKey(const IpcProtocol::Message& msg);
    // Bytes this message took on the pipe: length header + compact JSON. extraBase64 = the length of a base64 field
    // already taken out of the payload (the preview picture), counted with its key instead of serialized again.
    static qint64 wireBytes(const IpcProtocol::Message& msg, qsizetype extraBase64 = -1);
    // Bytes a binary preview frame took: header + version + head length + JSON head (msg without the picture, plus
    // binaryKey) + the JPEG (ipcprotocol.h, kBinaryFrameFlag).
    static qint64 binaryWireBytes(const IpcProtocol::Message& msg, const QString& binaryKey, qsizetype jpegLen);
    static qsizetype base64Length(qsizetype bytes) { return ((bytes + 2) / 3) * 4; }   // padded, as QByteArray::toBase64 writes it
    static bool looksLikeJpeg(const QByteArray& b) { return b.size() > 2 && quint8(b[0]) == 0xFF && quint8(b[1]) == 0xD8; }   // SOI marker

signals:
    void connectionStateChanged(bool up);
    void connectFailed(const QString& reason, bool busy);   // busy: the pipe exists but never accepted (hung Supervisor)
    void inboxReady();
    void stalledChanged(bool stalled, qint64 silentMs, bool busy);

private:
    struct Entry { Delivery d; QString key; quint64 seq = 0; bool dead = false; };

    IpcTransport* m_ipc = nullptr;
    QTimer* m_stallTimer = nullptr;
    QElapsedTimer m_clock;                  // monotonic; started once, only read afterwards
    std::atomic<qint64> m_lastRxMs{-1};     // m_clock ms of the last message; -1 = not connected
    std::atomic<bool> m_stalled{false};
    std::atomic<int> m_stallMs{kDefaultStallMs};
    bool m_acceptBinary = true;             // set before the thread starts or on the worker thread
    bool m_up = false;                      // worker thread: the transport reports the pipe connected (it repeats itself)
    bool m_busyStall = false;               // worker thread: the stall came from a busy connect, not silence

    mutable QMutex m_mu;                    // guards everything below
    std::deque<Entry> m_inbox;
    QHash<QString, quint64> m_keySeq;       // key -> seq of its live entry
    quint64 m_nextSeq = 0;
    int m_live = 0;
    bool m_posted = false;                  // an inboxReady is on its way and the GUI has not seen the inbox empty since
    Stats m_stats;

    void onMessage(const IpcProtocol::Message& msg);
    void onTransportState(bool up);
    void onConnectFailed(const QString& reason);
    void checkStall();
    void push(Delivery&& d, const QString& key);
    void compactLocked();
    void setStalled(bool on, qint64 silent, bool busy);
};

#endif // SUPERVISORCLIENTWORKER_H
