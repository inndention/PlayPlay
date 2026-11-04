// core/http_internal.h - glue between core/http_server.cpp and core/http_ctx.cpp (ANFS-F4a). Not a module API:
// handlers use http_ctx.h only. Kept out of the frozen headers (whose Response / BodyReader have no hooks for it).
//
// The server opens a RequestScope on the worker's stack for each request before it constructs the BodyReader and the
// Response. Both pick the scope up in their constructors (thread-local, so only objects built on that worker during
// that request see it); objects built elsewhere (test doubles) get the defaults.
#pragma once

#include "core/http_ctx.h"

#include <atomic>
#include <string>
#include <string_view>

namespace anfs::http::detail {

struct RequestScope {
    int keepAliveSecs = 5;              // Keep-Alive: timeout=<secs> (floor of Node keepAliveTimeout / 1000); < 0 = left out
    const Response* res = nullptr;      // "100 Continue" is sent on the first body read only while this has not started
    bool continueSent = false;          // set by the BodyReader (also when it skipped the 100 because a reply started)
    bool bodyBroken = false;            // the body framing broke, the client left mid-body or a read timed out
    const std::atomic<bool>* closing = nullptr;   // set while the server drains: replies say Connection: close
};

// Sets the thread's current scope for its lifetime (nesting restores the previous one).
class ScopeGuard {
public:
    explicit ScopeGuard(RequestScope& s);
    ~ScopeGuard();
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
private:
    RequestScope* prev_;
};
RequestScope* currentScope();

// Clears the headers that must not leak into an error reply written after a handler failed (C# SafeError).
void resetForError(Response& res);

// Node's bare protocol-error reply ("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"): no Date, no body.
std::string bareReply(int status);

// ASCII helpers shared by the parser, the router and the context.
bool iequals(std::string_view a, std::string_view b);
std::string_view trimOws(std::string_view s);              // spaces and tabs
bool isTokenChar(unsigned char c);                          // RFC 9110 tchar
// True when the comma-separated header value holds token (any case), e.g. Connection: "keep-alive, Upgrade".
bool hasToken(std::string_view headerValue, std::string_view token);

} // namespace anfs::http::detail
