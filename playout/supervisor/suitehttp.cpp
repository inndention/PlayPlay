#include "suitehttp.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <memory>

static constexpr qint64 kMaxBody = 16 * 1024 * 1024;

SuiteHttp::SuiteHttp(QObject* parent) : QObject(parent) {}

QJsonObject SuiteHttp::parse(const QByteArray& data) {
    QJsonParseError pe; const QJsonDocument d = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) return {};
    if (d.isObject()) return d.object();
    if (d.isArray()) { QJsonObject o; o["_array"] = d.array(); return o; }
    return {};
}

QString SuiteHttp::errorText(int http, const QJsonObject& json, const QByteArray& raw, const QString& netError) {
    const QJsonValue e = json.value("error");
    QString msg = e.isObject() ? e.toObject().value("message").toString(e.toObject().value("code").toString()) : e.toString();
    if (msg.isEmpty()) msg = json.value("detail").toString();
    if (msg.isEmpty() && json.isEmpty() && !raw.isEmpty()) msg = QString::fromUtf8(raw.left(200)).simplified();
    if (http == 0) return netError.isEmpty() ? QString("no answer") : netError;
    return msg.isEmpty() ? QString("HTTP %1").arg(http) : QString("HTTP %1: %2").arg(http).arg(msg);
}

static void finishReply(QNetworkReply* r, std::shared_ptr<QByteArray> buf, SuiteHttp::Cb cb) {
    const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString err;
    if (r->error() != QNetworkReply::NoError && http == 0) err = r->error() == QNetworkReply::OperationCanceledError ? QString("timed out") : r->errorString();
    const QByteArray data = *buf;
    cb(http, SuiteHttp::parse(data), data, err);
}

void SuiteHttp::raw(const QString& method, const QString& url, const Headers& headers, const QByteArray& body, const QByteArray& contentType, int timeoutMs, Cb cb) {
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!contentType.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    QNetworkReply* r = m_nam.sendCustomRequest(req, method.toUtf8(), body);
    auto buf = std::make_shared<QByteArray>();
    connect(r, &QNetworkReply::readyRead, r, [r, buf]() { buf->append(r->readAll()); if (buf->size() > kMaxBody) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [r, buf, cb]() { buf->append(r->readAll()); r->deleteLater(); finishReply(r, buf, cb); });
}

void SuiteHttp::json(const QString& method, const QString& url, const Headers& headers, const QJsonObject& body, int timeoutMs, Cb cb) {
    QByteArray data;
    const QString m = method.toUpper();
    if (m != "GET" && m != "DELETE" && m != "OPTIONS") data = body.contains("_array") ? QJsonDocument(body.value("_array").toArray()).toJson(QJsonDocument::Compact) : QJsonDocument(body).toJson(QJsonDocument::Compact);
    raw(m, url, headers, data, data.isEmpty() ? QByteArray() : QByteArray("application/json"), timeoutMs, cb);
}

void SuiteHttp::download(const QString& url, const Headers& headers, const QString& localPath, int timeoutMs, Progress progress, Cb cb) {
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    auto file = std::make_shared<QFile>(localPath + ".part");
    if (!file->open(QIODevice::WriteOnly)) { cb(0, {}, {}, "cannot write " + localPath); return; }
    QNetworkReply* r = m_nam.get(req);
    auto errBody = std::make_shared<QByteArray>();
    connect(r, &QNetworkReply::readyRead, r, [r, file, errBody]() {
        const QByteArray chunk = r->readAll();
        if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400) { if (errBody->size() < 65536) errBody->append(chunk); return; }
        file->write(chunk);
    });
    if (progress) connect(r, &QNetworkReply::downloadProgress, r, [progress](qint64 b, qint64 t) { progress(b, t); });
    connect(r, &QNetworkReply::finished, this, [r, file, errBody, localPath, cb]() {
        r->deleteLater();
        file->write(r->readAll()); file->close();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError || http >= 400) {
            QFile::remove(localPath + ".part");
            cb(http, SuiteHttp::parse(*errBody), *errBody, http == 0 ? r->errorString() : QString());
            return;
        }
        QFile::remove(localPath);
        if (!QFile::rename(localPath + ".part", localPath)) { cb(0, {}, {}, "cannot finish " + localPath); return; }
        cb(http ? http : 200, {}, {}, QString());
    });
}

void SuiteHttp::multipart(const QString& url, const Headers& headers, const QList<QPair<QString, QString>>& fields, const QString& fileField,
                          const QString& filePath, const QString& fileType, int timeoutMs, Cb cb) {
    auto* mp = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const auto& f : fields) {
        QHttpPart p; p.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"%1\"").arg(f.first)); p.setBody(f.second.toUtf8()); mp->append(p);
    }
    auto* file = new QFile(filePath, mp);
    if (!file->open(QIODevice::ReadOnly)) { delete mp; cb(0, {}, {}, "cannot read " + filePath); return; }
    QHttpPart fp; fp.setHeader(QNetworkRequest::ContentTypeHeader, fileType);
    fp.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"%1\"; filename=\"%2\"").arg(fileField, QFileInfo(filePath).fileName()));
    fp.setBodyDevice(file); mp->append(fp);
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(timeoutMs);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    QNetworkReply* r = m_nam.post(req, mp);
    mp->setParent(r);
    auto buf = std::make_shared<QByteArray>();
    connect(r, &QNetworkReply::readyRead, r, [r, buf]() { buf->append(r->readAll()); if (buf->size() > kMaxBody) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [r, buf, cb]() { buf->append(r->readAll()); r->deleteLater(); finishReply(r, buf, cb); });
}
