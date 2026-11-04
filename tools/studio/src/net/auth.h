#pragma once
// Token auth for the Studio's REST and WS ports (ST-04, docs/plans/starmedia-studio.md §4.1). The service binds
// loopback only (StudioService::checkOptions), but loopback is not trust: any local process, and any web page through
// the browser, can reach 127.0.0.1. So every route but /health and the home page needs the bearer token in
// <data>/studio-token:
//   - generated on first start: 32 random bytes, base64url (43 chars), the same shape as StarAgent's agent-token;
//   - created with an owner-only DACL (the current user and SYSTEM, inheritance removed: what
//     `icacls <f> /inheritance:r /grant:r <user>:F` leaves on D:\StarMedia\mesh\token), never readable for a moment
//     by anyone else (the DACL is set at creation, then the file is renamed into place);
//   - kept, never regenerated, while it is valid; a malformed file refuses start-up (fail closed) instead of being
//     replaced behind the operator's back.
// Clients send "Authorization: Bearer <token>" or "X-Studio-Token: <token>". The WS handshake may also use ?token=,
// because a browser's WebSocket cannot set headers. Tokens are compared in constant time over their SHA-256, are
// registered with the log scrubber, and are never logged or returned; only the hash stays in memory.
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>

class QUrlQuery;

namespace studio::net {

class Headers;

class Auth {
public:
    static QString fileName() { return QStringLiteral("studio-token"); }

    /// Reads <dataDir>/studio-token, creating it when missing. False with `error` (and no token) when the file is
    /// malformed or cannot be created. The owner-only DACL is re-applied to an existing file (best effort, logged).
    bool load(const QString& dataDir, QString* error = nullptr);
    /// An in-memory token (tests, embedding); no file.
    void setToken(const QString& token);
    /// Replaces the token: a new one is written (owner-only, atomic rename over the old file) and takes effect at once.
    /// Only after load(). Clients holding the old token get 401 from then on.
    bool rotate(QString* error = nullptr);
    bool hasToken() const { return !m_hash.isEmpty(); }
    QString tokenPath() const { return m_path; }

    /// Constant-time comparison of a presented token with ours. False when either is empty.
    bool matches(QByteArrayView presented) const;
    /// The token a request presents: Authorization: Bearer, else X-Studio-Token, else (only when `query` is given: the
    /// WS handshake) ?token=.
    static QByteArray presented(const Headers& headers, const QUrlQuery* query = nullptr);
    bool check(const Headers& headers, const QUrlQuery* query = nullptr) const { return matches(presented(headers, query)); }

    static QString generate();
    /// 24..512 printable ASCII characters without spaces (a pasted mesh token or a generated one).
    static bool validToken(QStringView t);
    /// For clients (CLI, tests, --print-token): the token of a token file (BOM and whitespace trimmed), without creating
    /// it. Empty with `error` when missing or malformed.
    static QString readTokenFile(const QString& path, QString* error = nullptr);

private:
    QString m_path;
    QByteArray m_hash;                     // SHA-256 of the token
};

/// Writes `data` to `path` readable only by the current user and SYSTEM. The DACL is set when the temp file is created,
/// then the file is renamed into place: no moment where another account can read it. Without `replace` an existing
/// file is kept and false is returned (`*existed` = true).
bool writePrivateFile(const QString& path, const QByteArray& data, bool replace, QString* error = nullptr, bool* existed = nullptr);
/// Re-applies the owner-only DACL to an existing file.
bool restrictToOwner(const QString& path, QString* error = nullptr);
/// Diagnostics and tests: the SIDs a file's DACL allows, and whether the DACL is protected from inheritance.
QStringList aclSids(const QString& path, bool* isProtected = nullptr, QString* error = nullptr);
/// "S-1-5-21-..." of the account this process runs as (empty off Windows).
QString currentUserSid();

} // namespace studio::net
