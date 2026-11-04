// core/http_ctx.h - the per-request objects of the in-house HTTP/1.1 server: headers, query, request, streaming body
// reader, streaming response and the handler context. FROZEN (ANFS-F0); implemented by ANFS-F4a (core/http_ctx.cpp).
//
// Handlers are straight-line blocking code on a worker thread (anfs-cpp.md 2.4). Node-compat rules that live here:
// - Errors are {"error":"<text>"} (Ctx::error); HttpError thrown from a handler becomes exactly that body.
// - JSON replies are JSON.stringify bytes with Content-Type "application/json; charset=utf-8"; no ETag, no
//   X-Powered-By (deviation C8).
// - Reason phrases are Node's http.STATUS_CODES.
// - HEAD: handlers run as for GET; the response writes headers (with the GET Content-Length) and no body.
#pragma once

#include "core/config.h"
#include "core/json.h"
#include "core/net.h"
#include "core/stream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anfs::http {

struct Route;      // core/http_router.h
class Server;      // core/http_server.h

// Thrown by handlers and helpers; the server answers status with {"error":text} unless the headers were already
// sent (then the connection is aborted).
class HttpError : public std::runtime_error {
public:
    HttpError(int status, const std::string& text) : std::runtime_error(text), status(status) {}
    int status = 500;
};

const char* reasonPhrase(int status);             // Node http.STATUS_CODES ("unknown" for others, as Node writes)

// decodeURIComponent-like: %XX as UTF-8 bytes; invalid escapes stay literal; plusAsSpace for query strings.
std::string urlDecode(std::string_view s, bool plusAsSpace);
std::string encodeURIComponent(std::string_view s);   // JS encodeURIComponent (UTF-8, unreserved A-Z a-z 0-9 -_.!~*'())

// Ordered header list: names compared case-insensitively, duplicates kept in arrival order, names stored as given.
class Headers {
public:
    using Field = std::pair<std::string, std::string>;

    const std::string* find(std::string_view name) const;          // first value
    std::string get(std::string_view name, std::string_view def = {}) const;
    std::string joined(std::string_view name) const;               // all values joined with ", " (Node req.headers)
    std::vector<std::string> getAll(std::string_view name) const;
    bool has(std::string_view name) const { return find(name) != nullptr; }
    void add(std::string name, std::string value);
    void set(std::string name, std::string value);                 // replaces all, keeps the first one's position
    bool remove(std::string_view name);                            // removes all of that name
    void clear() { f_.clear(); }
    size_t size() const { return f_.size(); }
    bool empty() const { return f_.empty(); }
    const std::vector<Field>& fields() const { return f_; }

private:
    std::vector<Field> f_;
};

// Query string (Express req.query for flat keys): keys are case-sensitive, the first occurrence wins for get(),
// '+' is a space, %XX decodes as UTF-8.
class Query {
public:
    Query() = default;
    explicit Query(std::string_view rawQuery);
    std::optional<std::string> get(std::string_view key) const;
    std::string value(std::string_view key, std::string_view def = {}) const;
    bool has(std::string_view key) const { return get(key).has_value(); }
    std::vector<std::string> all(std::string_view key) const;
    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
    // URLSearchParams#toString form (spaces as '+'), used when PeerClient replaces auth= or Proxy drops agentId.
    static std::string build(const std::vector<std::pair<std::string, std::string>>& items);

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct Request {
    std::string method;                           // upper-case
    std::string target;                           // request-target as received: "/api/browse?path=C%3A%5C"
    std::string path;                             // target without the query, NOT percent-decoded (Express req.path)
    std::string rawQuery;                         // after '?', without it
    int httpMinor = 1;                            // HTTP/1.x
    Headers headers;
    Query query;

    std::string peerIp;                           // socket address (IPv4-mapped normalised); PROXY v1 client on the
    int peerPort = 0;                             // TLS front's internal listener
    std::string localIp;
    int localPort = 0;
    std::string clientIp;                         // trusted-proxy aware (ServerOptions::resolveClient)
    net::NetClass clientClass = net::NetClass::Wan;
    bool secure = false;                          // arrived through the TLS front

    int64_t contentLength = -1;                   // -1 when absent or chunked
    bool chunked = false;
    bool expectContinue = false;

    std::string header(std::string_view name, std::string_view def = {}) const { return headers.get(name, def); }
};

namespace detail {
// The byte transport under one connection: the server's socket connection, or a test double. recv() serves bytes
// already buffered by the header parser first; unread() pushes bytes back (pipelined requests after a chunked body).
class Wire {
public:
    virtual ~Wire() = default;
    virtual size_t recv(char* buf, size_t n) = 0;                  // 0 = closed; throws IoError on timeout/reset
    virtual void unread(const char* p, size_t n) = 0;
    virtual void send(const char* p, size_t n) = 0;                // throws IoError{peerGone} when the client left
    // Zero-copy file body (TransmitFile in 1 GiB slices). Returns false when unsupported; the caller then copies.
    virtual bool sendFile(void* fileHandle, int64_t offset, int64_t count) = 0;
    virtual void abort() = 0;                                      // hard close (RST)
};
} // namespace detail

// The request body as a stream. Decodes Content-Length and chunked framing (trailers kept), sends
// "HTTP/1.1 100 Continue" before the first read when the client asked for it (Node default), and never reads past
// the end of this request's body.
class BodyReader final : public Source {
public:
    BodyReader(detail::Wire& wire, const Request& req);
    ~BodyReader() override;
    BodyReader(const BodyReader&) = delete;
    BodyReader& operator=(const BodyReader&) = delete;

    size_t read(char* buf, size_t n) override;    // 0 at the end of the body; throws IoError
    int64_t length() const override;              // Content-Length, -1 when chunked or unknown
    bool hasBody() const;                         // Content-Length > 0 or chunked
    bool finished() const;
    int64_t consumed() const;
    const Headers& trailers() const;

    // Buffers the whole body; throws HttpError(413, "request entity too large") past limit.
    std::string readAll(int64_t limit);
    // Streams into out; throws HttpError(413, ...) past limit (limit < 0 = none). Returns bytes copied.
    int64_t copyTo(Sink& out, int64_t limit = -1, const std::function<void(int64_t)>& progress = {});
    // Discards the rest of the body; returns the bytes discarded, or -1 when more than cap remained (the
    // connection must then close instead of being kept alive).
    int64_t drain(int64_t cap);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The response: ordered headers, one-shot send() or streaming begin()/write()/end(). Content-Length when known,
// chunked framing when begin() gets -1 (HTTP/1.1; HTTP/1.0 then closes the connection). A Date header and
// "Connection: keep-alive|close" are added like Node.
class Response {
public:
    Response(detail::Wire& wire, const Request& req);
    ~Response();
    Response(const Response&) = delete;
    Response& operator=(const Response&) = delete;

    Headers& headers();
    const Headers& headers() const;
    void setHeader(std::string name, std::string value) { headers().set(std::move(name), std::move(value)); }
    int status() const;
    bool headersSent() const;
    bool finished() const;
    bool aborted() const;
    bool isHead() const;
    void setKeepAlive(bool on);
    bool keepAlive() const;

    void send(int status, std::string_view contentType, std::string_view body);
    void sendJson(int status, const JVal& v);
    void sendText(int status, std::string_view text, std::string_view contentType = "text/plain; charset=utf-8");
    void sendEmpty(int status);                   // 204, 304, 416 ... with Content-Length: 0 where Node sends it
    void redirect(std::string_view location, int status = 302);

    void begin(int status, int64_t contentLength); // -1 = chunked
    void write(const char* p, size_t n);
    void write(std::string_view s) { write(s.data(), s.size()); }
    // Writes count bytes of an open file after begin() (zero-copy when the wire supports it, else a 64 KB copy).
    void writeFile(void* fileHandle, int64_t offset, int64_t count);
    void end();                                   // terminating chunk for chunked bodies
    void abort();                                 // RST; used when an error happens after the headers went out
    Sink& sink();                                 // write() as a Sink, for copyStream / ZipWriter / json::Writer
    int64_t bodyBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Everything a handler sees.
class Ctx {
public:
    Ctx(Request& req, Response& res, BodyReader& body, std::shared_ptr<const Config> config, Server* server = nullptr)
        : req(req), res(res), body(body), config(std::move(config)), server(server) {}
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;

    Request& req;
    Response& res;
    BodyReader& body;
    std::shared_ptr<const Config> config;         // snapshot taken when the request started
    Server* server = nullptr;
    const Route* route = nullptr;                 // set by the router before middleware runs (may be null)
    std::vector<std::pair<std::string, std::string>> params;       // {name} / {*rest} values, decoded
    JVal items = JVal::object();                  // per-request scratch for middleware (never secrets)

    const std::string& method() const { return req.method; }
    const std::string& path() const { return req.path; }
    std::string param(std::string_view name) const;               // "" when absent; name case-insensitive
    std::optional<std::string> query(std::string_view name) const { return req.query.get(name); }
    std::string queryOr(std::string_view name, std::string_view def = {}) const { return req.query.value(name, def); }
    std::string header(std::string_view name) const { return req.headers.get(name); }
    bool isWan() const { return req.clientClass == net::NetClass::Wan; }

    void json(int status, const JVal& v) { res.sendJson(status, v); }
    void error(int status, std::string_view text);                // exactly {"error":"<text>"}

    // Express json() semantics (mod-fscore Api.Body): an empty body or a non-JSON Content-Type gives an empty
    // object; invalid JSON throws HttpError(400, "Invalid JSON body."); past limit HttpError(413, ...).
    JVal readJson(int64_t limit = 1 << 20);
};

} // namespace anfs::http
