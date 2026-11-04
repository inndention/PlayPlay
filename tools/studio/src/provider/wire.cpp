#include "provider/wire.h"

#include "core/clock.h"
#include "core/config.h"
#include "core/json.h"
#include "core/log.h"
#include "provider/limits.h"
#include "provider/modelhealth.h"
#include "provider/payload.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <algorithm>
#include <cmath>
#include <mutex>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#include <QSslSocket>
#endif

namespace studio::provider {

namespace {

constexpr QStringView kCat = u"openrouter";

// ECMAScript WhiteSpace + LineTerminator (String.prototype.trim and \s)
bool jsSpace(char16_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000
        || c == 0xFEFF;
}

// `.replace(/\s+/g, ' ').trim()`
QString jsCollapse(QStringView s) {
    QString out;
    out.reserve(s.size());
    bool inSpace = false;
    for (QChar c : s) {
        if (jsSpace(c.unicode())) { inSpace = true; continue; }
        if (inSpace && !out.isEmpty()) out += QLatin1Char(' ');
        inSpace = false;
        out += c;
    }
    return out;
}

QString jsTrim(QStringView s) {
    qsizetype a = 0, b = s.size();
    while (a < b && jsSpace(s[a].unicode())) ++a;
    while (b > a && jsSpace(s[b - 1].unicode())) --b;
    return s.mid(a, b - a).toString();
}

QJsonValue member(const QJsonValue& v, QLatin1StringView key) {
    return v.isObject() ? v.toObject().value(key) : QJsonValue(QJsonValue::Undefined);
}
bool nullish(const QJsonValue& v) { return v.isUndefined() || v.isNull(); }

// JS String(v) for what can sit in error.message
QString jsStringOf(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return json::jsNumber(v.toDouble());
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isNull()) return QStringLiteral("null");
    if (v.isObject()) return QStringLiteral("[object Object]");
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue& x : v.toArray()) parts << (nullish(x) ? QString() : jsStringOf(x));
        return parts.join(QLatin1Char(','));
    }
    return QStringLiteral("undefined");
}

QByteArray headerOf(const HeaderList& h, const QByteArray& name) {
    QByteArray out;
    bool any = false;
    for (const auto& p : h) {
        if (p.first.compare(name, Qt::CaseInsensitive) != 0) continue;
        if (any) out += ", ";
        out += p.second;
        any = true;
    }
    return out;
}

bool isHttpUrl(QStringView p) {
    return p.startsWith(u"http://", Qt::CaseInsensitive) || p.startsWith(u"https://", Qt::CaseInsensitive);
}

QString originOf(const QUrl& u) {
    const int port = u.port(u.scheme().compare(QLatin1StringView("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
    return u.scheme().toLower() + QStringLiteral("://") + u.host().toLower() + QLatin1Char(':') + QString::number(port);
}

QString envText(const char* name) { return jsTrim(qEnvironmentVariable(name)); }
QString num(qint64 v) { return QString::number(v); }

// ---------------------------------------------------------------------------------------------------------------
// Transports. A Transfer sends one request once and reports exactly one outcome, unless abort() came first (then none).

class Transfer : public QObject {
public:
    std::function<void(Response)> onDone;
    std::function<void(const QString& why, bool sent)> onFail;
    virtual void start() = 0;
    /// stops the request; no callback runs afterwards
    virtual void abort() = 0;
    /// a request byte may have left this process
    virtual bool sent() const = 0;

protected:
    void done(Response r) {
        if (m_over) return;
        m_over = true;
        if (onDone) onDone(std::move(r));
    }
    void failed(const QString& why) {
        if (m_over) return;
        m_over = true;
        if (onFail) onFail(why, sent());
    }
    bool m_over = false;
};

// Everything that may be sent more than once safely: QNAM (keep-alive, HTTP/2 when offered, redirects).
class QnamTransfer final : public Transfer {
public:
    QnamTransfer(QNetworkAccessManager* nam, const QUrl& url, const QByteArray& method, const std::optional<QByteArray>& body,
                 const HeaderList& headers, qint64 maxBytes, bool keyed)
        : m_nam(nam), m_url(url), m_method(method), m_body(body), m_headers(headers), m_max(maxBytes), m_keyed(keyed) {}
    ~QnamTransfer() override { abort(); }

    void start() override {
        QNetworkRequest rq(m_url);
        for (const auto& h : m_headers) rq.setRawHeader(h.first, h.second);
        rq.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
        // a request that carries a key never follows a redirect to another origin (the key would go with it)
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                        m_keyed ? QNetworkRequest::SameOriginRedirectPolicy : QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        rq.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        m_reply = m_body ? m_nam->sendCustomRequest(rq, m_method, *m_body) : m_nam->sendCustomRequest(rq, m_method);
        QObject::connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
            if (got > m_max || total > m_max) {
                const QString why = QStringLiteral("the answer is larger than %1").arg(payload::fmtMB(m_max));
                drop();
                failed(why);
            }
        });
        QObject::connect(m_reply, &QNetworkReply::finished, this, [this]() { finished(); });
    }
    void abort() override {
        m_over = true;
        drop();
    }
    bool sent() const override { return true; }   // QNAM does not say; treat any start as sent

private:
    void drop() {
        if (!m_reply) return;
        QNetworkReply* r = m_reply;
        m_reply = nullptr;
        QObject::disconnect(r, nullptr, this, nullptr);
        r->abort();
        r->deleteLater();
    }
    void finished() {
        QNetworkReply* r = m_reply;
        if (!r) return;
        m_reply = nullptr;
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const int err = int(r->error());
        // errors 1-199 (connection, proxy) and 301-399 (protocol) are transport failures even with a status (a body cut
        // short); 201-299 and 401-499 only mirror an HTTP status, whose answer is read like any other
        const bool transport = status <= 0 || (err >= 1 && err <= 199) || (err >= 301 && err <= 399);
        if (transport) {
            failed(r->errorString());
            return;
        }
        Response out;
        out.status = status;
        out.headers = r->rawHeaderPairs();
        out.body = r->readAll();
        out.http2 = r->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool();
        done(std::move(out));
    }

    QNetworkAccessManager* m_nam;
    QUrl m_url;
    QByteArray m_method;
    std::optional<QByteArray> m_body;
    HeaderList m_headers;
    qint64 m_max;
    bool m_keyed;
    QNetworkReply* m_reply = nullptr;
};

// Paid POSTs: one HTTP/1.1 request on a connection of its own (Connection: close). Nothing here ever writes the request
// a second time, whatever the connection does.
class OneShotTransfer final : public Transfer {
public:
    OneShotTransfer(const QUrl& url, const QByteArray& method, const std::optional<QByteArray>& body, const HeaderList& headers,
                    qint64 maxBytes)
        : m_url(url), m_method(method), m_body(body), m_headers(headers), m_max(maxBytes) {}
    ~OneShotTransfer() override { abort(); }

    void start() override {
        const bool tls = m_url.scheme().compare(QLatin1StringView("https"), Qt::CaseInsensitive) == 0;
        const QString host = m_url.host();
        const quint16 port = quint16(m_url.port(tls ? 443 : 80));
        if (tls) {
#if QT_CONFIG(ssl)
            if (!QSslSocket::supportsSsl()) { later(QStringLiteral("TLS is not available in this build (no TLS backend)")); return; }
            auto* s = new QSslSocket(this);
            QSslConfiguration conf = s->sslConfiguration();
            conf.setAllowedNextProtocols({QByteArrayLiteral("http/1.1")});
            s->setSslConfiguration(conf);
            m_socket = s;
            wire();
            QObject::connect(s, &QSslSocket::encrypted, this, [this]() { write(); });
            s->connectToHostEncrypted(host, port);   // the peer is verified (VerifyPeer, the host name), errors abort
#else
            later(QStringLiteral("TLS is not available in this build"));
#endif
            return;
        }
        m_socket = new QTcpSocket(this);
        wire();
        QObject::connect(m_socket, &QTcpSocket::connected, this, [this]() { write(); });
        m_socket->connectToHost(host, port);
    }
    void abort() override {
        m_over = true;
        if (m_socket) {
            QObject::disconnect(m_socket, nullptr, this, nullptr);
            m_socket->abort();
            m_socket->deleteLater();
            m_socket = nullptr;
        }
    }
    bool sent() const override { return m_sent; }

private:
    enum class Framing { None, Length, Chunked, UntilClose };

    void later(const QString& why) {
        QMetaObject::invokeMethod(this, [this, why]() { failed(why); }, Qt::QueuedConnection);
    }
    void wire() {
        QObject::connect(m_socket, &QTcpSocket::readyRead, this, [this]() { read(); });
        QObject::connect(m_socket, &QTcpSocket::disconnected, this, [this]() { closed(); });
        QObject::connect(m_socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
            if (e == QAbstractSocket::RemoteHostClosedError) { closed(); return; }
            fail(m_socket ? m_socket->errorString() : QStringLiteral("connection error"));
        });
    }
    void write() {
        if (m_over || !m_socket) return;
        QByteArray target = m_url.path(QUrl::FullyEncoded).toUtf8();
        if (target.isEmpty()) target = "/";
        if (m_url.hasQuery()) target += '?' + m_url.query(QUrl::FullyEncoded).toUtf8();
        const bool tls = m_url.scheme().compare(QLatin1StringView("https"), Qt::CaseInsensitive) == 0;
        QByteArray hostHeader = m_url.host(QUrl::FullyEncoded).toUtf8();
        if (hostHeader.contains(':')) hostHeader = '[' + hostHeader + ']';
        if (m_url.port() > 0 && m_url.port() != (tls ? 443 : 80)) hostHeader += ':' + QByteArray::number(m_url.port());
        QByteArray head = m_method + ' ' + target + " HTTP/1.1\r\nHost: " + hostHeader + "\r\nConnection: close\r\n";
        for (const auto& h : m_headers) head += h.first + ": " + h.second + "\r\n";
        if (m_body || m_method == "POST" || m_method == "PUT") head += "Content-Length: " + QByteArray::number(m_body ? m_body->size() : 0) + "\r\n";
        head += "\r\n";
        m_sent = true;   // from here on the request may reach the server: it is never written again
        m_socket->write(head);
        if (m_body && !m_body->isEmpty()) m_socket->write(*m_body);
        m_body.reset();  // the copy is in the socket's buffer now
    }
    void fail(const QString& why) {
        if (m_over) return;
        abortSocket();
        failed(why);
    }
    void abortSocket() {
        if (!m_socket) return;
        QTcpSocket* s = m_socket;
        m_socket = nullptr;
        QObject::disconnect(s, nullptr, this, nullptr);
        s->abort();
        s->deleteLater();
    }
    void complete() {
        if (m_over) return;
        abortSocket();
        Response r;
        r.status = m_status;
        r.headers = m_respHeaders;
        r.body = std::move(m_respBody);
        done(std::move(r));
    }
    void closed() {
        if (m_over) return;
        if (m_socket) m_buf += m_socket->readAll();
        parse();
        if (m_over) return;
        if (m_headDone && m_framing == Framing::UntilClose) { complete(); return; }
        fail(m_headDone ? QStringLiteral("the connection closed before the answer was complete")
                        : QStringLiteral("the connection closed without an answer"));
    }
    void read() {
        if (m_over || !m_socket) return;
        m_buf += m_socket->readAll();
        parse();
    }
    bool bodyFull(qint64 more) {
        if (m_respBody.size() + more <= m_max) return false;
        fail(QStringLiteral("the answer is larger than %1").arg(payload::fmtMB(m_max)));
        return true;
    }
    void parse() {
        while (!m_over) {
            if (!m_headDone) {
                const qsizetype end = m_buf.indexOf("\r\n\r\n");
                if (end < 0) {
                    if (m_buf.size() > 64 * 1024) fail(QStringLiteral("the answer's head is too large"));
                    return;
                }
                const QList<QByteArray> lines = m_buf.left(end).split('\n');
                m_buf.remove(0, end + 4);
                const QList<QByteArray> sl = lines.value(0).trimmed().split(' ');
                bool ok = false;
                const int status = sl.value(1).toInt(&ok);
                if (!sl.value(0).startsWith("HTTP/1.") || !ok || status < 100 || status > 999) {
                    fail(QStringLiteral("not an HTTP/1.1 answer"));
                    return;
                }
                HeaderList hs;
                for (qsizetype i = 1; i < lines.size(); ++i) {
                    const QByteArray l = lines[i].trimmed();
                    const qsizetype c = l.indexOf(':');
                    if (c > 0) hs.push_back({l.left(c).trimmed(), l.mid(c + 1).trimmed()});
                }
                if (status < 200) continue;   // 100 Continue, 103 Early Hints: the real answer follows
                m_status = status;
                m_respHeaders = hs;
                m_headDone = true;
                const QByteArray te = headerOf(hs, "transfer-encoding").toLower();
                const QByteArray cl = headerOf(hs, "content-length");
                if (m_method == "HEAD" || status == 204 || status == 304) m_framing = Framing::None;
                else if (te.contains("chunked")) m_framing = Framing::Chunked;
                else if (!cl.isEmpty()) {
                    bool lok = false;
                    m_left = cl.trimmed().toLongLong(&lok);
                    if (!lok || m_left < 0 || cl.contains(',')) { fail(QStringLiteral("bad Content-Length in the answer")); return; }
                    if (bodyFull(m_left)) return;
                    m_framing = Framing::Length;
                } else m_framing = Framing::UntilClose;
                if (m_framing == Framing::None) { complete(); return; }
                continue;
            }
            switch (m_framing) {
            case Framing::Length: {
                const qint64 n = std::min<qint64>(m_left, m_buf.size());
                m_respBody += m_buf.left(n);
                m_buf.remove(0, n);
                m_left -= n;
                if (m_left == 0) complete();
                return;
            }
            case Framing::UntilClose:
                if (bodyFull(m_buf.size())) return;
                m_respBody += m_buf;
                m_buf.clear();
                return;
            case Framing::Chunked: {
                if (m_chunkLeft > 0) {
                    const qint64 n = std::min<qint64>(m_chunkLeft, m_buf.size());
                    if (bodyFull(n)) return;
                    m_respBody += m_buf.left(n);
                    m_buf.remove(0, n);
                    m_chunkLeft -= n;
                    if (m_chunkLeft > 0) return;
                    m_chunkCrlf = true;
                }
                if (m_chunkCrlf) {
                    if (m_buf.size() < 2) return;
                    if (!m_buf.startsWith("\r\n")) { fail(QStringLiteral("bad chunk framing in the answer")); return; }
                    m_buf.remove(0, 2);
                    m_chunkCrlf = false;
                }
                const qsizetype eol = m_buf.indexOf("\r\n");
                if (eol < 0) {
                    if (m_buf.size() > 8192) fail(QStringLiteral("bad chunk framing in the answer"));
                    return;
                }
                const QByteArray line = m_buf.left(eol);
                if (m_trailers) {
                    m_buf.remove(0, eol + 2);
                    if (line.isEmpty()) { complete(); return; }
                    continue;
                }
                bool ok = false;
                const qint64 size = line.split(';').value(0).trimmed().toLongLong(&ok, 16);
                if (!ok || size < 0) { fail(QStringLiteral("bad chunk size in the answer")); return; }
                m_buf.remove(0, eol + 2);
                if (size == 0) { m_trailers = true; continue; }
                m_chunkLeft = size;
                continue;
            }
            case Framing::None:
                return;
            }
        }
    }

    QUrl m_url;
    QByteArray m_method;
    std::optional<QByteArray> m_body;
    HeaderList m_headers;
    qint64 m_max;
    QTcpSocket* m_socket = nullptr;
    bool m_sent = false;
    QByteArray m_buf;
    bool m_headDone = false;
    int m_status = 0;
    HeaderList m_respHeaders;
    QByteArray m_respBody;
    Framing m_framing = Framing::None;
    qint64 m_left = 0;
    qint64 m_chunkLeft = 0;
    bool m_chunkCrlf = false;
    bool m_trailers = false;
};

} // namespace

// the abort listener's way back to the Wire, cleared when the Wire goes (a listener may run on any thread)
struct detail::WireLink {
    std::mutex m;
    Wire* wire = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------

QByteArray Response::header(const QByteArray& name) const { return headerOf(headers, name); }

QJsonValue Response::json() const { return json::parse(body); }

QString WireError::kindName() const {
    switch (kind) {
    case Kind::Http: return QStringLiteral("http");
    case Kind::Limit: return QStringLiteral("limit");
    case Kind::TooLarge: return QStringLiteral("too_large");
    case Kind::Timeout: return QStringLiteral("timeout");
    case Kind::Network: return QStringLiteral("network");
    case Kind::Cancelled: return QStringLiteral("cancelled");
    case Kind::Invalid: return QStringLiteral("invalid");
    case Kind::Refused: return QStringLiteral("refused");
    }
    return QStringLiteral("network");
}

ProviderError WireError::toProviderError() const {
    ProviderError p;
    p.name = kind == Kind::Cancelled ? QStringLiteral("AbortError")
        : kind == Kind::Limit        ? QStringLiteral("ProviderLimitError")
        : status > 0                 ? QStringLiteral("OpenRouterError")
                                     : QStringLiteral("Error");
    if (status > 0) p.status = status;
    p.message = message;
    p.providerMessage = providerMessage;
    if (limit) {
        LimitVerdict v;
        v.kind = limit->kind;
        if (limit->retryAfterMs && *limit->retryAfterMs > 0) v.retryAfterMs = limit->retryAfterMs;
        v.model = limit->model;
        v.remembered = limit->remembered;
        p.verdict = v;
    }
    return p;
}

QJsonObject WireError::toJson() const {
    QJsonObject o{{QStringLiteral("kind"), kindName()}, {QStringLiteral("message"), message}, {QStringLiteral("attempts"), attempts}};
    if (status) o.insert(QStringLiteral("status"), status);
    if (!provider.isEmpty()) o.insert(QStringLiteral("provider"), provider);
    if (!providerMessage.isEmpty()) o.insert(QStringLiteral("providerMessage"), providerMessage);
    if (outcomeUnknown) o.insert(QStringLiteral("outcomeUnknown"), true);
    if (sentNothing) o.insert(QStringLiteral("sentNothing"), true);
    if (!safetyCode.isEmpty()) o.insert(QStringLiteral("safetyCode"), safetyCode);
    if (limit) {
        QJsonObject l{{QStringLiteral("kind"), limitKindName(limit->kind)}, {QStringLiteral("retries"), limit->retries}};
        if (!limit->gate.isEmpty()) l.insert(QStringLiteral("gate"), limit->gate);
        if (!limit->model.isEmpty()) l.insert(QStringLiteral("model"), limit->model);
        if (limit->retryAfterMs) l.insert(QStringLiteral("retryAfterMs"), double(*limit->retryAfterMs));
        if (!limit->limitProvider.isEmpty()) l.insert(QStringLiteral("provider"), limit->limitProvider);
        if (limit->remembered) l.insert(QStringLiteral("remembered"), true);
        o.insert(QStringLiteral("limit"), l);
    }
    return o;
}

ProviderDetail providerDetail(const QJsonValue& body) {
    const QJsonValue md = member(member(body, QLatin1StringView("error")), QLatin1StringView("metadata"));
    if (!md.isObject()) return {};
    ProviderDetail d;
    if (const QJsonValue pn = member(md, QLatin1StringView("provider_name")); pn.isString() && !jsTrim(pn.toString()).isEmpty())
        d.provider = jsTrim(pn.toString()).left(60);
    QJsonValue raw = member(md, QLatin1StringView("raw"));
    if (raw.isString()) {
        const QJsonValue parsed = json::parse(raw.toString().toUtf8());
        if (!parsed.isUndefined()) raw = parsed;
    }
    // pick(o): a string as is; an object -> pick(o.error?.message ?? o.error ?? o.message ?? o.detail ?? o.msg) ??
    // JSON.stringify(o); anything else (numbers, too deep) -> nothing
    std::function<std::optional<QString>(const QJsonValue&, int)> pick = [&](const QJsonValue& o, int depth) -> std::optional<QString> {
        if (o.isString()) return o.toString();
        if (!(o.isObject() || o.isArray()) || depth > 3) return std::nullopt;
        QJsonValue next = member(member(o, QLatin1StringView("error")), QLatin1StringView("message"));
        for (QLatin1StringView k : {QLatin1StringView("error"), QLatin1StringView("message"), QLatin1StringView("detail"), QLatin1StringView("msg")})
            if (nullish(next)) next = member(o, k);
        if (auto s = pick(next, depth + 1)) return s;
        return QString::fromUtf8(json::compact(o));
    };
    if (auto t = pick(raw, 0)) {
        const QString text = jsCollapse(*t);
        if (!text.isEmpty()) d.raw = text.size() > 300 ? text.left(299) + QChar(0x2026) : text;
    }
    return d;
}

QString fmtWait(qint64 ms) {
    auto round = [](double x) { return qint64(std::floor(x + 0.5)); };   // Math.round
    if (ms < 90000) return QStringLiteral("%1 s").arg(std::max<qint64>(1, round(ms / 1000.0)));
    if (ms < 90LL * 60000) return QStringLiteral("%1 min").arg(round(ms / 60000.0));
    return QStringLiteral("%1 h").arg(round(ms / 3600000.0));
}

QString limitMessage(int status, const LimitSignal& sig, const QString& model, int retries, bool remembered, bool asked) {
    if (sig.kind == LimitKind::Cap) return QStringLiteral("%1 %2").arg(status).arg(sig.message);   // StarMedia's own cap words it better
    const QString who = sig.provider.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(sig.provider);
    const QString mdl = model.isEmpty() ? QString() : QStringLiteral(" for %1").arg(model);
    const QString said = sig.message.isEmpty() ? QString() : QStringLiteral(" The provider said: %1").arg(sig.message);
    const QString when = sig.retryAfterMs && *sig.retryAfterMs ? QStringLiteral(" It asks for %1.").arg(fmtWait(*sig.retryAfterMs)) : QString();
    if (sig.kind == LimitKind::Quota)
        return QStringLiteral("%1 The provider is limiting us%2: the daily quota%3 is used up, so retrying will not help today and it was not "
                              "retried%4.%5 The fallback models a step lists are tried automatically; otherwise choose another model for it.%6")
            .arg(status).arg(who, mdl, remembered ? QStringLiteral(" (an earlier answer said so; nothing was sent now)") : QString(), when, said);
    const QString tried = retries ? QStringLiteral(" and retried %1 time%2").arg(retries).arg(retries == 1 ? QString() : QStringLiteral("s")) : QString();
    return QStringLiteral("%1 The provider is limiting us%2: too many requests%3. The studio slowed down%4%5; try again in a moment.%6%7")
        .arg(status).arg(who, mdl, tried, asked ? QStringLiteral(" (the wait it asks for is too long to sit through here)") : QString(), when, said);
}

QString tooLargeMessage(QStringView path, qint64 bytes, const QString& upstream) {
    const QString size = bytes ? QStringLiteral(" (this request was %1)").arg(payload::fmtMB(bytes)) : QString();
    if (upstream.contains(QLatin1StringView("post_max_size"), Qt::CaseInsensitive) || upstream.contains(QLatin1StringView("web host"), Qt::CaseInsensitive))
        return upstream;
    return QStringLiteral("Request too large%1 for %2: OpenRouter accepts at most %3 per request here, and models accept much less per image "
                          "or audio file. Use smaller images or shorter audio (the studio shrinks images and splits long audio automatically; "
                          "if you still see this, the file is unusually large). Details: %4")
        .arg(size, path.toString(), payload::fmtMB(payload::bodyCap(path).cap), upstream);
}

// ---------------------------------------------------------------------------------------------------------------

struct Wire::Call {
    quint64 id = 0;
    Request rq;
    QPointer<QObject> ctx;
    bool hadCtx = false;
    Done done;
    Failed failed;
    QUrl url;
    QString dpath;                       // what diagnostics and messages show: the path, never a query string
    QString base;                        // another provider's base (empty = OpenRouter)
    std::optional<GateKind> gate;
    bool paid = false;
    bool oneShot = false;
    qint64 timeoutMs = 0;
    qint64 bodyBytes = 0;
    int attempt = 0;
    int sends = 0;
    bool busyRetried = false;
    bool finished = false;
    bool timedOut = false;
    Slot slot;
    Transfer* transfer = nullptr;        // owned by the Wire (parent), deleteLater when done
    Clock::TimerId timer = 0;
    Clock::TimerId sleep = 0;
    AbortSignal::ListenerId listener = 0;
    qint64 t0 = 0;
    qint64 sendT0 = 0;
};

Wire::Wire(Config& config, Gates& gates, Clock& clock, QObject* parent)
    : QObject(parent), m_config(config), m_gates(gates), m_clock(clock), m_nam(new QNetworkAccessManager(this)),
      m_link(std::make_shared<detail::WireLink>()) {
    m_link->wire = this;
    m_nam->setAutoDeleteReplies(false);
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

Wire::~Wire() {
    {
        std::lock_guard<std::mutex> g(m_link->m);
        m_link->wire = nullptr;
    }
    const auto calls = m_calls;
    for (const auto& c : calls) {
        if (c->transfer) { c->transfer->abort(); c->transfer = nullptr; }
        c->finished = true;
        if (c->listener) c->rq.signal.removeListener(c->listener);
        c->slot.release();
    }
    m_calls.clear();
}

void Wire::setOptions(const WireOptions& o) { m_options = o; }

QString Wire::baseUrl() const {
    QString b = m_options.baseUrl.isEmpty() ? m_config.openRouterBaseUrl() : m_options.baseUrl;
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b;
}

QString Wire::providerBase(QStringView model) const {
    const qsizetype slash = model.indexOf(u'/');
    if (slash <= 0) return {};
    QString b = m_options.providers.value(model.left(slash).toString());
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b;
}

double Wire::rnd() const {
    return m_options.rand ? m_options.rand() : QRandomGenerator::global()->generateDouble();
}

QJsonObject Wire::stats() const {
    return QJsonObject{{QStringLiteral("calls"), double(m_stats.calls)},
                       {QStringLiteral("sent"), double(m_stats.sent)},
                       {QStringLiteral("retries"), double(m_stats.retries)},
                       {QStringLiteral("refusedBeforeSend"), double(m_stats.refused)},
                       {QStringLiteral("refusedBySafety"), double(m_stats.safety)},
                       {QStringLiteral("unknownOutcome"), double(m_stats.unknown)},
                       {QStringLiteral("cancelled"), double(m_stats.cancelled)},
                       {QStringLiteral("http2"), double(m_stats.http2)},
                       {QStringLiteral("http1"), double(m_stats.http1)},
                       {QStringLiteral("oneShot"), double(m_stats.oneShot)},
                       {QStringLiteral("inFlight"), int(m_calls.size())}};
}

quint64 Wire::request(Request rq, QObject* context, Done done, Failed failed) {
    auto c = std::make_shared<Call>();
    c->id = ++m_nextId;
    c->rq = std::move(rq);
    c->ctx = context;
    c->hadCtx = context != nullptr;
    c->done = std::move(done);
    c->failed = std::move(failed);
    c->t0 = m_clock.steadyMs();
    m_calls.insert(c->id, c);
    ++m_stats.calls;
    begin(c);
    return c->id;
}

void Wire::begin(const std::shared_ptr<Call>& c) {
    Request& rq = c->rq;
    rq.method = rq.method.toUpper();
    c->base = providerBase(rq.model);
    const bool absolute = isHttpUrl(rq.path);
    c->url = absolute ? QUrl(rq.path) : QUrl((c->base.isEmpty() ? baseUrl() : c->base) + rq.path);
    c->dpath = absolute ? log::urlForLog(c->url) : rq.path.section(QLatin1Char('?'), 0, 0);
    if ((!absolute && !rq.path.startsWith(QLatin1Char('/'))) || !c->url.isValid() || c->url.host().isEmpty()
        || !(c->url.scheme() == QLatin1StringView("http") || c->url.scheme() == QLatin1StringView("https"))) {
        WireError e;
        e.kind = WireError::Kind::Invalid;
        e.message = QStringLiteral("not a usable request URL for %1").arg(c->dpath);
        e.sentNothing = true;
        fail(c, e);
        return;
    }
    c->gate = gateFor(QString::fromLatin1(rq.method), rq.path);
    c->paid = c->gate && (*c->gate == GateKind::Videos || *c->gate == GateKind::Images || *c->gate == GateKind::Audio);
    c->oneShot = c->paid && m_options.paidOneShot;
    c->timeoutMs = std::max<qint64>(1, m_options.maxTimeoutMs > 0 ? std::min(rq.timeoutMs, m_options.maxTimeoutMs) : rq.timeoutMs);
    // the model whose "daily quota is used up" answer is remembered (a local / pool provider has no such quota)
    if (!c->base.isEmpty()) rq.quotaKey.clear();
    c->bodyBytes = rq.body ? rq.body->size() : 0;

    // refuse before uploading tens of megabytes that OpenRouter will reject anyway
    // (log lines use the multi-argument arg(): a path may hold "%2F", which a chained arg() would replace)
    const payload::BodyCap cap = payload::bodyCap(rq.path);
    if (rq.body && c->bodyBytes > cap.safe) {
        log::warn(kCat, QStringLiteral("%1 -> 413 refused before sending (bytes=%2 cap=%3)").arg(c->dpath, num(c->bodyBytes), num(cap.safe)));
        WireError e;
        e.kind = WireError::Kind::TooLarge;
        e.status = 413;
        e.message = QStringLiteral("413 ") + tooLargeMessage(c->dpath, c->bodyBytes, QStringLiteral("refused before sending"));
        e.sentNothing = true;
        ++m_stats.refused;
        fail(c, e);
        return;
    }
    // a provider's own whole-request cap (Sourceful: 4.5 MB): OpenRouter would forward it and the provider refuse it
    if (const auto own = limits::requestCapFor(rq.model); rq.body && own && c->bodyBytes > *own) {
        log::warn(kCat, QStringLiteral("%1 -> 413 refused before sending: over %2's request cap (bytes=%3 cap=%4)")
                            .arg(c->dpath, rq.model, num(c->bodyBytes), num(*own)));
        WireError e;
        e.kind = WireError::Kind::TooLarge;
        e.status = 413;
        e.message = QStringLiteral("413 Request too large (this request was %1) for %2: %3 accepts at most %4 per request (the provider's "
                                   "own limit). Use smaller or fewer images. Details: refused before sending")
                        .arg(payload::fmtMB(c->bodyBytes), c->dpath, rq.model, payload::fmtMB(*own));
        e.sentNothing = true;
        ++m_stats.refused;
        fail(c, e);
        return;
    }
    // the ST-15 provider hook: a picture / video / cloned-speech body is checked as it will be sent; a refusal never leaves
    // the process (after the size checks: an oversized body is refused anyway, and is not parsed for nothing)
    if (auto refused = safetyRefusal(c)) {
        fail(c, *refused);
        return;
    }
    log::debug(kCat, QStringLiteral("%1 %2 (bytes=%3 timeoutMs=%4 retries=%5%6)")
                         .arg(QString::fromLatin1(rq.method), c->dpath, num(c->bodyBytes), num(c->timeoutMs), num(rq.retries),
                              c->oneShot ? QStringLiteral(" one-shot") : QString()));
    // this model's quota is already known to be used up: say so now, without a request, a slot or a retry
    if (auto known = rememberedQuota(c)) {
        log::info(kCat, QStringLiteral("%1 -> refused locally: quota used up for %2").arg(c->dpath, rq.quotaKey));
        fail(c, *known);
        return;
    }
    // the Stop button, from any thread: hop back to this one
    if (rq.signal.valid()) {
        if (rq.signal.aborted()) { stop(c); return; }
        std::shared_ptr<detail::WireLink> link = m_link;
        const quint64 id = c->id;
        c->listener = rq.signal.onAbort([link, id]() {
            std::lock_guard<std::mutex> g(link->m);
            if (!link->wire) return;
            Wire* w = link->wire;
            QMetaObject::invokeMethod(w, [w, id]() {
                // a copy: stop() removes the call from m_calls, which would free a reference into the hash
                if (const std::shared_ptr<Call> c = w->m_calls.value(id)) w->stop(c);
            }, Qt::QueuedConnection);
        });
    }
    attempt(c);
}

std::optional<WireError> Wire::rememberedQuota(const std::shared_ptr<Call>& c) {
    if (c->rq.quotaKey.isEmpty()) return std::nullopt;
    const auto e = m_gates.breaker().check(c->rq.quotaKey);
    if (!e) return std::nullopt;
    LimitSignal sg;
    sg.kind = e->kind;
    sg.message = e->message;
    sg.provider = e->provider;
    sg.retryAfterMs = std::max<qint64>(0, e->untilSteadyMs - m_clock.steadyMs());
    WireError w;
    w.kind = WireError::Kind::Limit;
    w.status = 429;
    w.message = limitMessage(429, sg, c->rq.quotaKey, 0, true, false);
    w.sentNothing = c->sends == 0;
    LimitInfo li;
    li.kind = e->kind;
    li.model = c->rq.quotaKey;
    li.remembered = true;
    li.retryAfterMs = sg.retryAfterMs;
    w.limit = li;
    return w;
}

std::optional<WireError> Wire::safetyRefusal(const std::shared_ptr<Call>& c) {
    const Request& rq = c->rq;
    if (!rq.body) return std::nullopt;
    QString p = c->url.path().toLower();
    while (p.endsWith(QLatin1Char('/'))) p.chop(1);
    const QByteArray& b = *rq.body;
    // a key written with escapes ("modalities") is not found by the byte search: such a body is read in full
    const bool escaped = b.contains("\\u00");
    const bool visualPath = p.endsWith(u"/images") || p.endsWith(u"/images/generations") || p.endsWith(u"/images/edits")
        || p.endsWith(u"/images/variations") || p.endsWith(u"/videos");
    // only bodies that can ask for a picture, a video or cloned speech are parsed: a vision chat body can be tens of MB
    const bool read = visualPath
        || ((p.endsWith(u"/chat/completions") || p.endsWith(u"/responses")) && (escaped || b.contains("\"modalities\"") || b.contains("image_generation")))
        || (p.contains(u"/audio/") && (escaped || b.contains("\"input_references\"")));
    if (!read) return std::nullopt;
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(b, &pe);
    safety::Verdict v;
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (!visualPath) return std::nullopt;   // a chat / audio body that is not JSON asks for no picture the hook can read
        // a picture or video request whose prompt cannot be read is not sent (fail closed, like the hook itself)
        v.refuse = true;
        v.code = QStringLiteral("unreadable_body");
        v.reason = QStringLiteral("Refused: the picture / video request body is not a JSON object, so its prompt cannot be checked for real people.");
        log::warn(u"safety", QStringLiteral("blocked before send: %1 (unreadable body)").arg(c->dpath));
    } else {
        v = safety::checkOutgoing(c->url.path(), doc.object(), rq.safety);   // logs a refusal itself (never the body)
    }
    if (!v.refuse) return std::nullopt;
    WireError e;
    e.kind = WireError::Kind::Refused;
    e.message = v.reason;
    e.safetyCode = v.code;
    e.sentNothing = true;
    ++m_stats.safety;
    return e;
}

void Wire::attempt(const std::shared_ptr<Call>& c) {
    if (c->finished) return;
    if (c->rq.signal.aborted()) { stop(c); return; }
    if (!c->gate) { send(c); return; }
    Gates::Request g;
    g.kind = *c->gate;
    g.providerKey = providerKeyOf(c->rq.model);
    g.ownOnly = !c->base.isEmpty();
    g.signal = c->rq.signal;
    std::weak_ptr<Call> weak = c;
    // the timeout starts with the request, not while it waits in line
    m_gates.acquire(g, this,
                    [this, weak](Slot s) {
                        auto c = weak.lock();
                        if (!c || c->finished) return;   // dropping `s` gives the slot back
                        c->slot = std::move(s);
                        send(c);
                    },
                    [this, weak](const QString& why) {
                        auto c = weak.lock();
                        if (!c || c->finished) return;
                        if (c->rq.signal.aborted()) { stop(c); return; }
                        WireError e;
                        e.kind = WireError::Kind::Network;
                        e.message = QStringLiteral("the request gate refused %1: %2").arg(c->dpath, why);
                        fail(c, e);
                    });
}

HeaderList Wire::headersFor(const std::shared_ptr<Call>& c) const {
    HeaderList h;
    for (const auto& p : c->rq.headers) {
        static const QList<QByteArray> owned = {"authorization", "content-type", "content-length", "host", "connection", "transfer-encoding"};
        if (owned.contains(p.first.toLower())) continue;
        // the one-shot transport writes the head itself: a CR / LF / NUL (or a bad name) would split the request or
        // inject a header, so such a header is dropped (QNAM refuses them too; the name only is logged, never the value)
        const auto bad = [](const QByteArray& s) { return s.contains('\r') || s.contains('\n') || s.contains('\0'); };
        if (p.first.isEmpty() || bad(p.first) || bad(p.second) || p.first.contains(':') || p.first.contains(' ')) {
            log::warn(kCat, QStringLiteral("%1: header \"%2\" dropped (control characters)").arg(c->dpath, QString::fromLatin1(p.first.left(40)).simplified()));
            continue;
        }
        h.push_back(p);
    }
    const QString origin = originOf(c->url);
    if (!c->base.isEmpty()) {
        // another provider (local service, render pool): its own token if one is configured, never the OpenRouter key
        const QString tok = envText("STARMEDIA_PROVIDER_TOKEN");
        if (!tok.isEmpty() && origin == originOf(QUrl(c->base))) {
            log::addSecret(tok);
            h.push_back({"Authorization", "Bearer " + tok.toUtf8()});
        }
    } else if (origin == originOf(QUrl(baseUrl()))) {
        // the key is read now (env, then config) and goes only to the OpenRouter origin
        const Secret key = m_config.openRouterKey().key;
        if (!key.isEmpty()) h.push_back({"Authorization", key.bearer()});
        const QString referer = envText("OPENROUTER_REFERER");
        if (!referer.isEmpty()) h.push_back({"HTTP-Referer", referer.toUtf8()});
        const QString title = envText("OPENROUTER_TITLE");
        h.push_back({"X-Title", (title.isEmpty() ? m_options.title : title).toUtf8()});
    }
    if (c->rq.body) h.push_back({"Content-Type", c->rq.contentType});
    h.push_back({"User-Agent", QByteArrayLiteral("PlayPlayStudio/") + PLAYPLAY_STUDIO_VERSION});
    return h;
}

void Wire::send(const std::shared_ptr<Call>& c) {
    if (c->finished) return;
    // Stop pressed while the call waited in line, and the slot came just as it fired: nothing was sent, nothing to bill
    if (c->rq.signal.aborted()) { stop(c); return; }
    // another call found out, while this one waited in line, that the model's quota is used up: do not send this one
    if (auto known = rememberedQuota(c)) {
        c->slot.release();
        fail(c, *known);
        return;
    }
    const HeaderList headers = headersFor(c);
    const bool keyed = std::any_of(headers.begin(), headers.end(), [](const auto& p) { return p.first == "Authorization"; });
    Transfer* t = c->oneShot
        ? static_cast<Transfer*>(new OneShotTransfer(c->url, c->rq.method, c->rq.body, headers, c->rq.maxResponseBytes))
        : static_cast<Transfer*>(new QnamTransfer(m_nam, c->url, c->rq.method, c->rq.body, headers, c->rq.maxResponseBytes, keyed));
    t->setParent(this);
    c->transfer = t;
    c->timedOut = false;
    ++c->sends;
    ++m_stats.sent;
    if (c->oneShot) ++m_stats.oneShot;
    c->sendT0 = m_clock.steadyMs();
    std::weak_ptr<Call> weak = c;
    t->onDone = [this, weak, t](Response r) {
        auto c = weak.lock();
        t->deleteLater();
        if (!c || c->finished || c->transfer != t) return;
        c->transfer = nullptr;
        onAnswer(c, std::move(r));
    };
    t->onFail = [this, weak, t](const QString& why, bool sent) {
        auto c = weak.lock();
        t->deleteLater();
        if (!c || c->finished || c->transfer != t) return;
        c->transfer = nullptr;
        onBroken(c, why, sent);
    };
    // the timer covers the whole answer, body included
    c->timer = m_clock.callAfter(c->timeoutMs, this, [this, weak]() {
        auto c = weak.lock();
        if (!c || c->finished) return;
        c->timer = 0;
        c->timedOut = true;
        Transfer* tr = c->transfer;
        const bool sent = tr ? tr->sent() : false;
        c->transfer = nullptr;
        if (tr) { tr->abort(); tr->deleteLater(); }
        onBroken(c, QString(), sent);
    });
    t->start();
}

void Wire::noteProtocol(const QUrl& url, bool http2) {
    if (http2) ++m_stats.http2; else ++m_stats.http1;
    const QString key = originOf(url) + (http2 ? QStringLiteral(" h2") : QStringLiteral(" h1"));
    if (m_protocolSeen.contains(key)) return;
    m_protocolSeen.insert(key);
    log::info(kCat, QStringLiteral("%1 answered over %2 (Http2WasUsedAttribute=%3)")
                        .arg(log::urlForLog(url.adjusted(QUrl::RemovePath)), http2 ? QStringLiteral("HTTP/2") : QStringLiteral("HTTP/1.1"),
                             http2 ? QStringLiteral("true") : QStringLiteral("false")));
}

void Wire::onAnswer(const std::shared_ptr<Call>& c, Response r) {
    if (c->timer) { m_clock.cancel(c->timer); c->timer = 0; }
    r.attempts = c->sends;
    r.ms = m_clock.steadyMs() - c->sendT0;
    noteProtocol(c->url, r.http2);
    const int attempt = c->attempt;
    const QString gateName = c->gate ? provider::gateName(*c->gate) : QString();
    if (r.status >= 200 && r.status < 300) {
        // a full window of good answers gives the slots back one by one
        if (c->slot.valid()) c->slot.onSuccess();
        c->slot.release();
        log::debug(kCat, QStringLiteral("%1 <- %2 (attempt=%3 ms=%4 %5)")
                             .arg(c->dpath, num(r.status), num(attempt), num(r.ms), r.http2 ? QStringLiteral("h2") : QStringLiteral("http/1.1")));
        succeed(c, std::move(r));
        return;
    }
    const QString text = QString::fromUtf8(r.body);
    QJsonValue body = json::parse(r.body);
    if (body.isUndefined()) body = text;
    // a limit answer slows the gate down BEFORE the slot is given back: giving it back first would let the next call
    // in line out at the old, higher limit and without the pause, in the very moment the provider asked us to ease off
    const bool busy = r.status == 503 && !r.header("x-local-busy").isEmpty();
    const auto limit = busy ? std::nullopt : classifyLimit(r.status, r.headers, body, text, m_clock.nowMs());
    AdaptiveLimit::LimitResult eased;
    if (limit && c->slot.valid()) eased = c->slot.onLimit(*limit);
    const bool longAsk = limit && limit->kind == LimitKind::Rate && limit->retryAfterMs && *limit->retryAfterMs > kMaxInlineWaitMs;
    const QString& quotaKey = c->rq.quotaKey;
    // a used-up quota, or a wait too long to sit through, is remembered for this model so the next calls do not each find out
    if (limit && !quotaKey.isEmpty() && (limit->kind == LimitKind::Quota || longAsk))
        m_gates.breaker().trip(quotaKey, *limit, limit->retryAfterMs.value_or(kDefaultQuotaTtlMs));
    c->slot.release();
    const QJsonValue em = member(member(body, QLatin1StringView("error")), QLatin1StringView("message"));
    const QString top = nullish(em) ? text.left(300) : jsStringOf(em);
    // "Provider returned error" alone says nothing: append the upstream provider's name and its own message
    const ProviderDetail pd = providerDetail(body);
    const QString why = !pd.raw.isEmpty() && !top.contains(pd.raw)
        ? (pd.provider.isEmpty() ? QString() : pd.provider + QStringLiteral(": ")) + pd.raw
        : (!pd.provider.isEmpty() && !top.contains(pd.provider) ? pd.provider : QString());
    const QString raw = why.isEmpty() ? top : QStringLiteral("%1 (%2)").arg(top, why);
    const QString msg = r.status == 413 ? tooLargeMessage(c->dpath, c->bodyBytes, raw)
        : busy ? QStringLiteral("The local server is busy (every generation slot is taken); nothing was sent or charged. Try again in a "
                                "moment. (%1)").arg(raw.left(160))
               : raw;
    if (limit) {
        const bool final = limit->kind != LimitKind::Rate || longAsk || attempt >= c->rq.retries;
        WireError e;
        e.kind = WireError::Kind::Limit;
        e.status = r.status;
        e.message = limitMessage(r.status, *limit, quotaKey, attempt, false, longAsk);
        e.body = body;
        e.provider = pd.provider;
        e.providerMessage = pd.raw;
        LimitInfo li;
        li.kind = limit->kind;
        li.gate = gateName;
        li.model = quotaKey;
        li.retryAfterMs = limit->retryAfterMs;
        li.limitProvider = limit->provider;
        li.retries = attempt;
        e.limit = li;
        log::warn(kCat, QStringLiteral("%1 -> %2 (%3 limit) attempt=%4 retry=%5 ms=%6 gate=%7 retryAfterMs=%8 cut=%9")
                            .arg(c->dpath, num(r.status), limitKindName(limit->kind), num(attempt),
                                 final ? QStringLiteral("no") : QStringLiteral("yes"), num(r.ms), gateName,
                                 limit->retryAfterMs ? num(*limit->retryAfterMs) : QStringLiteral("-"),
                                 eased.cut ? QStringLiteral("yes") : QStringLiteral("no")));
        QJsonObject ev{{QStringLiteral("kind"), limitKindName(limit->kind)}, {QStringLiteral("message"), limit->message},
                       {QStringLiteral("cut"), eased.cut}, {QStringLiteral("willRetry"), !final}};
        if (!gateName.isEmpty()) ev.insert(QStringLiteral("gate"), gateName);
        if (!limit->provider.isEmpty()) ev.insert(QStringLiteral("provider"), limit->provider);
        if (limit->retryAfterMs) ev.insert(QStringLiteral("retryAfterMs"), double(*limit->retryAfterMs));
        if (!quotaKey.isEmpty()) ev.insert(QStringLiteral("model"), quotaKey);
        emit providerLimit(ev);
        // the same verdict rests the model in model health, so the two memories are set by one decision and cannot
        // disagree; only a firm "no" rests it (a used-up quota, a wait too long to sit through)
        if (m_health && !quotaKey.isEmpty() && (limit->kind == LimitKind::Quota || longAsk))
            m_health->mark(quotaKey, classifyError(e.toProviderError()));
        if (final) { fail(c, e); return; }
        retryAfter(c, retryDelayMs(attempt, limit->retryAfterMs, [this]() { return rnd(); }));
        return;
    }
    // (a 429 never gets here: classifyLimit reads every 429 as a rate limit, a used-up quota or the spend cap)
    WireError e;
    e.kind = r.status == 413 ? WireError::Kind::TooLarge : WireError::Kind::Http;
    e.status = r.status;
    e.message = QStringLiteral("%1 %2").arg(r.status).arg(msg);
    e.body = body;
    e.provider = pd.provider;
    e.providerMessage = pd.raw;
    // a gateway that gave up waiting on the provider (bad gateway / gateway timeout, Cloudflare 520-524) did not say the
    // provider refused: a paid generation may already run and be billed, so it is not re-sent (stricter than the TS)
    const bool gatewayGaveUp = !busy && (r.status == 502 || r.status == 504 || (r.status >= 520 && r.status <= 524));
    const bool unknown = c->paid && gatewayGaveUp;
    const bool retry = !unknown && (busy ? !c->busyRetried : r.status == 429 || r.status >= 500) && attempt < c->rq.retries;
    log::warn(kCat, QStringLiteral("%1 -> %2 attempt=%3 retry=%4 ms=%5%6")
                        .arg(c->dpath, num(r.status), num(attempt), retry ? QStringLiteral("yes") : QStringLiteral("no"), num(r.ms),
                             busy ? QStringLiteral(" busy") : unknown ? QStringLiteral(" outcome-unknown") : QString()));
    if (unknown) {
        e.message += QStringLiteral(". It may still have run; it was not sent again, so nothing is charged twice. Check the provider's "
                                    "activity before trying again.");
        e.outcomeUnknown = true;
    }
    if (!retry) { fail(c, e); return; }
    qint64 delay;
    if (busy) {
        c->busyRetried = true;
        delay = m_options.busyWaitMs + qint64(rnd() * 500);
    } else {
        delay = m_options.backoffBaseMs * (qint64(1) << std::min(attempt, 20)) + qint64(rnd() * double(m_options.backoffJitterMs));
    }
    retryAfter(c, delay);
}

void Wire::onBroken(const std::shared_ptr<Call>& c, const QString& why, bool sentSomething) {
    if (c->timer) { m_clock.cancel(c->timer); c->timer = 0; }
    c->slot.release();
    // a generation call that may have reached the provider may still be running (and be billed) on the far side:
    // sending it again would pay twice. The one-shot transport knows whether a byte left; QNAM does not say.
    const bool mayHaveRun = c->paid && (sentSomething || !c->oneShot);
    // Stop: no retry, no sleep. The transfer is already gone here (the break or the timer came before the queued
    // stop()), so stop() cannot ask it whether it sent: pass what is known, or outcomeUnknown would be lost
    if (c->rq.signal.aborted()) { stop(c, mayHaveRun); return; }
    const int attempt = c->attempt;
    const qint64 ms = m_clock.steadyMs() - c->sendT0;
    const QString message = c->timedOut
        ? QStringLiteral("timed out after %1 s waiting for %2").arg(qint64(std::floor(c->timeoutMs / 1000.0 + 0.5))).arg(c->dpath)
        : why;
    const bool again = attempt < c->rq.retries && !mayHaveRun;
    log::warn(kCat, QStringLiteral("%1 -> %2 attempt=%3 retry=%4 ms=%5")
                        .arg(c->dpath, message, num(attempt), again ? QStringLiteral("yes") : QStringLiteral("no"), num(ms)));
    WireError e;
    e.kind = c->timedOut ? WireError::Kind::Timeout : WireError::Kind::Network;
    e.message = message;
    if (again) {
        retryAfter(c, m_options.backoffBaseMs * (qint64(1) << std::min(attempt, 20)));
        return;
    }
    if (mayHaveRun) {
        e.message += QStringLiteral(". It may still have run; it was not sent again, so nothing is charged twice. Check the provider's "
                                    "activity before trying again.");
        e.outcomeUnknown = true;
    }
    fail(c, e);
}

void Wire::retryAfter(const std::shared_ptr<Call>& c, qint64 ms) {
    ++c->attempt;
    ++m_stats.retries;
    std::weak_ptr<Call> weak = c;
    // the slot is already back in the line; a Stop ends the sleep (stop() cancels this timer)
    c->sleep = m_clock.callAfter(std::max<qint64>(0, ms), this, [this, weak]() {
        auto c = weak.lock();
        if (!c || c->finished) return;
        c->sleep = 0;
        attempt(c);
    });
}

void Wire::stop(const std::shared_ptr<Call>& c, bool mayHaveRun) {
    if (c->finished) return;
    bool inFlightSent = mayHaveRun;
    if (c->transfer) {
        inFlightSent = inFlightSent || c->transfer->sent();
        c->transfer->abort();
        c->transfer->deleteLater();
        c->transfer = nullptr;
    }
    WireError e;
    e.kind = WireError::Kind::Cancelled;
    // TS cancelled(): a generic or abort-worded reason reads "Cancelled"
    const QString reason = c->rq.signal.reason();
    e.message = reason.isEmpty() || reason.contains(QLatin1StringView("abort"), Qt::CaseInsensitive) ? QStringLiteral("Cancelled") : reason;
    // a paid request already on the wire may run anyway: the caller must reconcile, not re-send
    e.outcomeUnknown = c->paid && inFlightSent;
    if (e.outcomeUnknown) e.message += QStringLiteral(" (the request had already been sent; it may still run and be billed)");
    ++m_stats.cancelled;
    fail(c, e);
}

void Wire::fail(const std::shared_ptr<Call>& c, WireError e) {
    if (c->finished) return;
    e.attempts = c->sends;
    if (c->sends == 0) e.sentNothing = true;
    if (e.outcomeUnknown) ++m_stats.unknown;
    finish(c);
    if (!c->failed) return;
    QObject* target = c->hadCtx ? c->ctx.data() : this;
    if (!target) return;
    QMetaObject::invokeMethod(target, [f = std::move(c->failed), e = std::move(e)]() { f(e); }, Qt::QueuedConnection);
}

void Wire::succeed(const std::shared_ptr<Call>& c, Response r) {
    if (c->finished) return;
    finish(c);
    if (!c->done) return;
    QObject* target = c->hadCtx ? c->ctx.data() : this;
    if (!target) return;
    QMetaObject::invokeMethod(target, [f = std::move(c->done), r = std::move(r)]() { f(r); }, Qt::QueuedConnection);
}

void Wire::finish(const std::shared_ptr<Call>& c) {
    c->finished = true;
    if (c->timer) { m_clock.cancel(c->timer); c->timer = 0; }
    if (c->sleep) { m_clock.cancel(c->sleep); c->sleep = 0; }
    if (c->listener) { c->rq.signal.removeListener(c->listener); c->listener = 0; }
    if (c->transfer) { c->transfer->abort(); c->transfer->deleteLater(); c->transfer = nullptr; }
    c->slot.release();
    m_calls.remove(c->id);
}

} // namespace studio::provider
