// SupervisorCore: reports routes (see registerRoutes order in supervisorcore.cpp).
//   GET /api/v1/reports                  → catalogue of the reports (names, params, URL templates, brand, channels)
//   GET /api/v1/reports/<name>           → JSON data for one report (asplayed|assets|full|adherence|contracts|top|compare)
//   GET /api/v1/reports/asset/{id}       → JSON per-asset report
//   GET /reports/                        → raw: index.html, <name>.html|.csv|.json, asset/<id>.html; `?format=html|csv|json`
// Both prefixes sit behind the same bearer token (settings rest.token). /api/ is gated by RestApiServer; the raw /reports/ handler
// checks the token itself (`?token=`, browsers cannot send a header on navigation): the .json / .csv mirrors answer with the same
// as-run history, client names, contract delivery and local file paths as the gated API, so they must not be open. Report pages carry
// the token on their own links and forms (reports.cpp kJs); the dashboard (web/reports.js) and the operator app (ReportsView.qml)
// append it to the URLs they open. With no token configured nothing is gated, as before.
#include "supervisorcore.h"
#include "restapiserver.h"
#include "reports.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QRegularExpression>

using namespace sched;
using Q = QHash<QString, QString>;

namespace {
/// Write one complete HTTP response on a raw socket and close it. No CORS header: report pages are navigation targets, never XHR
/// targets, and `Access-Control-Allow-Origin: *` would let any web page open in an operator's browser read them cross-origin.
void sendRaw(QTcpSocket* sock, int status, const QByteArray& contentType, const QByteArray& body, const QByteArray& extraHeaders = QByteArray()) {
    static const QHash<int, QByteArray> reasons = { {200, "OK"}, {400, "Bad Request"}, {401, "Unauthorized"}, {404, "Not Found"}, {503, "Service Unavailable"} };
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + " " + reasons.value(status, "OK") + "\r\n";
    h += "Content-Type: " + contentType + "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n" + extraHeaders;   // the page URL may carry ?token=: never leak it in a Referer
    h += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
    sock->write(h + body); sock->flush(); sock->disconnectFromHost();
    QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
    if (sock->state() == QAbstractSocket::UnconnectedState) sock->deleteLater();
}
/// Constant-time token comparison.
bool sameToken(const QString& presented, const QString& expected) {
    const QByteArray a = presented.toUtf8(), b = expected.toUtf8();
    if (a.size() != b.size()) return false;
    int diff = 0; for (int i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}
}

void SupervisorCore::registerReportRoutes() {
    RestApiServer* r = m_rest.get();
    m_reports = std::make_unique<ReportBuilder>(m_store.get());
    ReportBuilder* rb = m_reports.get();
    // The same token RestApiServer::setAuthToken gets right after route registration (settings rest.token, read once at startup).
    const QString token = m_store->setting("rest.token");

    // --- JSON routes (one per report) -------------------------------------------------------------
    // htmlUrl / csvUrl echo the request's parameters but never its `token` (a credential has no business in a response body, a log or a cache).
    r->registerRoute("GET", "/api/v1/reports", [rb](const QJsonObject&, const Q&) { return rb->catalogue(); });
    for (const QString& name : ReportBuilder::names()) {
        if (name == "asset") continue;
        r->registerRoute("GET", "/api/v1/reports/" + name, [rb, name](const QJsonObject&, const Q& q) {
            QJsonObject o = rb->run(name, q);
            if (!o.contains("error")) { QString qs; for (auto it = q.constBegin(); it != q.constEnd(); ++it) if (it.key() != "format" && it.key() != "token") qs += (qs.isEmpty() ? "?" : "&") + QUrl::toPercentEncoding(it.key()) + "=" + QUrl::toPercentEncoding(it.value()); o["htmlUrl"] = "/reports/" + name + ".html" + qs; o["csvUrl"] = "/reports/" + name + ".csv" + qs; }
            return o;
        });
    }
    r->registerRoute("GET", "/api/v1/reports/asset/{id}", [rb](const QJsonObject&, const Q& q) {
        QJsonObject o = rb->run("asset", q);
        if (!o.contains("error")) { const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(q.value("id"))); o["htmlUrl"] = "/reports/asset/" + enc + ".html?days=" + QString::number(o.value("days").toInt()); o["csvUrl"] = "/reports/asset/" + enc + ".csv?days=" + QString::number(o.value("days").toInt()); }
        return o;
    });

    // --- raw HTML / CSV / JSON under /reports/ ---------------------------------------------------
    r->registerRaw("GET", "/reports/", [rb, token](QTcpSocket* sock, const QString& rawPath, const Q& q0) {
        if (!token.isEmpty() && !sameToken(q0.value("token"), token)) {
            sendRaw(sock, 401, "text/plain; charset=utf-8", "Unauthorized: bearer token required (append ?token=<token> to the report URL)"); return;
        }
        Q q = q0; QString path = rawPath;
        const int qi = path.indexOf('?');                       // defensive: the server already splits the query, but handle both
        if (qi >= 0) { for (const auto& item : QUrlQuery(path.mid(qi + 1)).queryItems(QUrl::FullyDecoded)) if (!q.contains(item.first)) q[item.first] = item.second; path = path.left(qi); }
        q.remove("token");                                      // checked above; never part of a report's parameters
        QString rest = QUrl::fromPercentEncoding(path.mid(QString("/reports/").size()).toUtf8());
        if (rest.endsWith(".html")) rest.chop(5); else if (rest.endsWith(".csv")) { rest.chop(4); q["format"] = "csv"; } else if (rest.endsWith(".json")) { rest.chop(5); q["format"] = "json"; }
        QString name = rest;
        if (name.startsWith("asset/")) { q["id"] = name.mid(6); name = "asset"; }
        if (name.isEmpty() || name == "index") { sendRaw(sock, 200, "text/html; charset=utf-8", rb->indexHtml()); return; }
        if (!ReportBuilder::names().contains(name)) { sendRaw(sock, 404, "text/plain; charset=utf-8", QByteArray("unknown report: ") + name.toUtf8() + "\nknown: " + ReportBuilder::names().join(", ").toUtf8()); return; }
        const QJsonObject data = rb->run(name, q);
        const QString format = q.value("format", "html").toLower();
        const int status = data.contains("error") ? qMax(400, data.value("status").toInt(400)) : 200;
        if (format == "json") { sendRaw(sock, status, "application/json; charset=utf-8", QJsonDocument(data).toJson(QJsonDocument::Compact)); return; }
        if (format == "csv") {
            QString fn = name; if (name == "asset") fn += "_" + q.value("id"); fn += "_" + (data.contains("from") ? data.value("from").toString() : data.value("date").toString(data.value("asOf").toString()));
            fn.replace(QRegularExpression("[^A-Za-z0-9_.-]+"), "_");
            sendRaw(sock, status, "text/csv; charset=utf-8", rb->csv(name, data), "Content-Disposition: attachment; filename=\"" + fn.toUtf8() + ".csv\"\r\n"); return;
        }
        sendRaw(sock, status, "text/html; charset=utf-8", rb->html(name, data));
    });
    qInfo() << "[Reports] routes registered: /api/v1/reports/* and /reports/*" << (token.isEmpty() ? "(open: no rest.token)" : "(bearer token required)");
}
