#pragma once
// Reporter feed (docs/reporter-feed.md): a field reporter goes live from the getstarnews app into PlayPlay. The Supervisor
// keeps one session per reporter, runs an RTMP ingest the phone publishes to, lists the feed as a live input (library entry
// live://reporter/<id>, also under `reporter` in GET /api/v1/inputs) and, when a go-live countdown reaches zero, takes it
// to program on the session's channel through the normal transport take.
//
// Built into PlayPlaySupervisor by cmake/lane-reporterfeed.cmake (target_sources + laneext::Registrar "reporterfeed").
// supervisorcore*.cpp is untouched except two guarded lines that add `reporter` to GET /api/v1/inputs.
//
// Routes (JSON; the Supervisor's bearer token applies like every /api/v1 route):
//   POST   /api/v1/reporter/sessions          {reporter, title?, channel?, countdownSec?} -> session + publishUrl + streamKey
//                                             (the key is in this reply only; later reads carry a hash prefix)
//   GET    /api/v1/reporter/sessions[?all=1]  {sessions:[...]}   GET/DELETE /api/v1/reporter/sessions/{id}
//   POST   /api/v1/reporter/sessions/{id}/go  {countdownSec?}: countdown, WS "reporter" events every second, take at zero
//   POST   /api/v1/reporter/sessions/{id}/take-now | /cancel | /end
//   GET/PUT /api/v1/reporter/config
// Media path (mode "ffmpeg", default): one ffmpeg.exe per session listening (`-listen 1`) on the session's RTMP port, copying
// the feed to MPEG-TS over UDP on 127.0.0.1:<udpPort>; the engine pulls that as a normal `stream` live source.
// Mode "mediamtx" only computes URLs for a relay the operator runs (RTMP or WHIP in, RTSP pull for the engine).
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace laneext { struct Context; }

namespace reporterfeed {

struct Config {
    QString host;                       // address the phone publishes to; empty = this machine's first non-loopback IPv4
    int rtmpPortBase = 1936;            // not 1935: something else may own it
    int portCount = 8;                  // sessions use rtmpPortBase .. rtmpPortBase+portCount-1
    int udpPortBase = 50040;            // engine-side UDP of slot i is udpPortBase+i
    QString app = "reporter";           // rtmp://host:port/<app>/<key>
    QString mode = "ffmpeg";            // "ffmpeg" | "mediamtx"
    QString mediamtxPublishBase;        // mode mediamtx: the phone publishes <base>/<app>/<key> (rtmp://...)
    QString mediamtxWhipBase;           // mode mediamtx: WHIP at <base>/<app>/<key>/whip (http://...)
    QString mediamtxPullBase = "rtsp://127.0.0.1:8554";   // mode mediamtx: the engine pulls <base>/<app>/<key>
    int defaultCountdownSec = 10;
    QString ffmpegPath;                 // empty = ffmpeg.exe next to the Supervisor, else "ffmpeg" on PATH

    QJsonObject toJson() const;
    static Config fromJson(const QJsonObject& o, const Config& base = Config());
};

struct Session {
    QString id;
    QString reporter;
    QString title;
    QString channel;
    QString status = "idle";            // idle -> countdown -> live -> ended; cancel: countdown -> idle
    int countdownSec = 10;
    int remainingSec = 0;               // while counting down
    int slot = -1;                      // port slot (rtmpPortBase+slot / udpPortBase+slot)
    QString keyHash;                    // sha256 hex of the stream key (the key itself is never stored in the session)
    bool feedUp = false;                // a publisher is connected to the ingest
    int ingestRestarts = 0;
    QString lastError;
    qint64 createdMs = 0, goMs = 0, liveMs = 0, endedMs = 0;

    bool active() const { return status != "ended"; }
    QJsonObject toJson() const;
};

/// Session store + state machine + countdown. Side effects (ingest process, media library entry, take, WS) go through the
/// hooks below so the Supervisor wiring (install) and the unit test (stubs) share one implementation.
class ReporterFeed : public QObject {
    Q_OBJECT
public:
    explicit ReporterFeed(QObject* parent = nullptr);

    Config config;
    // hooks
    std::function<void(const Session&, const QString& key)> startIngest;   // run the listener for this session's key
    std::function<void(const Session&)> stopIngest;
    std::function<void(const Session&)> saveInput;                         // (re)write the library entry live://reporter/<id>
    std::function<void(const Session&)> removeInput;
    std::function<void(const Session&, std::function<void(bool, const QString&)>)> takeToProgramAsync;
    std::function<bool(const Session&, QString* why)> takeToProgram;       // transport take on session.channel
    std::function<void(const Session&)> releaseProgram;                    // back to the schedule
    std::function<void(const QString& event, const QJsonObject& payload)> broadcast;   // WS type "reporter"
    std::function<void(const QString& channel, const QString& level, const QString& msg)> alert;
    std::function<qint64()> nowMs;                                          // test clock

    // sessions
    QJsonObject create(const QJsonObject& body, QString* err);              // the only reply that carries streamKey
    bool exists(const QString& id) const { return m_sessions.contains(id); }
    Session session(const QString& id, bool* found = nullptr) const;
    QList<Session> sessions(bool includeEnded = false) const;
    QJsonArray sessionsJson(bool includeEnded = false) const;
    bool go(const QString& id, int countdownSec, QString* err);             // countdownSec < 0 = the session's own
    bool takeNow(const QString& id, QString* err);
    bool cancel(const QString& id, QString* err);
    bool end(const QString& id, QString* err);
    bool remove(const QString& id);
    void noteFeed(const QString& id, bool up, const QString& error = QString());
    void noteTakeFailure(const QString& id, const QString& error);
    /// One countdown step for every session counting down (the install hooks a 1 s timer; tests call it directly).
    void tick();

    // derived
    QString enginePath(const Session& s) const;                             // what the engine opens (udp:// or rtsp://)
    QString publishUrl(const Session& s, const QString& key) const;         // what the phone types
    QString whipUrl(const Session& s, const QString& key) const;            // mode mediamtx only, else ""
    QJsonObject inputJson(const Session& s) const;                          // {name:"Reporter: x", type:"stream", url, path, ...}
    QJsonArray inputsJson() const;                                          // active sessions as inputs
    QStringList ffmpegArgs(const Session& s, const QString& key) const;     // the ingest command line (mode ffmpeg)
    int rtmpPort(const Session& s) const { return config.rtmpPortBase + qMax(0, s.slot); }
    int udpPort(const Session& s) const { return config.udpPortBase + qMax(0, s.slot); }
    /// A second copy of the stream for the engine's input monitors. A UDP port takes one reader, and GO must be able to open
    /// `enginePath` while the On Air wall shows the same feed (it failed with WSAEADDRINUSE, -10048, before). Not used in mode
    /// mediamtx (RTSP serves many readers).
    int monitorUdpPort(const Session& s) const { return config.udpPortBase + 100 + qMax(0, s.slot); }
    QString monitorPath(const Session& s) const { return config.mode == "mediamtx" ? QString() : QString("udp://127.0.0.1:%1").arg(monitorUdpPort(s)); }
    QString hostForUrls() const;

    static QString newKey();                                                // 24 hex chars from the CSPRNG
    static QString hashKey(const QString& key);                             // sha256 hex

private:
    int freeSlot() const;
    bool goLiveNow(Session& s, QString* err);
    void emitSession(const QString& event, const Session& s);
    QHash<QString, Session> m_sessions;
    QHash<QString, QString> m_keys;     // id -> key, memory only (the listener restart needs it); dropped at end
    QHash<QString, quint64> m_takeGeneration;
    QStringList m_order;                // creation order
};

/// Lane install (called by the laneext registrar): routes, timer, ingest processes, media entries, loopback takes.
void install(const laneext::Context& ctx);
/// Active sessions as inputs for GET /api/v1/inputs (empty when the lane is not installed).
QJsonArray inputsJson();
void noteAsRunFailure(const QString& channel, const QString& assetId, const QString& reason);

} // namespace reporterfeed
