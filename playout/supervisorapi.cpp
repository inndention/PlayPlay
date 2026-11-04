#include "supervisorapi.h"
#include "supervisorclient.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSEngine>
#include <QQmlEngine>
#include <QDebug>
#include <QSettings>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QDesktopServices>
#include <QTimer>
#include <QDateTime>
#include <QBuffer>
#include <QImageReader>
#include <utility>

SupervisorApi::SupervisorApi(QObject* parent) : QObject(parent) {
    m_token = qEnvironmentVariable("PLAYPLAY_API_TOKEN");
    if (m_token.isEmpty()) m_token = QSettings().value("supervisor/token").toString();
}

void SupervisorApi::setToken(const QString& t) { m_token = t; QSettings().setValue("supervisor/token", t); emit baseUrlChanged(); }

void SupervisorApi::setBaseUrl(const QString& u) {
    QString b = u; while (b.endsWith('/')) b.chop(1);
    if (b == m_base) return;
    m_base = b; emit baseUrlChanged();
}

QString SupervisorApi::rawUrl(const QString& path) const {
    QString u = m_base + (path.startsWith('/') ? path : "/" + path);
    if (!m_token.isEmpty() && (path.startsWith("/preview/") || path.startsWith("/api/"))) u += (u.contains('?') ? "&" : "?") + QString("token=") + QString::fromUtf8(QUrl::toPercentEncoding(m_token));
    return u;
}

QString SupervisorApi::localPath(const QString& u) const { return u.startsWith("file:") ? QUrl(u).toLocalFile() : u; }

bool SupervisorApi::openMediaFile(const QString& path) const {
    const QFileInfo file(localPath(path));
    return file.isFile() && QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
}

void SupervisorApi::revealInExplorer(const QString& path) const {
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(localPath(path));
    if (QFileInfo(native).isFile()) QProcess::startDetached("explorer.exe", {"/select,", native});
    else QProcess::startDetached("explorer.exe", {native});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(localPath(path)).absolutePath()));
#endif
}

void SupervisorApi::shutdown() {
    m_down = true;
    m_retryCbs.clear();
    m_callbacks.clear();
    m_held.clear(); m_heldOrder.clear();
    const auto replies = m_nam.findChildren<QNetworkReply*>();
    for (QNetworkReply* r : replies) { r->disconnect(this); r->abort(); }
}

void SupervisorApi::setPaused(bool p) {
    if (p == m_paused) return;
    m_paused = p;
    if (p) {
        qInfo() << "[SupervisorApi] Supervisor stalled: holding GET requests until it answers";
    } else {
        // Released in first-held order, one request per path; every caller held for that path gets its answer.
        const QStringList order = std::exchange(m_heldOrder, {});
        const QHash<QString, QList<QJSValue>> held = std::exchange(m_held, {});
        m_heldOverflowLogged = false;
        if (!order.isEmpty()) qInfo() << "[SupervisorApi] Supervisor answers again: sending" << order.size() << "held GET path(s)";
        if (!m_down) for (const QString& path : order) request("GET", path, {}, held.value(path));
    }
    emit pausedChanged();
}

int SupervisorApi::heldCallbacks() const {
    int n = 0;
    for (const auto& cbs : m_held) n += int(cbs.size());
    return n;
}

void SupervisorApi::hold(const QString& path, const QList<QJSValue>& cbs) {
    auto it = m_held.find(path);
    if (it == m_held.end()) {
        if (m_heldOrder.size() >= kHeldPaths) {
            if (!m_heldOverflowLogged) { m_heldOverflowLogged = true; qWarning() << "[SupervisorApi] more than" << kHeldPaths << "different GETs held while stalled; dropping new ones"; }
            return;
        }
        it = m_held.insert(path, {});
        m_heldOrder << path;
    }
    for (const QJSValue& cb : cbs) it->append(cb);
    while (it->size() > kHeldCallbacksPerPath) it->removeFirst();   // a poll timer keeps adding: its newest calls are enough
}

void SupervisorApi::get(const QString& path, const QJSValue& cb) { request("GET", path, {}, one(cb)); }
void SupervisorApi::post(const QString& path, const QVariant& body, const QJSValue& cb) { request("POST", path, body, one(cb)); }
void SupervisorApi::put(const QString& path, const QVariant& body, const QJSValue& cb) { request("PUT", path, body, one(cb)); }
void SupervisorApi::del(const QString& path, const QJSValue& cb) { request("DELETE", path, {}, one(cb)); }

// Recursively replace QJSValue (from QML) with plain variants so the JSON conversion sees real lists and maps.
static QVariant plainVariant(const QVariant& v) {
    if (v.metaType() == QMetaType::fromType<QJSValue>()) return plainVariant(v.value<QJSValue>().toVariant());
    if (v.typeId() == QMetaType::QVariantMap) {
        QVariantMap m = v.toMap();
        for (auto it = m.begin(); it != m.end(); ++it) it.value() = plainVariant(it.value());
        return m;
    }
    if (v.typeId() == QMetaType::QVariantList || v.typeId() == QMetaType::QStringList) {
        QVariantList l = v.toList();
        for (auto& x : l) x = plainVariant(x);
        return l;
    }
    return v;
}

void SupervisorApi::request(const QString& method, const QString& path, const QVariant& body, const QList<QJSValue>& cbs, int attempt, qint64 firstMs) {
    if (m_down) return; // QML destruction can request work after shutdown; never retain callbacks past its engine.
    if (m_paused && !m_down && method == "GET") { hold(path, cbs); return; }   // also start-up retries: they resume with the rest
    if (!firstMs) firstMs = QDateTime::currentMSecsSinceEpoch();
    const QString url = m_base + "/api/v1" + (path.startsWith('/') ? path : "/" + path);
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    // Folder drops are walked and probed on the Supervisor: give them time; everything else stays snappy.
    req.setTransferTimeout(path.startsWith("/media/resolve") || path.startsWith("/media/scan") ? 120000 : 8000);
    QByteArray payload;
    if (body.isValid() && !body.isNull()) {
        // QML hands nested JS arrays/objects over as QJSValue; QJsonValue::fromVariant turns those into null
        // (a dropped file list arrived as {"paths":null}). Unwrap them to plain QVariantList/QVariantMap first.
        const QVariant plain = plainVariant(body);
        const QJsonDocument doc = plain.typeId() == QMetaType::QVariantList
            ? QJsonDocument(QJsonArray::fromVariantList(plain.toList()))
            : QJsonDocument(QJsonObject::fromVariantMap(plain.toMap()));
        payload = doc.toJson(QJsonDocument::Compact);
    }
    QNetworkReply* reply = m_nam.sendCustomRequest(req, method.toLatin1(), payload);
    ++m_pending; emit pendingChanged();
    if (!cbs.isEmpty()) m_callbacks.insert(reply, cbs);
    connect(reply, &QNetworkReply::finished, this, [this, reply, method, path, attempt, firstMs]() {
        --m_pending; emit pendingChanged();
        const QList<QJSValue> callbacks = m_callbacks.take(reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status > 0) m_reachable = true;
        const QNetworkReply::NetworkError netErr = reply->error();
        const bool noConnection = status == 0 && (netErr == QNetworkReply::ConnectionRefusedError || netErr == QNetworkReply::RemoteHostClosedError || netErr == QNetworkReply::TimeoutError);
        if (noConnection && !m_reachable && !m_down && method == "GET" && QDateTime::currentMSecsSinceEpoch() - firstMs < 20000) {
            if (attempt == 0) qInfo().noquote() << "[SupervisorApi] Supervisor not up yet, retrying quietly:" << path;
            const int id = ++m_retrySerial;
            if (!callbacks.isEmpty()) m_retryCbs.insert(id, callbacks);
            QTimer::singleShot(qMin(2000, 400 + 300 * attempt), this, [this, id, method, path, attempt, firstMs]() {
                const QList<QJSValue> cb2 = m_retryCbs.take(id);
                if (m_down) return;
                request(method, path, QVariant(), cb2, attempt + 1, firstMs);
            });
            reply->deleteLater();
            return;
        }
        const QByteArray raw = reply->readAll();
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &pe);
        QVariant data = doc.isNull() ? QVariant(QString::fromUtf8(raw)) : (doc.isArray() ? QVariant(doc.array().toVariantList()) : QVariant(doc.object().toVariantMap()));
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        if (!ok) {
            m_lastError = QString("%1 %2 -> %3 %4").arg(method, path).arg(status).arg(doc.isObject() ? doc.object().value("error").toString(reply->errorString()) : reply->errorString());
            emit lastErrorChanged();
            emit failed(method, path, status, m_lastError);
        }
        if (QJSEngine* eng = callbacks.isEmpty() ? nullptr : qjsEngine(this)) {
            for (const QJSValue& callback : callbacks) {
                if (!callback.isCallable()) continue;
                QJSValueList args; args << QJSValue(ok) << eng->toScriptValue(data) << QJSValue(status);   // own copy per caller
                if (qEnvironmentVariableIsSet("PLAYPLAY_TRACE_CALLBACKS")) qInfo() << "[SupervisorApi] calling" << method << path;
                const QJSValue r = callback.call(args);
                if (r.isError()) qWarning() << "[SupervisorApi] callback error:" << r.toString() << "for" << method << path;
                if (m_down) break;   // a callback may tear the UI down
            }
        }
        reply->deleteLater();
    });
}

// ---------------------------------------------------------------------------

PreviewImageProvider::PreviewImageProvider(SupervisorClient* client)
    : QQuickImageProvider(QQuickImageProvider::Image), m_client(client) {}

QImage PreviewImageProvider::decodeScaled(const QByteArray& jpeg, const QSize& requested) {
    if (jpeg.isEmpty()) return {};
    QBuffer buf;
    buf.setData(jpeg);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "jpeg");
    if (requested.isValid() && !requested.isEmpty()) {
        const QSize full = reader.size();   // from the header only; no decode yet
        if (full.isValid() && !full.isEmpty()) {
            const QSize fit = full.scaled(requested, Qt::KeepAspectRatio);
            // Smaller than the picture: libjpeg decodes at the DCT scale just above `fit` and the handler rescales the
            // rest (both far cheaper than a full decode + QImage::scaled). Same size or larger: the picture as it is.
            if (fit.width() < full.width() || fit.height() < full.height()) reader.setScaledSize(fit);
        }
    }
    return reader.read();
}

QImage PreviewImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize) {
    QString ch = id; const int q = ch.indexOf('?'); if (q >= 0) ch = ch.left(q);
    QImage img = m_client ? decodeScaled(m_client->previewJpeg(ch), requestedSize) : QImage();
    if (img.isNull()) { img = QImage(320, 180, QImage::Format_RGB32); img.fill(QColor("#0a0a12")); }   // no frame yet: the dark card
    if (size) *size = img.size();
    return img;
}
