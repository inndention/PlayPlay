#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define FD_SETSIZE 256          // one select() for every target (Winsock's default is 64)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#include "healthprobe.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <mutex>

namespace ppwatch {

// --- URL --------------------------------------------------------------------------------------------------------------

QString Url::text() const {
    return QStringLiteral("http://%1:%2%3").arg(v6 ? "[" + host + "]" : host).arg(port).arg(QString::fromLatin1(path));
}

bool parseUrl(const QString& s, Url* out, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    QString t = s.trimmed();
    if (t.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) return fail(QStringLiteral("https is not supported (the health endpoints are plain HTTP on loopback): ") + s);
    if (t.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) t = t.mid(7);
    else if (t.contains(QLatin1String("://"))) return fail(QStringLiteral("only http:// URLs: ") + s);
    const int slash = t.indexOf('/');
    const QString hostPort = slash < 0 ? t : t.left(slash);
    const QString path = slash < 0 ? QStringLiteral("/healthz") : t.mid(slash);
    QString host; QString port;
    if (hostPort.startsWith('[')) {                               // [::1]:7799
        const int close = hostPort.indexOf(']');
        if (close < 0) return fail(QStringLiteral("bad IPv6 host: ") + s);
        host = hostPort.mid(1, close - 1);
        if (hostPort.mid(close + 1).startsWith(':')) port = hostPort.mid(close + 2);
    } else {
        const int colon = hostPort.lastIndexOf(':');
        if (colon < 0) return fail(QStringLiteral("no port: ") + s);
        host = hostPort.left(colon);
        port = hostPort.mid(colon + 1);
    }
    bool ok = false;
    const int p = port.toInt(&ok);
    if (!ok || p <= 0 || p > 65535) return fail(QStringLiteral("bad port: ") + s);
    // Loopback only: ppwatch watches, dumps and (opt-in) restarts processes on this machine, identified by who owns the
    // listening port. A remote host can be neither verified nor dumped, and it must never be contacted by accident.
    Url u;
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) host = QStringLiteral("127.0.0.1");
    const QHostAddress a(host);
    if (a.isNull() || !a.isLoopback())
        return fail(QStringLiteral("only loopback hosts (127.0.0.1, localhost, ::1): ppwatch watches processes on this machine; got ") + s);
    u.v6 = a.protocol() == QAbstractSocket::IPv6Protocol;
    u.host = a.toString();
    u.port = quint16(p);
    u.path = path.toLatin1();
    if (u.path.isEmpty() || u.path.contains(' ') || u.path.contains('\r') || u.path.contains('\n')) return fail(QStringLiteral("bad path: ") + s);
    if (out) *out = u;
    if (error) error->clear();
    return true;
}

// --- probe ------------------------------------------------------------------------------------------------------------

const char* ProbeResult::kindName(Kind k) {
    switch (k) {
    case Answered: return "answered";
    case Refused: return "refused";
    case ConnectTimeout: return "connect-timeout";
    case ReplyTimeout: return "reply-timeout";
    case BadReply: return "bad-reply";
    case Error: break;
    }
    return "error";
}

QString ProbeResult::state() const {
    if (kind != Answered) return QString();
    if (status < 200 || status > 299) return QStringLiteral("error");
    const QString s = json.value(QStringLiteral("state")).toString();
    return s.isEmpty() ? QStringLiteral("ok") : s;
}

namespace {

// HTTP/1.x reply -> status, body (Content-Length honoured when present). False until the headers are complete.
bool parseReply(const QByteArray& in, bool closed, ProbeResult* r) {
    const int hdrEnd = in.indexOf("\r\n\r\n");
    if (hdrEnd < 0) return false;
    if (!in.startsWith("HTTP/1.")) { r->kind = ProbeResult::BadReply; r->error = QStringLiteral("not an HTTP reply"); return true; }
    const int sp = in.indexOf(' ');
    r->status = sp > 0 ? in.mid(sp + 1, 3).toInt() : 0;
    qint64 contentLength = -1;
    static const QRegularExpression re(QStringLiteral("\\r\\ncontent-length:\\s*(\\d+)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(in.left(hdrEnd + 2)));
    if (m.hasMatch()) contentLength = m.captured(1).toLongLong();
    QByteArray body = in.mid(hdrEnd + 4);
    if (contentLength >= 0) {
        if (body.size() < contentLength && !closed) return false;   // wait for the rest
        body.truncate(int(std::min<qint64>(contentLength, body.size())));
    } else if (!closed) {
        return false;                                                 // no length: the server closes when done
    }
    if (r->status <= 0) { r->kind = ProbeResult::BadReply; r->error = QStringLiteral("no status"); return true; }
    r->kind = ProbeResult::Answered;
    r->body = body;
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    r->json = doc.isObject() ? doc.object() : QJsonObject();
    return true;
}

#ifdef _WIN32
void ensureWinsock() {
    static std::once_flag once;
    std::call_once(once, []() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
}

struct Conn {
    SOCKET s = INVALID_SOCKET;
    int phase = 0;              // 0 connecting, 1 connected (send/read), 2 done
    QByteArray req, in;
    int sent = 0;
};

constexpr int kMaxReply = 256 * 1024;
#endif

} // namespace

std::vector<ProbeResult> probeAll(const std::vector<Url>& urls, int timeoutMs) {
    std::vector<ProbeResult> out(urls.size());
#ifdef _WIN32
    ensureWinsock();
    QElapsedTimer t; t.start();
    std::vector<Conn> c(urls.size());
    const size_t maxAtOnce = FD_SETSIZE - 8;
    if (urls.size() > maxAtOnce) {                                   // never in practice: split, keep the order
        std::vector<Url> a(urls.begin(), urls.begin() + long(maxAtOnce)), b(urls.begin() + long(maxAtOnce), urls.end());
        std::vector<ProbeResult> ra = probeAll(a, timeoutMs), rb = probeAll(b, timeoutMs);
        ra.insert(ra.end(), rb.begin(), rb.end());
        return ra;
    }
    auto finish = [&](size_t i, ProbeResult::Kind k, const QString& err) {
        out[i].kind = k; out[i].error = err; out[i].ms = t.elapsed(); c[i].phase = 2;
    };
    for (size_t i = 0; i < urls.size(); ++i) {
        const Url& u = urls[i];
        c[i].req = "GET " + u.path + " HTTP/1.1\r\nHost: " + (u.v6 ? "[" + u.host.toLatin1() + "]" : u.host.toLatin1()) + ":" +
                   QByteArray::number(u.port) + "\r\nConnection: close\r\nUser-Agent: PlayPlayWatchdog\r\nAccept: application/json\r\n\r\n";
        sockaddr_storage ss{};
        int len = 0;
        if (u.v6) {
            auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
            a6->sin6_family = AF_INET6; a6->sin6_port = htons(u.port);
            inet_pton(AF_INET6, u.host.toLatin1().constData(), &a6->sin6_addr);
            len = sizeof(sockaddr_in6);
        } else {
            auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
            a4->sin_family = AF_INET; a4->sin_port = htons(u.port);
            inet_pton(AF_INET, u.host.toLatin1().constData(), &a4->sin_addr);
            len = sizeof(sockaddr_in);
        }
        c[i].s = socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (c[i].s == INVALID_SOCKET) { finish(i, ProbeResult::Error, QStringLiteral("socket() failed (%1)").arg(WSAGetLastError())); continue; }
        u_long nb = 1;
        ioctlsocket(c[i].s, FIONBIO, &nb);
        if (connect(c[i].s, reinterpret_cast<sockaddr*>(&ss), len) == 0) { c[i].phase = 1; continue; }
        const int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) continue;
        finish(i, e == WSAECONNREFUSED ? ProbeResult::Refused : ProbeResult::Error, QStringLiteral("connect failed (%1)").arg(e));
    }
    for (;;) {
        const qint64 left = timeoutMs - t.elapsed();
        if (left <= 0) break;
        fd_set rd, wr, ex;
        FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
        int n = 0;
        for (const Conn& k : c) {
            if (k.phase == 0) { FD_SET(k.s, &wr); FD_SET(k.s, &ex); ++n; }
            else if (k.phase == 1) { if (k.sent < k.req.size()) FD_SET(k.s, &wr); FD_SET(k.s, &rd); FD_SET(k.s, &ex); ++n; }
        }
        if (!n) break;
        timeval tv; tv.tv_sec = long(left / 1000); tv.tv_usec = long((left % 1000) * 1000);
        const int sel = select(0, &rd, &wr, &ex, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) continue;                                       // the loop head ends it at the deadline
        for (size_t i = 0; i < c.size(); ++i) {
            Conn& k = c[i];
            if (k.phase == 0) {
                if (FD_ISSET(k.s, &ex)) {                             // a failed non-blocking connect lands here
                    int err = 0, el = sizeof(err);
                    getsockopt(k.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                    finish(i, err == WSAECONNREFUSED ? ProbeResult::Refused : ProbeResult::Error, QStringLiteral("connect failed (%1)").arg(err));
                } else if (FD_ISSET(k.s, &wr)) {
                    k.phase = 1;
                }
                continue;
            }
            if (k.phase != 1) continue;
            if (FD_ISSET(k.s, &wr) && k.sent < k.req.size()) {
                const int w = send(k.s, k.req.constData() + k.sent, k.req.size() - k.sent, 0);
                if (w > 0) k.sent += w;
                else if (WSAGetLastError() != WSAEWOULDBLOCK) { finish(i, ProbeResult::Error, QStringLiteral("send failed (%1)").arg(WSAGetLastError())); continue; }
            }
            if (FD_ISSET(k.s, &rd) || FD_ISSET(k.s, &ex)) {
                char buf[8192];
                bool closed = false;
                for (;;) {
                    const int r = recv(k.s, buf, sizeof(buf), 0);
                    if (r > 0) { k.in.append(buf, r); if (k.in.size() > kMaxReply) break; continue; }
                    if (r == 0) { closed = true; break; }
                    const int e = WSAGetLastError();
                    if (e != WSAEWOULDBLOCK) closed = true;           // reset after a complete reply still counts
                    break;
                }
                if (k.in.size() > kMaxReply) { finish(i, ProbeResult::BadReply, QStringLiteral("reply larger than %1 bytes").arg(kMaxReply)); continue; }
                ProbeResult r;
                if (parseReply(k.in, closed, &r)) { r.ms = t.elapsed(); out[i] = r; k.phase = 2; }
                else if (closed) finish(i, k.in.isEmpty() ? ProbeResult::Error : ProbeResult::BadReply,
                                        k.in.isEmpty() ? QStringLiteral("closed without a reply") : QStringLiteral("incomplete reply"));
            }
        }
    }
    for (size_t i = 0; i < c.size(); ++i) {
        if (c[i].phase == 0) finish(i, ProbeResult::ConnectTimeout, QStringLiteral("no connection within %1 ms").arg(timeoutMs));
        else if (c[i].phase == 1) {
            ProbeResult r;
            if (parseReply(c[i].in, true, &r) && r.kind == ProbeResult::Answered && !c[i].in.isEmpty() && r.status) { r.ms = t.elapsed(); out[i] = r; c[i].phase = 2; }
            else finish(i, ProbeResult::ReplyTimeout, QStringLiteral("connected, no reply within %1 ms").arg(timeoutMs));
        }
        if (c[i].s == INVALID_SOCKET) continue;
        // A probe that timed out is reset, not closed gracefully: a hung server's accept queue (HealthEndpoint listens
        // with a backlog of 8) should not fill with our dead connections, and no TIME_WAIT is left on either side.
        if (out[i].kind == ProbeResult::ReplyTimeout || out[i].kind == ProbeResult::ConnectTimeout) {
            linger lg; lg.l_onoff = 1; lg.l_linger = 0;
            setsockopt(c[i].s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        }
        closesocket(c[i].s);
    }
#else
    for (ProbeResult& r : out) { r.kind = ProbeResult::Error; r.error = QStringLiteral("Windows only"); }
    Q_UNUSED(timeoutMs);
#endif
    return out;
}

ProbeResult probe(const Url& url, int timeoutMs) { return probeAll({url}, timeoutMs).front(); }

// --- tracker ----------------------------------------------------------------------------------------------------------

bool Tracker::isHangState(const QString& s) { return s == QLatin1String("hang") || s == QLatin1String("unresponsive"); }

Decision Tracker::feed(const Observation& ob) {
    Decision d;
    d.from = m_state;
    QString to = m_state;
    switch (ob.reach) {
    case Observation::Answered: m_noAnswer = 0; to = ob.state.isEmpty() ? QStringLiteral("ok") : ob.state; break;
    // One missed reply is noise (a busy machine, a slow scheduler); N in a row with the listener still there is a
    // process that accepts and never answers: the 10-25 picture.
    case Observation::NoAnswer: if (++m_noAnswer >= std::max(1, m_o.unresponsivePolls)) to = QStringLiteral("unresponsive"); break;
    case Observation::NoListener: m_noAnswer = 0; to = QStringLiteral("down"); break;
    case Observation::PidGone: m_noAnswer = 0; to = QStringLiteral("gone"); break;
    }
    // Someone restarted the process behind the port: whatever happens next is a new episode.
    if (m_inEpisode && ob.pid && m_episodePid && ob.pid != m_episodePid) m_inEpisode = false;
    if (!isHangState(to)) {
        m_inEpisode = false;
    } else if (!m_inEpisode) {
        m_inEpisode = true; m_restartDone = false;
        m_episodeStart = ob.tMs; m_episodePid = ob.pid;
        d.incident = true;
    } else if (!m_episodePid && ob.pid) {
        m_episodePid = ob.pid;
    }
    if (m_inEpisode) {
        d.episodeMs = ob.tMs - m_episodeStart;
        if (m_o.supervisorRole && m_o.restartAfterMs > 0 && !m_restartDone && d.episodeMs >= m_o.restartAfterMs) {
            d.restart = true;
            m_restartDone = true;
        }
    }
    d.to = to;
    d.changed = to != m_state;
    m_state = to;
    return d;
}

} // namespace ppwatch
