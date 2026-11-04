// core/http_client.h - outbound HTTP on WinHTTP (an OS component). FROZEN (ANFS-F0); implemented by ANFS-F5
// (core/http_client.cpp).
//
// - One WinHTTP session per HttpClient with WINHTTP_ACCESS_TYPE_NO_PROXY; keep-alive connections are reused (the
//   peer's 5 s register no longer opens a TCP connection each time).
// - Redirects are never followed. TLS certificate errors are ignored (Node rejectUnauthorized:false).
// - Timeouts are inactivity timeouts: resolve 10 s, connect 10 s, send/receive = idleMs (default 30 s); a transfer
//   may last hours as long as bytes keep moving.
// - Request bodies stream from a Source: with a known length (Content-Length) or chunked framing written by the
//   client itself (bodyLength -1). Response bodies stream through ClientResponse::read().
// - Headers go out in the given order; Host, Content-Length and Transfer-Encoding are computed. Response header names
//   are lower-cased (Node res.headers).
// - cancel() from another thread closes the request handle: the blocked call throws ClientError{cancelled}.
#pragma once

#include "core/http_ctx.h"
#include "core/stream.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace anfs::http {

class ClientError : public IoError {
public:
    ClientError(const std::string& what, uint32_t winError, bool timeout = false, bool cancelled = false)
        : IoError(what, false, timeout), winError(winError), cancelled(cancelled) {}
    uint32_t winError = 0;
    bool cancelled = false;
};

// Shared cancel flag: cancel() aborts every request currently using the token.
class CancelToken {
public:
    CancelToken();
    ~CancelToken();
    void cancel();
    bool cancelled() const;
    struct State;
    std::shared_ptr<State> state() const { return s_; }

private:
    std::shared_ptr<State> s_;
};

struct ClientRequest {
    std::string method = "GET";
    std::string url;                              // http(s)://host[:port]/path?query
    Headers headers;
    Source* body = nullptr;                       // not owned
    int64_t bodyLength = -1;                      // with a body: -1 = chunked framing
    int idleMs = 30000;
    int connectMs = 10000;
    int resolveMs = 10000;
    CancelToken* cancel = nullptr;
};

class ClientResponse final : public Source {
public:
    ~ClientResponse() override;
    ClientResponse(const ClientResponse&) = delete;
    ClientResponse& operator=(const ClientResponse&) = delete;

    int status() const;
    const Headers& headers() const;               // lower-case names
    std::string header(std::string_view name) const { return headers().get(name); }
    int64_t contentLength() const;                // -1 when unknown
    size_t read(char* buf, size_t n) override;    // throws ClientError
    int64_t length() const override { return contentLength(); }
    std::string readAll(int64_t limit = 64LL << 20);   // throws TooLarge past limit
    void close();

    struct Impl;
    explicit ClientResponse(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> d_;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Sends the request and returns once the response headers arrived. Any HTTP status is a normal response;
    // transport failures, timeouts and cancels throw ClientError.
    std::unique_ptr<ClientResponse> send(const ClientRequest& req);

    // Convenience for small bodies; never throws (status 0 + error on a transport failure).
    struct Result {
        int status = 0;
        Headers headers;
        std::string body;
        std::string error;
    };
    Result fetch(std::string_view method, std::string_view url, const Headers& headers = {},
                 std::string_view body = {}, int idleMs = 30000, int64_t maxBody = 64LL << 20);

    static HttpClient& shared();                  // process-wide session

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace anfs::http
