#pragma once
// Request router for HttpServer (ST-04). Routes are `METHOD /literal/{param}/{*tail}` patterns:
//   - a literal segment matches itself exactly (case-sensitive), {name} one percent-decoded segment, {*name} the rest
//     of the path (zero or more segments, joined by '/'); the most specific match wins (literal > {param} > the end of
//     the pattern > {*tail}, segment by segment from the left), so /api/v1/jobs/active beats /api/v1/jobs/{id}, and
//     /files beats /files/{*p} for the path /files (a tail route gets an empty tail only when no exact route exists);
//   - GET routes answer HEAD too; a path that matches only other methods gets 405 with Allow; OPTIONS answers 204 with
//     Allow (no CORS headers: other origins stay unable to read answers or send the token header);
//   - dot segments ("." / ".." / "..." and other dots-and-spaces names, also percent-encoded and also between the '/'
//     or '\' a %2F / %5C put inside one segment) and NUL bytes are refused (400) before any handler sees them; a
//     trailing slash is ignored. Handlers still validate ids they use as file names (ids::isSafe).
// Every route needs the token (Access::Token) unless it says Access::Public. The check runs in admit(), on the request
// head, before the server reads any body byte. Without an Auth every Token route answers 401 (fail closed).
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include "net/httpserver.h"

namespace studio::net {

class Auth;

enum class Access { Token, Public };

/// Body limit of a public route that sets none (it answers 413 above this; token routes get the server default).
constexpr qint64 kPublicMaxBody = 64 * 1024;

struct RouteOptions {
    Access access = Access::Token;
    qint64 maxBodyBytes = -1;              // -1: ServerOptions::maxBodyBytes (kPublicMaxBody for a public route)
    bool spool = false;                    // large bodies to a temp file (Request::bodyFile)
    bool hashBody = false;                 // Request::bodySha256 while the body arrives
    int timeoutMs = -1;                    // handler deadline; -1: the server default, 0: none
};

using SyncHandler = std::function<Response(const Request&)>;

class Router {
public:
    void setAuth(const Auth* auth) { m_auth = auth; }

    /// A handler that answers before it returns. False for a malformed pattern or a duplicate (method, pattern).
    bool add(const QByteArray& method, const QString& pattern, SyncHandler h, RouteOptions o = {});
    /// A handler that answers through its Responder, now or later, from any thread.
    bool addAsync(const QByteArray& method, const QString& pattern, Handler h, RouteOptions o = {});
    bool remove(const QByteArray& method, const QString& pattern);
    int size() const { return int(m_routes.size()); }

    /// HttpServer's Admit: matches the route, checks the token and returns the handler and body limits, or the answer
    /// to refuse with (400, 401, 404, 405, 204 for OPTIONS).
    Admission admit(Request& rq) const;
    /// For admit() and tests: the route a method and raw path select. status 0 = found (route >= 0, params filled),
    /// else 400 / 404 / 405 (allowed = the methods the path has).
    struct Match {
        int route = -1;
        int status = 0;
        QHash<QString, QString> params;
        QStringList allowed;
    };
    Match match(const QByteArray& method, const QByteArray& rawPath) const;
    /// "/api/v1/projects/{id}" -> true; refuses empty, relative, unbalanced braces, bad names, a non-final {*tail}.
    static bool validPattern(const QString& pattern);

private:
    struct Seg {
        enum Kind { Literal, Param, Tail } kind;
        QString text;                      // literal text or the parameter name
    };
    struct Route {
        QByteArray method;
        QString pattern;
        QList<Seg> segs;
        Handler handler;
        RouteOptions opt;
    };
    static bool parsePattern(const QString& pattern, QList<Seg>* out);
    static bool matches(const Route& r, const QStringList& segs, QHash<QString, QString>* params);
    static int compareSpecificity(const Route& a, const Route& b);   // > 0: a is more specific

    QList<Route> m_routes;
    const Auth* m_auth = nullptr;
};

} // namespace studio::net
