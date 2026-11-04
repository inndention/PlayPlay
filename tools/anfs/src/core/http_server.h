// core/http_server.h - in-house HTTP/1.1 server on Winsock2: blocking sockets, one worker thread per connection
// from an elastic pool, and SendFile with Range/ETag. FROZEN (ANFS-F0); implemented by ANFS-F4a (server core) and
// ANFS-F4b (sendFile, ranges) in core/http_server.cpp.
//
// Spec (anfs-cpp.md 2.4):
// - One accept thread per listener; connections go to a pool of at most maxThreads workers (256 KB reserved stack,
//   idle workers expire after 30 s). Above the cap: 503 {"error":"Server busy."} and close.
// - Inactivity timeouts via SO_RCVTIMEO/SO_SNDTIMEO: header read 30 s, keep-alive idle 5 s, body read/write 120 s.
//   No total request timeout (multi-hour uploads). TCP_NODELAY on.
// - Header block max 64 KB, else 431. int64 Content-Length. Chunked request bodies with trailers. Expect:
//   100-continue answered on the handler's first body read. HTTP/1.0 (close by default) and pipelining work.
// - After a handler, an unread body of at most 1 MB is drained for keep-alive; a larger one closes the connection.
// - An exception after the headers were sent aborts the connection (RST); IoError{peerGone} is never logged.
// - stop(): new requests get 503 while in-flight ones finish (drainOnStopMs), then the sockets close. Parked long
//   polls are released by their modules' stop() first (ModeManager stops modules before the server).
#pragma once

#include "core/config.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/log.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace anfs::http {

struct ServerOptions {
    int maxThreads = 512;
    size_t threadStackBytes = 256 * 1024;
    int idleThreadExpiryMs = 30000;
    int headerTimeoutMs = 30000;
    int keepAliveIdleMs = 5000;                   // Node keepAliveTimeout
    int bodyTimeoutMs = 120000;
    int writeTimeoutMs = 120000;
    size_t maxHeaderBytes = 64 * 1024;
    int64_t keepAliveDrainBytes = 1 << 20;
    int drainOnStopMs = 3000;
    bool tcpNoDelay = true;
    // Fills Request::clientIp / clientClass from the socket peer and headers. The runtime wires net::realClient +
    // net::classify with the config's trusted proxies. Default: clientIp = peerIp, clientClass = Wan (fail closed).
    std::function<void(Request&, const Config&)> resolveClient;
};

class BindError : public std::runtime_error {
public:
    BindError(const std::string& what, int wsaError) : std::runtime_error(what), wsaError(wsaError) {}
    int wsaError = 0;                             // 10048 = address in use, 10013 = access denied
};

class Server {
public:
    // config: called once per request (snapshot handed to Ctx). log may be null.
    Server(Router& router, std::function<std::shared_ptr<const Config>()> config, Log* log = nullptr,
           ServerOptions options = {});
    ~Server();                                    // stop()
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds 0.0.0.0:port when bindAll, else bindAddress:port, and starts accepting. port 0 = ephemeral (tests).
    // Throws BindError. No fallback to another port: the caller (ModeManager, peer-legacy port+1 retry) decides.
    void start(int port, bool bindAll, const std::string& bindAddress = "127.0.0.1");
    // An extra loopback-only listener whose connections begin with a PROXY protocol v1 line
    // ("PROXY TCP4 <client> <server> <cport> <sport>\r\n"): the TLS front (w3-tls) pipes plaintext here, and the
    // request gets peerIp = <client>, secure = true. Lines from a non-127.0.0.1 socket are refused. Returns the port.
    int startProxyListener(int port = 0);
    void stop();

    bool running() const;
    int port() const;                             // the main listener's bound port
    std::vector<std::string> listening() const;   // "0.0.0.0:18080", "127.0.0.1:53211 (proxy)"
    int inFlight() const;
    int threads() const;
    int64_t requests() const;
    int64_t rejectedBusy() const;
    const ServerOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// ---- SendFile (ANFS-F4b): Express `send` behaviour, matched to Node exactly (anfs-cpp.md 1.6, 2.4, 3.9) ----

struct SendFileOptions {
    std::string contentType;                      // default mime::contentTypeForFile(path)
    std::string downloadName;                     // non-empty: Content-Disposition via contentDisposition()
    std::string dispositionType = "attachment";
    std::string cacheControl = "public, max-age=0";
    bool lastModified = true;
    bool etag = true;
};

// Weak ETag W/"<size hex>-<mtime ms hex>", Last-Modified, Accept-Ranges: bytes. A single bytes=a-b / a- / -n range
// gives 206 + Content-Range; a multi-range gives 200 with the full body; an unsatisfiable range gives 416 with
// Content-Range: bytes */<len>. If-None-Match / If-Modified-Since give 304; If-Range that does not match the ETag
// (or Last-Modified date) ignores the Range. Body via Response::writeFile (TransmitFile, 1 GiB slices).
// Returns false and sends nothing when the path is missing or a directory (callers write their own 404 text).
bool sendFile(Ctx& ctx, const std::string& path, const SendFileOptions& options = {});

enum class RangeResult { Full, Partial, Unsatisfiable };
// Parses one Range header against a length. Multi-range and malformed headers give Full (Express ignores them);
// start/end are inclusive. ANFS-F4b, checked against range-parser 1.2.1 (combine: true) as send uses it: a header
// not starting with / *bytes=/ is Full; each part that is invalid or past the end is dropped, and with none left the
// result is Unsatisfiable, so "bytes=5-1" gives 416 in Node (not Full); parts that overlap or touch are merged first,
// so "bytes=0-4,5-9" is Partial 0-9; two or more separate ranges give Full.
RangeResult parseRange(std::string_view header, int64_t length, int64_t& start, int64_t& end);

std::string weakEtag(int64_t size, int64_t mtimeMs);
std::string httpDate(int64_t epochMs);            // IMF-fixdate: "Wed, 14 Oct 2026 13:17:58 GMT"
int64_t parseHttpDate(std::string_view s);        // -1 when invalid
// The `content-disposition` npm module rule: filename="<ISO-8859-1 fallback>", plus filename*=UTF-8''<encoded>
// only when the name is not printable ISO-8859-1 or contains %XX. [UNVERIFIED against a running Node]
std::string contentDisposition(std::string_view filename, std::string_view type = "attachment");

} // namespace anfs::http
