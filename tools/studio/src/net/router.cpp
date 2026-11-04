#include "net/router.h"

#include <QDateTime>
#include <QHostAddress>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

#include "core/log.h"
#include "net/auth.h"

namespace studio::net {

namespace {

// A path component made of dots and spaces only ("." / ".." / "..." / ".. "): Windows trims trailing dots and spaces,
// so any of them can act as "." or ".." once a handler joins it into a file path.
bool dotComponent(QStringView c) {
    return !c.isEmpty() && std::all_of(c.begin(), c.end(), [](QChar ch) { return ch == QLatin1Char('.') || ch == QLatin1Char(' '); });
}

// Percent-decoded path segments; false for a dot segment, NUL or a malformed escape. A decoded segment may hold '/'
// (from %2F, e.g. a model id), but no dot component between its '/' or '\' either: "..%2F..%2Fsecret" is one segment
// whose value would climb out of a directory once a handler or a {*tail} join uses it. The trailing empty segment of
// "/a/b/" is dropped; "/" has no segments.
bool decodeSegments(const QByteArray& rawPath, QStringList* out) {
    if (!rawPath.startsWith('/')) return false;
    QList<QByteArray> parts = rawPath.mid(1).split('/');
    if (!parts.isEmpty() && parts.last().isEmpty()) parts.removeLast();
    for (const QByteArray& p : parts) {
        auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        for (qsizetype i = 0; i < p.size(); ++i)
            if (p[i] == '%' && (i + 2 >= p.size() || !hex(p[i + 1]) || !hex(p[i + 2]))) return false;
        const QString s = QUrl::fromPercentEncoding(p);
        if (s.contains(QChar(0))) return false;
        qsizetype start = 0;
        for (qsizetype i = 0; i <= s.size(); ++i) {
            if (i < s.size() && s[i] != QLatin1Char('/') && s[i] != QLatin1Char('\\')) continue;
            if (dotComponent(QStringView(s).sliced(start, i - start))) return false;
            start = i + 1;
        }
        out->append(s);
    }
    return true;
}

// Unauthorized attempts are logged at most once per peer address per minute (a misconfigured client polls).
void logUnauthorized(const Request& rq) {
    static QHash<QString, qint64> last;
    const QString peer = rq.peer.toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last.value(peer, 0) < 60000) return;
    if (last.size() > 1000) last.clear();
    last.insert(peer, now);
    log::warn(u"auth", QStringLiteral("%1 %2 from %3: missing or invalid token").arg(QString::fromLatin1(rq.method.left(16)), rq.path.left(200), peer));
}

} // namespace

bool Router::parsePattern(const QString& pattern, QList<Seg>* out) {
    if (!pattern.startsWith(QLatin1Char('/'))) return false;
    static const QRegularExpression name(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    QStringList parts = pattern.mid(1).split(QLatin1Char('/'));
    if (!parts.isEmpty() && parts.last().isEmpty()) parts.removeLast();
    QStringList names;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString& p = parts[i];
        if (p.isEmpty()) return false;
        if (p.startsWith(QLatin1Char('{'))) {
            if (!p.endsWith(QLatin1Char('}'))) return false;
            QString n = p.mid(1, p.size() - 2);
            const bool tail = n.startsWith(QLatin1Char('*'));
            if (tail) {
                if (i != parts.size() - 1) return false;     // {*tail} only last
                n = n.mid(1);
            }
            if (!name.match(n).hasMatch() || names.contains(n)) return false;
            names << n;
            out->append({tail ? Seg::Tail : Seg::Param, n});
        } else {
            if (p.contains(QLatin1Char('{')) || p.contains(QLatin1Char('}')) || p == QLatin1String(".") || p == QLatin1String("..")) return false;
            out->append({Seg::Literal, p});
        }
    }
    return true;
}

bool Router::validPattern(const QString& pattern) {
    QList<Seg> s;
    return parsePattern(pattern, &s);
}

bool Router::add(const QByteArray& method, const QString& pattern, SyncHandler h, RouteOptions o) {
    if (!h) return false;
    return addAsync(method, pattern, [h = std::move(h)](const Request& rq, Responder res) { res.send(h(rq)); }, o);
}

bool Router::addAsync(const QByteArray& method, const QString& pattern, Handler h, RouteOptions o) {
    Route r;
    r.method = method.toUpper();
    if (r.method.isEmpty() || !h || !parsePattern(pattern, &r.segs)) return false;
    for (const Route& e : m_routes)
        if (e.method == r.method && e.pattern == pattern) return false;
    r.pattern = pattern;
    r.handler = std::move(h);
    r.opt = o;
    m_routes.append(std::move(r));
    return true;
}

bool Router::remove(const QByteArray& method, const QString& pattern) {
    const QByteArray m = method.toUpper();
    return m_routes.removeIf([&](const Route& r) { return r.method == m && r.pattern == pattern; }) > 0;
}

bool Router::matches(const Route& r, const QStringList& segs, QHash<QString, QString>* params) {
    QHash<QString, QString> got;
    qsizetype i = 0;
    for (const Seg& s : r.segs) {
        if (s.kind == Seg::Tail) {
            got.insert(s.text, segs.mid(i).join(QLatin1Char('/')));
            i = segs.size();
            break;
        }
        if (i >= segs.size()) return false;
        if (s.kind == Seg::Literal) {
            if (segs[i] != s.text) return false;
        } else {
            if (segs[i].isEmpty()) return false;   // "//" gives an empty segment: never a parameter value
            got.insert(s.text, segs[i]);
        }
        ++i;
    }
    if (i != segs.size()) return false;
    if (params) *params = std::move(got);
    return true;
}

int Router::compareSpecificity(const Route& a, const Route& b) {
    // Segment by segment from the left: literal > {param} > the pattern's end > {*tail}. "The pattern ends here" ranks
    // above a tail, so for /files the exact route /files beats /files/{*p} (whose tail would match nothing), and / is
    // never shadowed by a catch-all /{*path}. When one pattern ends and the other goes on with a literal or a {param},
    // no path matches both, so that order is never used.
    auto rank = [](const Route& r, qsizetype i) {
        if (i >= r.segs.size()) return 1;
        return r.segs[i].kind == Seg::Literal ? 3 : r.segs[i].kind == Seg::Param ? 2 : 0;
    };
    const qsizetype n = qMax(a.segs.size(), b.segs.size());
    for (qsizetype i = 0; i < n; ++i) {
        const int ka = rank(a, i), kb = rank(b, i);
        if (ka != kb) return ka - kb;
        if (ka == 0 || ka == 1) break;     // both end here, or both take the rest as a tail: equally specific
    }
    return 0;                              // a tie: the route added first wins
}

Router::Match Router::match(const QByteArray& method, const QByteArray& rawPath) const {
    Match m;
    QStringList segs;
    if (!decodeSegments(rawPath, &segs)) {
        m.status = 400;
        return m;
    }
    // The most specific route of `meth` matching the path; every matching route's method goes into `allowed`.
    auto pick = [&](const QByteArray& meth, QHash<QString, QString>* params) {
        int best = -1;
        for (int i = 0; i < m_routes.size(); ++i) {
            const Route& r = m_routes[i];
            QHash<QString, QString> p;
            if (!matches(r, segs, &p)) continue;
            const QString name = QString::fromLatin1(r.method);
            if (!m.allowed.contains(name)) m.allowed << name;
            if (r.method != meth) continue;
            if (best < 0 || compareSpecificity(r, m_routes[best]) > 0) {
                best = i;
                *params = std::move(p);
            }
        }
        return best;
    };
    QHash<QString, QString> params;
    int best = pick(method, &params);
    if (best < 0 && method == "HEAD") best = pick("GET", &params);   // GET routes answer HEAD (the body is dropped)
    if (best >= 0) {
        m.route = best;
        m.params = std::move(params);
        return m;
    }
    m.status = m.allowed.isEmpty() ? 404 : 405;
    return m;
}

Admission Router::admit(Request& rq) const {
    Admission a;
    Match m = match(rq.method, rq.rawPath);
    if (m.status == 0) {
        const Route& r = m_routes[m.route];
        rq.params = std::move(m.params);
        rq.authenticated = m_auth && m_auth->check(rq.headers);
        if (r.opt.access == Access::Token && !rq.authenticated) {
            logUnauthorized(rq);
            Response e = Response::error(401, QStringLiteral("missing or invalid token (send Authorization: Bearer <studio-token>)"));
            e.headers.set("WWW-Authenticate", "Bearer realm=\"PlayPlayStudio\"");
            a.refuse = std::move(e);
            return a;
        }
        a.handler = r.handler;
        // A public route is reachable without the token: unless it says otherwise, it reads at most kPublicMaxBody, so
        // anonymous clients cannot make every connection buffer the server's default body limit.
        a.maxBodyBytes = r.opt.maxBodyBytes >= 0 ? r.opt.maxBodyBytes : r.opt.access == Access::Public ? kPublicMaxBody : -1;
        a.spool = r.opt.spool;
        a.hashBody = r.opt.hashBody;
        a.timeoutMs = r.opt.timeoutMs;
        return a;
    }
    if (m.status == 400) {
        a.refuse = Response::error(400, QStringLiteral("bad path"));
        return a;
    }
    if (m.status == 405) {
        QStringList allow = m.allowed;
        if (allow.contains(QStringLiteral("GET")) && !allow.contains(QStringLiteral("HEAD"))) allow.insert(allow.indexOf(QStringLiteral("GET")) + 1, QStringLiteral("HEAD"));
        if (!allow.contains(QStringLiteral("OPTIONS"))) allow << QStringLiteral("OPTIONS");
        Response e = rq.method == "OPTIONS" ? Response::empty(204) : Response::error(405, QStringLiteral("method not allowed"));
        e.headers.set("Allow", allow.join(QStringLiteral(", ")).toLatin1());
        a.refuse = std::move(e);
        return a;
    }
    a.refuse = Response::error(404, QStringLiteral("not found"));
    return a;
}

} // namespace studio::net
