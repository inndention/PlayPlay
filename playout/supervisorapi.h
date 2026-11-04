#ifndef SUPERVISORAPI_H
#define SUPERVISORAPI_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QJSValue>
#include <QVariant>
#include <QQuickImageProvider>
#include <QHash>
class QNetworkReply;

class SupervisorClient;

/**
 * SupervisorApi — the operator UI's REST bridge to the Supervisor.
 *
 * Telemetry, runlists, previews and alerts arrive over IPC (SupervisorClient);
 * everything the UI edits or browses (media, schedule, channel profiles,
 * as-run, settings) goes through REST so the web dashboard and the native app
 * share one surface. QML calls:
 *
 *   api.get("/channels/main/runlist", function(ok, data, status) {...})
 *   api.post("/channels/main/cg", {action: "show", ...}, cb)
 *   api.put("/schedule/main/slot/0-9", slot, cb)
 *   api.del("/media/" + id, cb)
 *
 * Paths are relative to /api/v1. Callbacks receive parsed JSON as a JS value.
 */
class SupervisorApi : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString baseUrl READ baseUrl WRITE setBaseUrl NOTIFY baseUrlChanged)
    Q_PROPERTY(int pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString token READ token WRITE setToken NOTIFY baseUrlChanged)
    // perf-app PA-1: true while the Supervisor is stalled (main.qml binds supervisor.supervisorStalled here). GETs are then
    // held instead of sent, one per path, so polling timers stop piling requests into a hung Supervisor's backlog; the
    // newest callbacks per path are kept and all get the one answer fetched when it is cleared. Writes (POST/PUT/DELETE)
    // are operator actions and go out as before.
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
public:
    explicit SupervisorApi(QObject* parent = nullptr);
    QString baseUrl() const { return m_base; }
    void setBaseUrl(const QString& u);
    int pending() const { return m_pending; }
    QString lastError() const { return m_lastError; }
    QString token() const { return m_token; }
    void setToken(const QString& t);
    bool paused() const { return m_paused; }
    void setPaused(bool p);
    int heldPaths() const { return int(m_heldOrder.size()); }   // tests / diagnostics
    int heldCallbacks() const;

    Q_INVOKABLE void get(const QString& path, const QJSValue& cb = QJSValue());
    Q_INVOKABLE void post(const QString& path, const QVariant& body, const QJSValue& cb = QJSValue());
    Q_INVOKABLE void put(const QString& path, const QVariant& body, const QJSValue& cb = QJSValue());
    Q_INVOKABLE void del(const QString& path, const QJSValue& cb = QJSValue());
    /// Absolute URL for raw resources (preview JPEG/MJPEG, static web root).
    Q_INVOKABLE QString rawUrl(const QString& path) const;
    /// Windows Explorer: select the file (or open the folder).
    Q_INVOKABLE void revealInExplorer(const QString& path) const;
    Q_INVOKABLE bool openMediaFile(const QString& path) const;
    Q_INVOKABLE QString localPath(const QString& urlOrPath) const;
    /// Drop pending callbacks (QJSValues must die before the QML engine) and abort requests.
    void shutdown();

signals:
    void baseUrlChanged();
    void pendingChanged();
    void lastErrorChanged();
    void failed(const QString& method, const QString& path, int status, const QString& message);
    void pausedChanged();

private:
    QNetworkAccessManager m_nam;
    QString m_base = "http://127.0.0.1:7700";
    QString m_token;                    // PLAYPLAY_API_TOKEN env or QSettings supervisor/token
    int m_pending = 0;
    QString m_lastError;
    QHash<QNetworkReply*, QList<QJSValue>> m_callbacks;   // a released held GET answers several callers
    // Held GETs while paused: path -> callbacks (newest kHeldCallbacksPerPath), in first-held order.
    static constexpr int kHeldCallbacksPerPath = 8;
    static constexpr int kHeldPaths = 128;
    bool m_paused = false;
    QHash<QString, QList<QJSValue>> m_held;
    QStringList m_heldOrder;
    bool m_heldOverflowLogged = false;
    void hold(const QString& path, const QList<QJSValue>& cbs);
    // Start-up: the app is often up before the Supervisor answers. A GET that fails to connect before ANY request has succeeded is asked
    // again, quietly (bounded, 20 s), instead of failing into a toast; once the Supervisor has answered, failures are reported as before.
    bool m_reachable = false;
    bool m_down = false;
    int m_retrySerial = 0;
    QHash<int, QList<QJSValue>> m_retryCbs;
    void request(const QString& method, const QString& path, const QVariant& body, const QList<QJSValue>& cbs, int attempt = 0, qint64 firstMs = 0);
    static QList<QJSValue> one(const QJSValue& cb) { return cb.isCallable() ? QList<QJSValue>{cb} : QList<QJSValue>{}; }
};

/// image://preview/<channelId>?<serial> — the latest engine preview frame.
///
/// PA-2 (W11): the client keeps each preview as JPEG bytes; this decodes them here, on the QML image loader thread
/// (ProgramImage uses asynchronous images), at the size the tile asked for (Image.sourceSize = the tile's size) with
/// QImageReader::setScaledSize, so libjpeg scales in the DCT domain (1/2, 1/4, 1/8) and a 1080p picture shown in a bank
/// tile is decoded at a fraction of the cost of a full decode plus a smooth rescale. Never upscaled here: a tile larger
/// than the picture gets it at its native size and the GPU scales it up.
class PreviewImageProvider : public QQuickImageProvider {
public:
    explicit PreviewImageProvider(SupervisorClient* client);
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
    // The JPEG decoded to fit `requested` (aspect kept; invalid or empty = full size; never upscaled). Null for a
    // picture that does not decode.
    static QImage decodeScaled(const QByteArray& jpeg, const QSize& requested);
private:
    SupervisorClient* m_client;
};

#endif // SUPERVISORAPI_H
