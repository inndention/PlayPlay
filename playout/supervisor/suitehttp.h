#ifndef SUITEHTTP_H
#define SUITEHTTP_H

#include <QObject>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <functional>

/**
 * SuiteHttp — the small HTTP client the render / Star node / publisher / agent suites share.
 * Every call answers exactly once: cb(httpStatus, json, raw, error). httpStatus is 0 when the request
 * never got an answer (refused, DNS, timeout); json["_array"] holds a top-level JSON array; a reply
 * that is not JSON leaves json empty and the bytes in raw. Bodies are capped at 16 MiB (downloads go to disk).
 */
class SuiteHttp : public QObject {
    Q_OBJECT
public:
    using Headers = QList<QPair<QByteArray, QByteArray>>;
    using Cb = std::function<void(int http, const QJsonObject& json, const QByteArray& raw, const QString& error)>;
    using Progress = std::function<void(qint64 bytes, qint64 total)>;
    explicit SuiteHttp(QObject* parent = nullptr);

    void json(const QString& method, const QString& url, const Headers& headers, const QJsonObject& body, int timeoutMs, Cb cb);
    void raw(const QString& method, const QString& url, const Headers& headers, const QByteArray& body, const QByteArray& contentType, int timeoutMs, Cb cb);
    /// GET to a file (".part" then rename), following redirects; cb's raw is empty.
    void download(const QString& url, const Headers& headers, const QString& localPath, int timeoutMs, Progress progress, Cb cb);
    /// multipart/form-data POST: text fields plus one file part.
    void multipart(const QString& url, const Headers& headers, const QList<QPair<QString, QString>>& fields, const QString& fileField,
                   const QString& filePath, const QString& fileType, int timeoutMs, Cb cb);
    static QJsonObject parse(const QByteArray& data);
    static QString errorText(int http, const QJsonObject& json, const QByteArray& raw, const QString& netError);

private:
    QNetworkAccessManager m_nam;
};

#endif // SUITEHTTP_H
