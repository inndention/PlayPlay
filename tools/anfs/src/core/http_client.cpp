// core/http_client.cpp - ANFS-F5: outbound HTTP on WinHTTP (anfs-cpp.md 2.5, frozen header core/http_client.h).
//
// Why asynchronous WinHTTP under a blocking API: a synchronous WinHTTP call can only be cancelled by closing its
// handle from another thread, and the owner thread may be just about to use that handle (a closed HINTERNET value can
// be reused by the next request). Here every WinHTTP call is started in WINHTTP_FLAG_ASYNC mode and the owner thread
// waits on {completion event, cancel event} with a safety timeout. Only the owner ever closes the request handle, and
// it waits for WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING before the per-request state (and the I/O buffer WinHTTP writes
// into) is freed. A CancelToken is a manual-reset event, so one token aborts any number of requests at once.
//
// Wire rules:
// - Session: WINHTTP_ACCESS_TYPE_NO_PROXY, no User-Agent (Node sends none), redirect policy NEVER plus
//   WINHTTP_DISABLE_REDIRECTS, cookies and automatic authentication off (a 401 is an ordinary response), unlimited
//   connections per server. Keep-alive connections are pooled per session (HttpClient), so a heartbeat reuses one.
// - Exact request head (raw capture test): request line, "Connection: Keep-Alive" (WinHTTP's own; Node's keep-alive
//   agent sends "keep-alive"), the caller's headers in order, the framing header, then "Host: host[:port]" last.
//   WinHTTP munges headers it knows: Accept, Accept-Encoding, Authorization, Cookie, If-None-Match, Referer, Range and
//   User-Agent move to the front in canonical case (alphabetical), and a caller Connection header merges into its own.
//   Names are case-insensitive and order-free for every receiver here, so this is accepted and pinned by the test.
//   Request headers go out in the caller's order; Host is WinHTTP's (from the URL), Content-Length / Transfer-Encoding
//   are computed here and written by us (WinHttpSendRequest gets WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, so bodies over
//   4 GiB work and WinHTTP adds nothing). Chunked framing is written by us. Header bytes are sent as given (an ASCII
//   block through WinHttpSendRequest, values with bytes >= 0x80 through WinHttpAddRequestHeadersEx); CR, LF or NUL in a
//   header is refused (Node ERR_INVALID_CHAR) and the value is never echoed in the error.
// - Without a body, methods other than GET/HEAD/DELETE/OPTIONS/TRACE/CONNECT send "Content-Length: 0" (Node's
//   useChunkedEncodingByDefault methods end() with a zero length).
// - The request path is sent as given except that bytes outside 0x21..0x7E are percent-encoded (Node would refuse a
//   space and mangle non-ASCII); WinHTTP's own escaping is off.
// - https: SECURITY_FLAG_IGNORE_UNKNOWN_CA | CERT_DATE_INVALID | CERT_CN_INVALID | CERT_WRONG_USAGE (Node
//   rejectUnauthorized:false), no client certificate, no revocation check.
// - Timeouts are inactivity timeouts: WinHttpSetTimeouts(resolve, connect, send = idle, receive = none) plus our own
//   inactivity clock per step driven by WinHTTP's connect / sent / received notifications (Impl::limitFor), which
//   closes the request handle when it expires. WinHTTP's own receive timeout is not used: it does not cover
//   QueryDataAvailable, and when it expires inside SendRequest WinHTTP retries the request on fresh connections for
//   seconds before reporting 12002. Note that for a request without a body WinHTTP completes WinHttpSendRequest only
//   when the response starts to arrive (measured: SENDREQUEST_COMPLETE never came from a silent server), so the
//   "send request" step is also where a silent server times out.
// - Responses: header names lower-cased, values trimmed, duplicates kept in order (Headers::joined gives Node's
//   ", " form); the body ends early -> ClientError (a Content-Length that was not met is never a silent EOF). At the
//   end of the body the request handle is closed at once, which hands the connection back to the pool.
// - Error texts name the scheme, host, port and path, never the query (it may carry auth=).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "core/http_client.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace anfs::http {

namespace {

constexpr int kSafetyMs = 2000;          // extra wait past WinHTTP's own timeout before we give up on it
constexpr int kCloseWaitMs = 15000;      // wait for HANDLE_CLOSING; past it the request state is leaked, never freed
constexpr size_t kIoBytes = kCopyBufferBytes;
constexpr size_t kFramePrefix = 8;       // room for "10000\r\n" in front of a chunk

std::wstring widenLatin1(std::string_view s)
{
    std::wstring w(s.size(), L'\0');
    for (size_t i = 0; i < s.size(); ++i) w[i] = static_cast<unsigned char>(s[i]);
    return w;
}

// Header text from WinHTTP back to bytes: code units below 256 are the wire bytes (latin1); anything else (WinHTTP
// decoded a multi-byte sequence) becomes UTF-8.
std::string narrowHeader(const wchar_t* p, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        unsigned c = p[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<unsigned>(p[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x100) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string lowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string_view trimOws(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string errorText(DWORD e)
{
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "name not resolved";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection reset or closed";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "invalid server response";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS failure";
    case ERROR_WINHTTP_INVALID_URL: return "invalid url";
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "unsupported url scheme";
    case ERROR_WINHTTP_HEADER_SIZE_OVERFLOW: return "response headers too large";
    case ERROR_WINHTTP_RESEND_REQUEST: return "server asked to resend";
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return "client certificate required";
    case ERROR_INVALID_PARAMETER: return "invalid parameter";
    case ERROR_NOT_ENOUGH_MEMORY: return "out of memory";
    default: break;
    }
    wchar_t buf[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), e, 0, buf, 256, nullptr);
    while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ' || buf[n - 1] == L'.')) --n;
    if (!n) return "error";
    std::string s;
    for (DWORD i = 0; i < n; ++i) s += buf[i] < 0x80 ? static_cast<char>(buf[i]) : '?';
    return s;
}

struct Url {
    bool secure = false;
    std::string host;          // without brackets
    int port = 0;
    std::string pathQuery;     // escaped request target
    std::string label;         // scheme://host:port/path, no query (error texts)
};

[[noreturn]] void badUrl(const std::string& why, DWORD code = ERROR_WINHTTP_INVALID_URL)
{
    throw ClientError("invalid url: " + why, code);
}

Url parseUrl(std::string_view url)
{
    Url u;
    size_t p = url.find("://");
    if (p == std::string_view::npos) badUrl("no scheme");
    std::string scheme = lowerAscii(std::string(url.substr(0, p)));
    if (scheme == "https") u.secure = true;
    else if (scheme != "http") badUrl("scheme " + scheme, ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    std::string_view rest = url.substr(p + 3);
    size_t end = rest.find_first_of("/?#");
    std::string_view auth = rest.substr(0, end);
    std::string_view tail = end == std::string_view::npos ? std::string_view() : rest.substr(end);
    if (auth.find('@') != std::string_view::npos) badUrl("credentials in the url are not supported");
    std::string_view portText;
    if (!auth.empty() && auth.front() == '[') {
        size_t close = auth.find(']');
        if (close == std::string_view::npos) badUrl("unterminated IPv6 address");
        u.host = std::string(auth.substr(1, close - 1));
        std::string_view after = auth.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') badUrl("bad authority");
            portText = after.substr(1);
        }
    } else {
        size_t colon = auth.rfind(':');
        u.host = std::string(auth.substr(0, colon));
        if (colon != std::string_view::npos) portText = auth.substr(colon + 1);
    }
    if (u.host.empty()) badUrl("no host");
    for (char c : u.host)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '/' || c == '\\' || c == '%') badUrl("bad host");
    u.port = u.secure ? 443 : 80;
    if (!portText.empty()) {
        long v = 0;
        for (char c : portText) {
            if (c < '0' || c > '9' || v > 65535) badUrl("bad port");
            v = v * 10 + (c - '0');
        }
        if (v < 1 || v > 65535) badUrl("bad port");
        u.port = static_cast<int>(v);
    }
    size_t hash = tail.find('#');
    if (hash != std::string_view::npos) tail = tail.substr(0, hash);
    std::string target = tail.empty() || tail.front() == '?' ? "/" + std::string(tail) : std::string(tail);
    static const char* hex = "0123456789ABCDEF";
    for (char ch : target) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) {
            u.pathQuery += '%';
            u.pathQuery += hex[c >> 4];
            u.pathQuery += hex[c & 15];
        } else {
            u.pathQuery += ch;
        }
    }
    size_t q = u.pathQuery.find('?');
    u.label = scheme + "://" + std::string(auth) + u.pathQuery.substr(0, q);
    return u;
}

bool methodSendsZeroLength(std::string_view m)
{
    return !(m == "GET" || m == "HEAD" || m == "DELETE" || m == "OPTIONS" || m == "TRACE" || m == "CONNECT");
}

// One WinHTTP session (shared by the client and every response it handed out, so a response may outlive the
// HttpClient object without dangling).
struct Session {
    HINTERNET h = nullptr;
    Session()
    {
        h = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                        WINHTTP_FLAG_ASYNC);
        if (!h) {
            DWORD e = GetLastError();
            throw ClientError("WinHttpOpen failed: " + errorText(e) + " (" + std::to_string(e) + ")", e);
        }
        DWORD v = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &v, sizeof v);
        v = INFINITE;
        WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &v, sizeof v);
        WinHttpSetOption(h, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &v, sizeof v);
    }
    ~Session()
    {
        if (h) WinHttpCloseHandle(h);
    }
};

// Per-request state shared with the WinHTTP callback. Freed by the owner only after HANDLE_CLOSING arrived.
struct Op {
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);     // auto-reset: one completion
    HANDLE progress = CreateEventW(nullptr, FALSE, FALSE, nullptr); // activity changed: the waiter recomputes its limit
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);    // HANDLE_CLOSING seen
    std::atomic<DWORD> err{0};
    std::atomic<DWORD> bytes{0};
    std::atomic<ULONGLONG> activity{0};    // tick of the last connect / request bytes sent / response bytes received
    std::atomic<bool> connected{false};
    std::vector<char> buf = std::vector<char>(kFramePrefix + kIoBytes + 2);
    ~Op()
    {
        if (done) CloseHandle(done);
        if (progress) CloseHandle(progress);
        if (closed) CloseHandle(closed);
    }
};

void CALLBACK onStatus(HINTERNET, DWORD_PTR context, DWORD code, LPVOID info, DWORD infoLen)
{
    Op* op = reinterpret_cast<Op*>(context);
    if (!op) return;
    switch (code) {
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
        op->connected = true;
        op->activity = GetTickCount64();
        SetEvent(op->progress);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_SENT:
    case WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED:
        op->activity = GetTickCount64();
        SetEvent(op->progress);
        break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        op->err = 0;
        SetEvent(op->done);
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
        op->bytes = info ? *static_cast<DWORD*>(info) : 0;
        op->err = 0;
        SetEvent(op->done);
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        op->bytes = infoLen;
        op->err = 0;
        SetEvent(op->done);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        DWORD e = info ? static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : 0;
        op->err = e ? e : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR);
        SetEvent(op->done);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(op->closed);    // last touch of op by WinHTTP
        break;
    default:
        break;
    }
}

} // namespace

// ---- CancelToken -------------------------------------------------------------------------------------------------

struct CancelToken::State {
    std::atomic<bool> flag{false};
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);        // manual reset: wakes every waiter
    ~State()
    {
        if (ev) CloseHandle(ev);
    }
};

CancelToken::CancelToken() : s_(std::make_shared<State>()) {}
CancelToken::~CancelToken() = default;

void CancelToken::cancel()
{
    s_->flag = true;
    SetEvent(s_->ev);
}

bool CancelToken::cancelled() const { return s_->flag.load(); }

// ---- ClientResponse::Impl: owns the request handles and runs every WinHTTP step ------------------------------------

struct ClientResponse::Impl {
    std::shared_ptr<Session> session;
    std::shared_ptr<CancelToken::State> tok;
    Op* op = new Op();
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;
    Url url;
    std::string method;
    int idleMs = 30000;                                     // 0 = no timeout (also for connect / resolve below)
    int connectMs = 10000;
    int resolveMs = 10000;

    int status = 0;
    Headers headers;
    int64_t contentLength = -1;
    bool bodyless = false;
    int64_t received = 0;
    bool eof = false;
    bool closedByUser = false;

    ~Impl() { release(); }

    // Closes the handles (aborting any pending operation) and frees op once WinHTTP let go of it.
    void release() noexcept
    {
        if (hRequest) {
            HINTERNET h = hRequest;
            hRequest = nullptr;
            WinHttpCloseHandle(h);
            if (WaitForSingleObject(op->closed, kCloseWaitMs) != WAIT_OBJECT_0) op = nullptr;   // leak, never free
        }
        if (hConnect) {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        delete op;
        op = nullptr;
    }

    [[noreturn]] void fail(const char* what, DWORD e)
    {
        const bool cancelled = tok && tok->flag.load();
        release();
        if (cancelled) throw ClientError(std::string(what) + " " + url.label + ": cancelled",
                                         ERROR_WINHTTP_OPERATION_CANCELLED, false, true);
        throw ClientError(std::string(what) + " " + url.label + ": " + errorText(e) + " (" + std::to_string(e) + ")", e,
                          e == ERROR_WINHTTP_TIMEOUT, false);
    }

    void checkUsable(const char* what)
    {
        if (tok && tok->flag.load()) fail(what, ERROR_WINHTTP_OPERATION_CANCELLED);
        if (!hRequest || !op)
            throw ClientError(std::string(what) + " " + url.label + ": " + (closedByUser ? "response closed" : "request ended"),
                              ERROR_WINHTTP_OPERATION_CANCELLED, false, closedByUser);
    }

    // Our own inactivity clock, measured from the later of the step's start and the last network activity WinHTTP
    // reported. Send: resolve + connect + idle until connected, then idle. Wait (receive response, data available,
    // read): idle. Write: idle + 2 s (a 64 KB write to a slow reader reports no progress until it completes;
    // WinHTTP's send timeout covers each socket send).
    enum class Phase { Send, Write, Wait };
    ULONGLONG limitFor(Phase ph) const
    {
        if (!idleMs) return 0;
        switch (ph) {
        case Phase::Send:
            if (op->connected.load()) return static_cast<ULONGLONG>(idleMs);
            return connectMs && resolveMs ? static_cast<ULONGLONG>(resolveMs) + connectMs + idleMs : 0;
        case Phase::Write: return static_cast<ULONGLONG>(idleMs) + kSafetyMs;
        case Phase::Wait: return static_cast<ULONGLONG>(idleMs);
        }
        return 0;
    }

    // Starts one asynchronous WinHTTP call and waits for its completion, the cancel token or the inactivity limit.
    // The progress event wakes the wait whenever WinHTTP reports activity, so a limit that shrinks (resolve +
    // connect + idle until connected, idle after) or a deadline that moves is picked up at once instead of after the
    // slice that was computed before the notification arrived.
    template <class Start>
    void step(const char* what, Phase ph, Start&& start)
    {
        checkUsable(what);
        op->err = 0;
        op->bytes = 0;
        ResetEvent(op->done);
        ResetEvent(op->progress);
        const ULONGLONG begin = GetTickCount64();
        if (!start()) fail(what, GetLastError());
        HANDLE hs[3] = {op->done, op->progress, tok ? tok->ev : nullptr};
        for (;;) {
            DWORD wait = INFINITE;
            if (const ULONGLONG lim = limitFor(ph)) {
                const ULONGLONG deadline = std::max(begin, op->activity.load()) + lim, now = GetTickCount64();
                if (now >= deadline) fail(what, ERROR_WINHTTP_TIMEOUT);
                wait = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 0x7FFFFFFF));
            }
            const DWORD r = WaitForMultipleObjects(tok ? 3 : 2, hs, FALSE, wait);
            if (r == WAIT_OBJECT_0) {
                if (DWORD e = op->err.load()) fail(what, e);
                return;
            }
            if (r == WAIT_OBJECT_0 + 2) fail(what, ERROR_WINHTTP_OPERATION_CANCELLED);
            if (r != WAIT_OBJECT_0 + 1 && r != WAIT_TIMEOUT) fail(what, GetLastError());
            // progress or the slice ended: loop, the limit and the deadline are recomputed
        }
    }

    // Writes the first n bytes of op->buf. The data always lives in op->buf, never in caller memory, because an
    // aborted write may still be read by WinHTTP until HANDLE_CLOSING (and op is leaked, not freed, if that never
    // comes).
    void writeBuf(size_t n)
    {
        while (n) {
            const DWORD k = static_cast<DWORD>(n);
            step("send body", Phase::Write, [&] { return WinHttpWriteData(hRequest, op->buf.data(), k, nullptr); });
            const DWORD w = op->bytes.load();
            if (w == 0 || w > k) fail("send body", ERROR_WINHTTP_CONNECTION_ERROR);
            if (w < k) std::memmove(op->buf.data(), op->buf.data() + w, k - w);
            n -= w;
        }
    }

    // Reads from the caller's body source into op->buf at offset; a throwing source aborts the request and its
    // exception reaches the caller unchanged (a relayed client that left stays IoError{peerGone}).
    size_t pull(Source& src, size_t offset, size_t n)
    {
        try {
            return src.read(op->buf.data() + offset, n);
        } catch (...) {
            release();
            throw;
        }
    }

    void sendBody(Source* body, int64_t length)
    {
        if (!body) return;
        if (length >= 0) {
            int64_t left = length;
            while (left > 0) {
                checkUsable("send body");
                size_t k = pull(*body, 0, static_cast<size_t>(std::min<int64_t>(left, kIoBytes)));
                if (k == 0) {
                    release();
                    throw ClientError("send body " + url.label + ": the body source ended " + std::to_string(left) +
                                          " bytes before its length", ERROR_INVALID_DATA);
                }
                writeBuf(k);
                left -= static_cast<int64_t>(k);
            }
            return;
        }
        // Chunked framing written here: "<hex>\r\n<data>\r\n" in one write, then "0\r\n\r\n".
        for (;;) {
            checkUsable("send body");
            size_t k = pull(*body, kFramePrefix, kIoBytes);
            if (k == 0) break;
            char head[16];
            int hn = std::snprintf(head, sizeof head, "%zx\r\n", k);
            const size_t at = kFramePrefix - static_cast<size_t>(hn);
            std::memcpy(op->buf.data() + at, head, static_cast<size_t>(hn));
            std::memcpy(op->buf.data() + kFramePrefix + k, "\r\n", 2);
            const size_t total = static_cast<size_t>(hn) + k + 2;
            if (at) std::memmove(op->buf.data(), op->buf.data() + at, total);
            writeBuf(total);
        }
        std::memcpy(op->buf.data(), "0\r\n\r\n", 5);
        writeBuf(5);
    }

    void readHeaders()
    {
        DWORD st = 0, len = sizeof st;
        if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &st, &len, WINHTTP_NO_HEADER_INDEX))
            fail("read headers", GetLastError());
        status = static_cast<int>(st);
        len = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                            WINHTTP_NO_HEADER_INDEX);
        std::wstring raw(len / sizeof(wchar_t) + 1, L'\0');
        len = static_cast<DWORD>(raw.size() * sizeof(wchar_t));
        if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &len,
                                 WINHTTP_NO_HEADER_INDEX))
            fail("read headers", GetLastError());
        raw.resize(len / sizeof(wchar_t));
        std::string text = narrowHeader(raw.data(), raw.size());
        size_t i = text.find("\r\n");                    // skip the status line
        i = i == std::string::npos ? text.size() : i + 2;
        while (i < text.size()) {
            size_t e = text.find("\r\n", i);
            if (e == std::string::npos) e = text.size();
            std::string_view line(text.data() + i, e - i);
            i = e + 2;
            size_t c = line.find(':');
            if (line.empty() || c == std::string_view::npos || c == 0) continue;
            headers.add(lowerAscii(std::string(trimOws(line.substr(0, c)))), std::string(trimOws(line.substr(c + 1))));
        }
        bodyless = method == "HEAD" || status == 204 || status == 304 || (status >= 100 && status < 200);
        const std::string te = headers.joined("transfer-encoding");
        const std::string* cl = headers.find("content-length");
        if (cl && te.empty()) {
            int64_t v = 0;
            bool ok = !cl->empty();
            for (char c : *cl) {
                if (c < '0' || c > '9' || v > (INT64_MAX - 9) / 10) {
                    ok = false;
                    break;
                }
                v = v * 10 + (c - '0');
            }
            if (ok) contentLength = v;
        }
    }

    size_t read(char* out, size_t n)
    {
        if (eof || n == 0) return 0;
        checkUsable("read body");
        // WinHttpReadData waits until the whole buffer is filled (and its receive timeout does not cover that wait),
        // so a stream would stall on a slow sender. QueryDataAvailable completes as soon as any byte is there; the
        // read of at most that many bytes then completes at once. 0 available = end of the body.
        step("read body", Phase::Wait, [&] { return WinHttpQueryDataAvailable(hRequest, nullptr); });
        DWORD k = op->bytes.load();
        if (k) {
            const DWORD want = static_cast<DWORD>(std::min<size_t>({n, kIoBytes, static_cast<size_t>(k)}));
            step("read body", Phase::Wait, [&] { return WinHttpReadData(hRequest, op->buf.data(), want, nullptr); });
            k = op->bytes.load();
        }
        if (k == 0) {
            eof = true;
            if (!bodyless && contentLength >= 0 && received < contentLength) {
                release();
                throw ClientError("read body " + url.label + ": the response ended " +
                                      std::to_string(contentLength - received) + " bytes early",
                                  ERROR_WINHTTP_CONNECTION_ERROR);
            }
            release();                                   // complete: the connection goes back to the pool
            return 0;
        }
        std::memcpy(out, op->buf.data(), k);
        received += k;
        return k;
    }
};

ClientResponse::ClientResponse(std::unique_ptr<Impl> impl) : d_(std::move(impl)) {}
ClientResponse::~ClientResponse() = default;
int ClientResponse::status() const { return d_->status; }
const Headers& ClientResponse::headers() const { return d_->headers; }
int64_t ClientResponse::contentLength() const { return d_->contentLength; }
size_t ClientResponse::read(char* buf, size_t n) { return d_->read(buf, n); }

std::string ClientResponse::readAll(int64_t limit)
{
    std::string out;
    if (d_->contentLength > 0 && !d_->bodyless) {
        if (limit >= 0 && d_->contentLength > limit) {
            close();
            throw TooLarge(limit);
        }
        out.reserve(static_cast<size_t>(d_->contentLength));
    }
    std::vector<char> buf(kIoBytes);
    for (;;) {
        size_t k = read(buf.data(), buf.size());
        if (k == 0) break;
        if (limit >= 0 && static_cast<int64_t>(out.size() + k) > limit) {
            close();
            throw TooLarge(limit);
        }
        out.append(buf.data(), k);
    }
    return out;
}

void ClientResponse::close()
{
    if (!d_->eof) d_->closedByUser = true;
    d_->release();
}

// ---- HttpClient ----------------------------------------------------------------------------------------------------

struct HttpClient::Impl {
    std::shared_ptr<Session> session = std::make_shared<Session>();
};

HttpClient::HttpClient() : d_(std::make_unique<Impl>()) {}
HttpClient::~HttpClient() = default;

HttpClient& HttpClient::shared()
{
    static HttpClient* c = new HttpClient();        // never destroyed: no WinHTTP teardown during static destruction
    return *c;
}

std::unique_ptr<ClientResponse> HttpClient::send(const ClientRequest& req)
{
    auto r = std::make_unique<ClientResponse::Impl>();
    r->session = d_->session;
    r->tok = req.cancel ? req.cancel->state() : nullptr;
    r->url = parseUrl(req.url);
    r->method = req.method.empty() ? std::string("GET") : req.method;
    for (char c : r->method)
        if (c <= 0x20 || c >= 0x7F || c == '(' || c == ')' || c == ',' || c == '/' || c == ':' || c == ';' ||
            c == '<' || c == '=' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' ||
            c == '{' || c == '}' || c == '"')
            throw ClientError("invalid method", ERROR_INVALID_PARAMETER);
    if (req.body && req.bodyLength < -1) throw ClientError("invalid body length", ERROR_INVALID_PARAMETER);
    if (!req.body && req.bodyLength > 0) throw ClientError("body length without a body source", ERROR_INVALID_PARAMETER);

    // Headers in the caller's order, then the computed framing header.
    std::vector<std::pair<std::string, std::string>> hl;
    bool ascii = true;
    for (const auto& f : req.headers.fields()) {
        if (ieq(f.first, "host") || ieq(f.first, "content-length") || ieq(f.first, "transfer-encoding")) continue;
        if (f.first.empty() || f.first.find_first_of(":\r\n\0 \t", 0, 6) != std::string::npos ||
            f.second.find_first_of("\r\n\0", 0, 3) != std::string::npos)
            throw ClientError("invalid header name or value", ERROR_INVALID_PARAMETER);   // value never echoed
        for (char c : f.first)
            if (static_cast<unsigned char>(c) >= 0x7F || static_cast<unsigned char>(c) < 0x21)
                throw ClientError("invalid header name or value", ERROR_INVALID_PARAMETER);
        for (char c : f.second) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
        hl.emplace_back(f.first, f.second);
    }
    if (req.body) {
        hl.emplace_back(req.bodyLength >= 0 ? "Content-Length" : "Transfer-Encoding",
                        req.bodyLength >= 0 ? std::to_string(req.bodyLength) : std::string("chunked"));
    } else if (methodSendsZeroLength(r->method)) {
        hl.emplace_back("Content-Length", "0");
    }
    std::string block;
    for (const auto& h : hl) block += h.first + ": " + h.second + "\r\n";

    const int idle = req.idleMs > 0 ? req.idleMs : 0;                  // 0 = WinHTTP infinite
    const int conn = req.connectMs > 0 ? req.connectMs : 0;
    const int resolve = req.resolveMs > 0 ? req.resolveMs : 0;
    r->idleMs = idle;
    r->connectMs = conn;
    r->resolveMs = resolve;

    if (r->tok && r->tok->flag.load())
        throw ClientError("request " + r->url.label + ": cancelled", ERROR_WINHTTP_OPERATION_CANCELLED, false, true);

    const std::wstring host = widenLatin1(r->url.host);
    r->hConnect = WinHttpConnect(r->session->h, host.c_str(), static_cast<INTERNET_PORT>(r->url.port), 0);
    if (!r->hConnect) r->fail("connect", GetLastError());
    const std::wstring verb = widenLatin1(r->method), target = widenLatin1(r->url.pathQuery);
    r->hRequest = WinHttpOpenRequest(r->hConnect, verb.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY |
                                         (r->url.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!r->hRequest) r->fail("open request", GetLastError());
    DWORD_PTR ctx = reinterpret_cast<DWORD_PTR>(r->op);
    WinHttpSetOption(r->hRequest, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof ctx);
    if (WinHttpSetStatusCallback(r->hRequest, onStatus,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES |
                                     WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER | WINHTTP_CALLBACK_FLAG_SEND_REQUEST |
                                     WINHTTP_CALLBACK_FLAG_RECEIVE_RESPONSE, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
        r->fail("open request", GetLastError());
    DWORD v = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(r->hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &v, sizeof v);
    v = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(r->hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &v, sizeof v);
    if (r->url.secure) {
        v = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (!WinHttpSetOption(r->hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &v, sizeof v))
            r->fail("open request", GetLastError());
        WinHttpSetOption(r->hRequest, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, WINHTTP_NO_CLIENT_CERT_CONTEXT, 0);
    }
    // Resolve, connect and send timeouts are WinHTTP's; the receive side is ours alone (Impl::limitFor closes the
    // handle). A WinHTTP receive timeout shorter than the server's silence made WinHttpSendRequest itself take
    // seconds: WinHTTP starts receiving inside SendRequest, times out, and retries the request on new connections a
    // few times before reporting 12002 (measured: 4.5 s for a 300 ms timeout, one accept per retry).
    if (!WinHttpSetTimeouts(r->hRequest, resolve, conn, idle, 0)) r->fail("open request", GetLastError());
    v = INFINITE;
    WinHttpSetOption(r->hRequest, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &v, sizeof v);

    // WinHTTP refuses a wide header block with code units >= 0x80 (ERROR_INVALID_PARAMETER). Values with such bytes
    // (latin1 file names in Content-Disposition, Node writes them as latin1) go through WinHttpAddRequestHeadersEx
    // with narrow strings, which puts the bytes on the wire unchanged (Windows 10 2004+; earlier systems refuse).
    std::wstring wblock;
    if (ascii) {
        wblock = widenLatin1(block);
    } else if (!hl.empty()) {
        using AddEx = DWORD(WINAPI*)(HINTERNET, DWORD, ULONGLONG, ULONGLONG, DWORD, WINHTTP_EXTENDED_HEADER*);
        static const AddEx addEx = reinterpret_cast<AddEx>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"winhttp.dll"), "WinHttpAddRequestHeadersEx")));
        if (!addEx) r->fail("send request", ERROR_INVALID_PARAMETER);
        std::vector<WINHTTP_EXTENDED_HEADER> xs(hl.size());
        for (size_t i = 0; i < hl.size(); ++i) {
            xs[i].pszName = hl[i].first.c_str();
            xs[i].pszValue = hl[i].second.c_str();
        }
        if (DWORD e = addEx(r->hRequest, WINHTTP_ADDREQ_FLAG_ADD, 0, 0, static_cast<DWORD>(xs.size()), xs.data()))
            r->fail("send request", e);
    }
    r->step("send request", ClientResponse::Impl::Phase::Send, [&] {
        return WinHttpSendRequest(r->hRequest, wblock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wblock.c_str(),
                                  wblock.empty() ? 0 : static_cast<DWORD>(wblock.size()), WINHTTP_NO_REQUEST_DATA, 0,
                                  WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, ctx);
    });
    r->sendBody(req.body, req.bodyLength);
    r->step("receive response", ClientResponse::Impl::Phase::Wait, [&] { return WinHttpReceiveResponse(r->hRequest, nullptr); });
    r->readHeaders();
    if (r->bodyless) {
        // Nothing to read: hand the connection back now (a HEAD reply keeps its GET Content-Length header).
        r->eof = true;
        r->release();
    }
    return std::make_unique<ClientResponse>(std::move(r));
}

HttpClient::Result HttpClient::fetch(std::string_view method, std::string_view url, const Headers& headers,
                                     std::string_view body, int idleMs, int64_t maxBody)
{
    Result out;
    try {
        ClientRequest q;
        q.method = std::string(method);
        q.url = std::string(url);
        q.headers = headers;
        q.idleMs = idleMs;
        StringSource src(body);
        if (!body.empty()) {
            q.body = &src;
            q.bodyLength = static_cast<int64_t>(body.size());
        }
        auto res = send(q);
        out.status = res->status();
        out.headers = res->headers();
        out.body = res->readAll(maxBody);
    } catch (const TooLarge&) {
        out = Result{};
        out.error = "response body larger than " + std::to_string(maxBody) + " bytes";
    } catch (const std::exception& e) {
        out = Result{};
        out.error = e.what();
    } catch (...) {
        out = Result{};
        out.error = "request failed";
    }
    return out;
}

} // namespace anfs::http
