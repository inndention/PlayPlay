// core/http_ctx.cpp - ANFS-F4a: headers, query, the streaming request body (Content-Length and chunked framing with
// trailers, 100-continue) and the streaming response (ordered headers, Content-Length or chunked, HEAD suppression),
// matched to the bytes Node 22's http module writes (checked against node 22.13.1, see test_anfs_core_http).
//
// Response header order follows Express on Node: the handler's headers in the order set, then Content-Length (Express
// res.send sets it as an ordinary header), then Date, Connection, Keep-Alive, and Transfer-Encoding last (Node adds
// it itself). Headers are written lazily, together with the first body bytes (Node's writeHead is lazy too), so a
// small reply goes out in one send().
#include "core/http_ctx.h"

#include "core/clock.h"
#include "core/http_internal.h"
#include "core/http_server.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace anfs::http {

// ---- small helpers ----------------------------------------------------------------------------------------------

namespace detail {

static inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

std::string_view trimOws(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool isTokenChar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    return c != 0 && std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

bool hasToken(std::string_view v, std::string_view token)
{
    size_t i = 0;
    while (i <= v.size()) {
        size_t j = v.find(',', i);
        if (j == std::string_view::npos) j = v.size();
        if (iequals(trimOws(v.substr(i, j - i)), token)) return true;
        i = j + 1;
    }
    return false;
}

std::string bareReply(int status)
{
    std::string s = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\nConnection: close\r\n\r\n";
    return s;
}

} // namespace detail

using detail::iequals;

const char* reasonPhrase(int status)
{
    // Node 22.13.1 http.STATUS_CODES, in its order.
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a Teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 509: return "Bandwidth Limit Exceeded";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: return "unknown";   // Node: res.statusMessage = STATUS_CODES[code] || 'unknown'
    }
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(std::string_view s, bool plusAsSpace)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>(h * 16 + l));
                i += 2;
                continue;
            }
        }
        out.push_back(plusAsSpace && c == '+' ? ' ' : c);
    }
    return out;
}

static const char kHex[] = "0123456789ABCDEF";

std::string encodeURIComponent(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            (c != 0 && std::strchr("-_.!~*'()", c))) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

// ---- Headers ----------------------------------------------------------------------------------------------------

const std::string* Headers::find(std::string_view name) const
{
    for (const Field& f : f_)
        if (iequals(f.first, name)) return &f.second;
    return nullptr;
}

std::string Headers::get(std::string_view name, std::string_view def) const
{
    const std::string* v = find(name);
    return v ? *v : std::string(def);
}

std::string Headers::joined(std::string_view name) const
{
    std::string out;
    bool any = false;
    for (const Field& f : f_) {
        if (!iequals(f.first, name)) continue;
        if (any) out += ", ";
        out += f.second;
        any = true;
    }
    return out;
}

std::vector<std::string> Headers::getAll(std::string_view name) const
{
    std::vector<std::string> out;
    for (const Field& f : f_)
        if (iequals(f.first, name)) out.push_back(f.second);
    return out;
}

void Headers::add(std::string name, std::string value) { f_.emplace_back(std::move(name), std::move(value)); }

void Headers::set(std::string name, std::string value)
{
    auto it = std::find_if(f_.begin(), f_.end(), [&](const Field& f) { return iequals(f.first, name); });
    if (it == f_.end()) {
        f_.emplace_back(std::move(name), std::move(value));
        return;
    }
    size_t at = static_cast<size_t>(it - f_.begin());
    f_.erase(std::remove_if(f_.begin() + static_cast<ptrdiff_t>(at) + 1, f_.end(),
                            [&](const Field& f) { return iequals(f.first, name); }),
             f_.end());
    f_[at].first = std::move(name);
    f_[at].second = std::move(value);
}

bool Headers::remove(std::string_view name)
{
    size_t before = f_.size();
    f_.erase(std::remove_if(f_.begin(), f_.end(), [&](const Field& f) { return iequals(f.first, name); }), f_.end());
    return f_.size() != before;
}

// ---- Query (Express 4 req.query through qs 6, flat keys) --------------------------------------------------------

// qs utils.decode: '+' -> ' ', then decodeURIComponent; when that throws (a bad escape or bytes that are not UTF-8)
// the plus-replaced text is kept as it is.
static bool validUtf8(std::string_view s)
{
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int n = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + static_cast<size_t>(n) >= s.size()) return false;
        for (int k = 1; k <= n; ++k) {
            unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return false;
        i += static_cast<size_t>(n) + 1;
    }
    return true;
}

static std::string qsDecode(std::string_view s)
{
    std::string plus(s);
    std::replace(plus.begin(), plus.end(), '+', ' ');
    std::string out;
    out.reserve(plus.size());
    for (size_t i = 0; i < plus.size(); ++i) {
        if (plus[i] != '%') { out.push_back(plus[i]); continue; }
        if (i + 2 >= plus.size()) return plus;
        int h = hexVal(plus[i + 1]), l = hexVal(plus[i + 2]);
        if (h < 0 || l < 0) return plus;
        out.push_back(static_cast<char>(h * 16 + l));
        i += 2;
    }
    return validUtf8(out) ? out : plus;
}

Query::Query(std::string_view raw)
{
    size_t i = 0, parts = 0;
    while (i <= raw.size() && parts < 1000) {      // qs parameterLimit 1000
        size_t j = raw.find('&', i);
        if (j == std::string_view::npos) j = raw.size();
        std::string_view part = raw.substr(i, j - i);
        i = j + 1;
        ++parts;
        if (part.empty()) continue;
        size_t eq = part.find("]=");
        eq = eq == std::string_view::npos ? part.find('=') : eq + 1;
        std::string key = qsDecode(eq == std::string_view::npos ? part : part.substr(0, eq));
        std::string val = eq == std::string_view::npos ? std::string() : qsDecode(part.substr(eq + 1));
        if (key.empty()) continue;                   // qs drops an empty key
        items_.emplace_back(std::move(key), std::move(val));
    }
}

std::optional<std::string> Query::get(std::string_view key) const
{
    for (const auto& kv : items_)
        if (kv.first == key) return kv.second;
    return std::nullopt;
}

std::string Query::value(std::string_view key, std::string_view def) const
{
    auto v = get(key);
    return v ? *v : std::string(def);
}

std::vector<std::string> Query::all(std::string_view key) const
{
    std::vector<std::string> out;
    for (const auto& kv : items_)
        if (kv.first == key) out.push_back(kv.second);
    return out;
}

// URLSearchParams serializer (application/x-www-form-urlencoded byte serializer).
static void formEncode(std::string& out, std::string_view s)
{
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '*' || c == '-' ||
            c == '.' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
}

std::string Query::build(const std::vector<std::pair<std::string, std::string>>& items)
{
    std::string out;
    for (const auto& kv : items) {
        if (!out.empty()) out.push_back('&');
        formEncode(out, kv.first);
        out.push_back('=');
        formEncode(out, kv.second);
    }
    return out;
}

// ---- BodyReader -------------------------------------------------------------------------------------------------

namespace {
constexpr size_t kChunkLineMax = 4096;          // a chunk-size line with extensions
constexpr size_t kTrailerMax = 64 * 1024;       // the trailer block, like the header block
const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
const char kTooLarge[] = "request entity too large";   // raw-body / body-parser text
} // namespace

struct BodyReader::Impl {
    Impl(detail::Wire& w, const Request& r) : wire(w), req(r)
    {
        detail::RequestScope* cur = detail::currentScope();
        sc = cur ? cur : &own;
    }
    detail::Wire& wire;
    const Request& req;
    detail::RequestScope own;                   // without a server scope (test doubles)
    detail::RequestScope* sc = nullptr;         // continueSent / bodyBroken live here, so the server sees them
    bool chunked = false;
    int64_t remaining = 0;                      // Content-Length mode
    bool done = false;
    int64_t consumed = 0;
    Headers trailers;
    enum class St { Size, Data, DataEnd, Trailers, Done } st = St::Size;
    int64_t chunkLeft = 0;
    size_t trailerBytes = 0;
    std::string buf;                            // read-ahead for the chunked framing; unread() at the end
    size_t pos = 0;

    [[noreturn]] void fail(const char* text)
    {
        sc->bodyBroken = true;
        throw HttpError(400, text);
    }

    void ensureContinue()
    {
        if (!req.expectContinue || sc->continueSent) return;
        sc->continueSent = true;                // also when skipped: a reply already went out (Node sends no 100 then)
        if (sc->res && sc->res->headersSent()) return;
        wire.send(kContinue, sizeof kContinue - 1);
    }

    size_t recvSome(char* p, size_t n)
    {
        size_t k;
        try {
            k = wire.recv(p, n);
        } catch (...) {
            sc->bodyBroken = true;
            throw;
        }
        if (k == 0) {
            sc->bodyBroken = true;
            throw IoError("client closed the connection mid-body", true);
        }
        return k;
    }

    void fill()
    {
        if (pos > 0) {
            buf.erase(0, pos);
            pos = 0;
        }
        char tmp[16 * 1024];
        size_t k = recvSome(tmp, sizeof tmp);
        buf.append(tmp, k);
    }

    // One CRLF-terminated line (without the CRLF); a bare LF or an over-long line is a framing error.
    std::string line(size_t max)
    {
        for (;;) {
            size_t nl = buf.find('\n', pos);
            if (nl != std::string::npos) {
                if (nl == pos || buf[nl - 1] != '\r') fail("invalid chunked body");
                std::string l = buf.substr(pos, nl - 1 - pos);
                if (l.size() > max) fail("invalid chunked body");
                pos = nl + 1;
                return l;
            }
            if (buf.size() - pos > max + 1) fail("invalid chunked body");
            fill();
        }
    }

    void finish()
    {
        st = St::Done;
        done = true;
        if (pos < buf.size()) wire.unread(buf.data() + pos, buf.size() - pos);   // the next pipelined request
        buf.clear();
        pos = 0;
    }

    size_t readChunked(char* out, size_t n)
    {
        for (;;) {
            switch (st) {
            case St::Size: {
                std::string l = line(kChunkLineMax);
                size_t i = 0;
                int64_t v = 0;
                while (i < l.size() && hexVal(l[i]) >= 0) {
                    if (v > (INT64_MAX >> 4)) fail("invalid chunked body");
                    v = v * 16 + hexVal(l[i]);
                    ++i;
                }
                if (i == 0) fail("invalid chunked body");
                // Chunk extensions (";name=value") are ignored; anything else after the size is an error.
                std::string_view restv = detail::trimOws(std::string_view(l).substr(i));
                if (!restv.empty() && restv[0] != ';') fail("invalid chunked body");
                if (v == 0) {
                    st = St::Trailers;
                } else {
                    chunkLeft = v;
                    st = St::Data;
                }
                break;
            }
            case St::Data: {
                size_t want = static_cast<size_t>(std::min<int64_t>(chunkLeft, static_cast<int64_t>(n)));
                size_t k;
                if (pos < buf.size()) {
                    k = std::min(want, buf.size() - pos);
                    std::memcpy(out, buf.data() + pos, k);
                    pos += k;
                } else {
                    k = recvSome(out, want);    // straight from the socket: no copy for big chunks
                }
                chunkLeft -= static_cast<int64_t>(k);
                consumed += static_cast<int64_t>(k);
                if (chunkLeft == 0) st = St::DataEnd;
                return k;
            }
            case St::DataEnd: {
                std::string l = line(0);
                if (!l.empty()) fail("invalid chunked body");
                st = St::Size;
                break;
            }
            case St::Trailers: {
                std::string l = line(kTrailerMax);
                if (l.empty()) {
                    finish();
                    return 0;
                }
                trailerBytes += l.size() + 2;
                if (trailerBytes > kTrailerMax) fail("invalid chunked body");
                size_t c = l.find(':');
                if (c == std::string::npos || c == 0) fail("invalid chunked body");
                std::string_view name = std::string_view(l).substr(0, c);
                for (char ch : name)
                    if (!detail::isTokenChar(static_cast<unsigned char>(ch))) fail("invalid chunked body");
                trailers.add(std::string(name), std::string(detail::trimOws(std::string_view(l).substr(c + 1))));
                break;
            }
            case St::Done:
                return 0;
            }
        }
    }
};

BodyReader::BodyReader(detail::Wire& wire, const Request& req) : d_(std::make_unique<Impl>(wire, req))
{
    d_->chunked = req.chunked;
    d_->remaining = req.chunked ? 0 : std::max<int64_t>(req.contentLength, 0);
    if (!hasBody()) {
        d_->done = true;
        d_->st = Impl::St::Done;
    }
}

BodyReader::~BodyReader() = default;

size_t BodyReader::read(char* buf, size_t n)
{
    Impl& d = *d_;
    if (d.done || n == 0) return 0;
    if (d.sc->bodyBroken) throw IoError("request body already failed", true);
    d.ensureContinue();
    if (d.chunked) return d.readChunked(buf, n);
    size_t want = static_cast<size_t>(std::min<int64_t>(d.remaining, static_cast<int64_t>(n)));
    size_t k = d.recvSome(buf, want);
    d.remaining -= static_cast<int64_t>(k);
    d.consumed += static_cast<int64_t>(k);
    if (d.remaining == 0) d.done = true;
    return k;
}

int64_t BodyReader::length() const { return d_->chunked ? -1 : std::max<int64_t>(d_->req.contentLength, 0); }
bool BodyReader::hasBody() const { return d_->req.chunked || d_->req.contentLength > 0; }
bool BodyReader::finished() const { return d_->done; }
int64_t BodyReader::consumed() const { return d_->consumed; }
const Headers& BodyReader::trailers() const { return d_->trailers; }

std::string BodyReader::readAll(int64_t limit)
{
    // raw-body refuses a declared length past the limit before reading anything.
    if (limit >= 0 && !d_->chunked && d_->req.contentLength > limit) throw HttpError(413, kTooLarge);
    std::string out;
    if (!d_->chunked && d_->remaining > 0) out.reserve(static_cast<size_t>(d_->remaining));
    char tmp[kCopyBufferBytes];
    for (;;) {
        size_t k = read(tmp, sizeof tmp);
        if (k == 0) break;
        if (limit >= 0 && static_cast<int64_t>(out.size() + k) > limit) throw HttpError(413, kTooLarge);
        out.append(tmp, k);
    }
    return out;
}

int64_t BodyReader::copyTo(Sink& out, int64_t limit, const std::function<void(int64_t)>& progress)
{
    if (limit >= 0 && !d_->chunked && d_->req.contentLength > limit) throw HttpError(413, kTooLarge);
    std::string buf(kCopyBufferBytes, '\0');
    int64_t total = 0;
    for (;;) {
        size_t k = read(&buf[0], buf.size());
        if (k == 0) break;
        if (limit >= 0 && total + static_cast<int64_t>(k) > limit) throw HttpError(413, kTooLarge);
        out.write(buf.data(), k);
        total += static_cast<int64_t>(k);
        if (progress) progress(total);
    }
    return total;
}

int64_t BodyReader::drain(int64_t cap)
{
    Impl& d = *d_;
    if (d.done) return 0;
    if (d.sc->bodyBroken) return -1;
    if (d.req.expectContinue && !d.sc->continueSent) return -1;   // the client may still be waiting for 100
    if (!d.chunked && d.remaining > cap) return -1;
    char tmp[kCopyBufferBytes];
    int64_t total = 0;
    for (;;) {
        size_t k = read(tmp, sizeof tmp);
        if (k == 0) return total;
        total += static_cast<int64_t>(k);
        if (total > cap) return -1;
    }
}

// ---- Response ---------------------------------------------------------------------------------------------------

namespace {
class ResponseSink final : public Sink {
public:
    explicit ResponseSink(Response& r) : r_(r) {}
    using Sink::write;
    void write(const char* p, size_t n) override { r_.write(p, n); }
private:
    Response& r_;
};

bool badHeaderText(std::string_view s)
{
    for (char c : s)
        if (c == '\r' || c == '\n' || c == '\0') return true;
    return false;
}

std::string hexLen(uint64_t n)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do {
        b[--i] = "0123456789abcdef"[n & 15];
        n >>= 4;
    } while (n);
    return std::string(b + i);
}
} // namespace

struct Response::Impl {
    Impl(detail::Wire& w, const Request& r, Response& self) : wire(w), req(r), sinkObj(self) {}
    detail::Wire& wire;
    const Request& req;
    Headers headers;
    int status = 200;
    bool headersSent = false;
    bool finished = false;
    bool aborted = false;
    bool head = false;
    bool keepAllowed = true;                    // what the request allows (HTTP/1.1 unless close, 1.0 keep-alive)
    bool keepAlive = true;
    int keepAliveSecs = 5;
    const std::atomic<bool>* closing = nullptr; // the server is stopping: replies say Connection: close
    bool noBody = false;                        // HEAD, 1xx, 204, 304
    bool chunked = false;
    int64_t declared = -1;
    int64_t bodyBytes = 0;
    std::string pending;                        // the header block, sent with the first body bytes
    ResponseSink sinkObj;

    void flushPending()
    {
        if (pending.empty()) return;
        std::string p;
        p.swap(pending);
        wire.send(p.data(), p.size());
    }

    // pending + one piece in a single send when small (one TCP segment for typical JSON replies).
    void sendWithPending(const char* p, size_t n)
    {
        if (!pending.empty() && pending.size() + n <= 64 * 1024) {
            pending.append(p, n);
            flushPending();
            return;
        }
        flushPending();
        if (n) wire.send(p, n);
    }
};

Response::Response(detail::Wire& wire, const Request& req) : d_(std::make_unique<Impl>(wire, req, *this))
{
    d_->head = req.method == "HEAD";
    const std::string conn = req.headers.joined("Connection");
    d_->keepAllowed = req.httpMinor >= 1 ? !detail::hasToken(conn, "close") : detail::hasToken(conn, "keep-alive");
    d_->keepAlive = d_->keepAllowed;
    if (const detail::RequestScope* sc = detail::currentScope()) {
        d_->keepAliveSecs = sc->keepAliveSecs;
        d_->closing = sc->closing;
    }
}

Response::~Response() = default;

Headers& Response::headers() { return d_->headers; }
const Headers& Response::headers() const { return d_->headers; }
int Response::status() const { return d_->status; }
bool Response::headersSent() const { return d_->headersSent; }
bool Response::finished() const { return d_->finished; }
bool Response::aborted() const { return d_->aborted; }
bool Response::isHead() const { return d_->head; }
void Response::setKeepAlive(bool on) { d_->keepAlive = on && d_->keepAllowed; }
bool Response::keepAlive() const { return d_->keepAlive; }
int64_t Response::bodyBytes() const { return d_->bodyBytes; }
Sink& Response::sink() { return d_->sinkObj; }

void Response::begin(int status, int64_t contentLength)
{
    Impl& d = *d_;
    if (d.headersSent) throw std::logic_error("response headers already sent");
    if (status < 100 || status > 999) throw std::invalid_argument("invalid HTTP status " + std::to_string(status));
    for (const auto& f : d.headers.fields()) {
        // Node refuses these (ERR_INVALID_CHAR / ERR_INVALID_HTTP_TOKEN): no response splitting through a handler.
        bool badName = f.first.empty();
        for (char c : f.first) badName = badName || !detail::isTokenChar(static_cast<unsigned char>(c));
        if (badName || badHeaderText(f.second)) throw std::invalid_argument("invalid response header " + f.first);
    }
    d.status = status;
    const bool statusNoBody = status < 200 || status == 204 || status == 304;
    d.noBody = statusNoBody || d.head;
    if (const std::string* c = d.headers.find("Connection"); c && detail::hasToken(*c, "close")) d.keepAlive = false;
    if (d.closing && d.closing->load()) d.keepAlive = false;
    for (const char* managed : {"Content-Length", "Transfer-Encoding", "Connection", "Keep-Alive"})
        d.headers.remove(managed);

    bool chunked = false;
    if (contentLength < 0 && !d.noBody) {
        if (d.req.httpMinor >= 1) chunked = true;
        else d.keepAlive = false;               // HTTP/1.0: the body ends when the connection closes
    }

    std::string h;
    h.reserve(256);
    h += "HTTP/1.1 ";
    h += std::to_string(status);
    h += ' ';
    h += reasonPhrase(status);
    h += "\r\n";
    for (const auto& f : d.headers.fields()) {
        h += f.first;
        h += ": ";
        h += f.second;
        h += "\r\n";
    }
    if (!statusNoBody && contentLength >= 0) {
        h += "Content-Length: ";
        h += std::to_string(contentLength);
        h += "\r\n";
    }
    if (!d.headers.has("Date")) {
        h += "Date: ";
        h += httpDate(anfs::nowMs());
        h += "\r\n";
    }
    if (d.keepAlive) {
        h += "Connection: keep-alive\r\n";
        if (d.keepAliveSecs >= 0) h += "Keep-Alive: timeout=" + std::to_string(d.keepAliveSecs) + "\r\n";
    } else {
        h += "Connection: close\r\n";
    }
    if (chunked) h += "Transfer-Encoding: chunked\r\n";
    h += "\r\n";
    d.pending = std::move(h);
    d.chunked = chunked;
    d.declared = d.noBody ? -1 : contentLength;
    d.headersSent = true;
}

void Response::write(const char* p, size_t n)
{
    Impl& d = *d_;
    if (!d.headersSent) begin(d.status, -1);
    if (d.finished) throw std::logic_error("response already finished");
    if (d.noBody || n == 0) return;
    if (d.declared >= 0 && d.bodyBytes + static_cast<int64_t>(n) > d.declared)
        throw std::logic_error("response body longer than its Content-Length");
    if (d.chunked) {
        std::string frame = hexLen(n) + "\r\n";
        if (n <= 16 * 1024) {
            frame.append(p, n);
            frame += "\r\n";
            d.sendWithPending(frame.data(), frame.size());
        } else {
            d.sendWithPending(frame.data(), frame.size());
            d.wire.send(p, n);
            d.wire.send("\r\n", 2);
        }
    } else {
        d.sendWithPending(p, n);
    }
    d.bodyBytes += static_cast<int64_t>(n);
}

void Response::writeFile(void* fileHandle, int64_t offset, int64_t count)
{
    Impl& d = *d_;
    if (!d.headersSent) begin(d.status, -1);
    if (d.finished) throw std::logic_error("response already finished");
    if (d.noBody || count <= 0) return;
    if (d.declared >= 0 && d.bodyBytes + count > d.declared)
        throw std::logic_error("response body longer than its Content-Length");
    if (d.chunked) {
        std::string frame = hexLen(static_cast<uint64_t>(count)) + "\r\n";
        d.sendWithPending(frame.data(), frame.size());
    } else {
        d.flushPending();
    }
    if (!d.wire.sendFile(fileHandle, offset, count)) {
        // Copy fallback (test wires): positioned reads, so the caller's file pointer does not matter.
        std::string buf(kCopyBufferBytes, '\0');
        int64_t left = count, at = offset;
        while (left > 0) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(at) & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(at) >> 32);
            DWORD want = static_cast<DWORD>(std::min<int64_t>(left, static_cast<int64_t>(buf.size()))), got = 0;
            if (!ReadFile(static_cast<HANDLE>(fileHandle), &buf[0], want, &got, &ov) || got == 0)
                throw IoError("file read failed while sending (" + std::to_string(GetLastError()) + ")");
            d.wire.send(buf.data(), got);
            left -= got;
            at += got;
        }
    }
    if (d.chunked) d.wire.send("\r\n", 2);
    d.bodyBytes += count;
}

void Response::end()
{
    Impl& d = *d_;
    if (d.finished) return;
    if (!d.headersSent) begin(d.status, 0);
    if (d.chunked) d.sendWithPending("0\r\n\r\n", 5);
    else d.flushPending();
    // A body shorter than its Content-Length: the client is still waiting for bytes, so the connection cannot be
    // reused (the server closes it; Node raises ERR_HTTP_CONTENT_LENGTH_MISMATCH).
    if (d.declared >= 0 && d.bodyBytes < d.declared) d.keepAlive = false;
    d.finished = true;
}

void Response::abort()
{
    Impl& d = *d_;
    if (d.aborted) return;
    d.aborted = true;
    d.keepAlive = false;
    d.finished = true;
    d.pending.clear();
    d.wire.abort();
}

void Response::send(int status, std::string_view contentType, std::string_view body)
{
    Impl& d = *d_;
    if (d.headersSent) throw std::logic_error("response headers already sent");
    if (status == 204 || status == 304) {
        // Express res.send: no Content-Type, Content-Length, Transfer-Encoding or body for these.
        d.headers.remove("Content-Type");
        body = {};
    } else if (!contentType.empty()) {
        d.headers.set("Content-Type", std::string(contentType));
    }
    begin(status, static_cast<int64_t>(body.size()));
    if (!body.empty()) write(body.data(), body.size());
    end();
}

void Response::sendJson(int status, const JVal& v) { send(status, "application/json; charset=utf-8", json::stringify(v)); }

void Response::sendText(int status, std::string_view text, std::string_view contentType) { send(status, contentType, text); }

void Response::sendEmpty(int status)
{
    begin(status, 0);
    end();
}

void Response::redirect(std::string_view location, int status)
{
    if (badHeaderText(location)) throw std::invalid_argument("invalid redirect location");
    // Express 4 res.redirect with no Accept match picks the text form.
    setHeader("Location", std::string(location));
    send(status, "text/plain; charset=utf-8", std::string(reasonPhrase(status)) + ". Redirecting to " + std::string(location));
}

// ---- detail glue ------------------------------------------------------------------------------------------------

namespace detail {
static thread_local RequestScope* tlScope = nullptr;
ScopeGuard::ScopeGuard(RequestScope& s) : prev_(tlScope) { tlScope = &s; }
ScopeGuard::~ScopeGuard() { tlScope = prev_; }
RequestScope* currentScope() { return tlScope; }
void resetForError(Response& res)
{
    // Headers the handler set that begin() refuses (CR/LF/NUL, a bad name) would refuse the error reply too.
    std::vector<std::string> bad;
    for (const auto& f : res.headers().fields()) {
        bool b = f.first.empty() || badHeaderText(f.second);
        for (char c : f.first) b = b || !isTokenChar(static_cast<unsigned char>(c));
        if (b) bad.push_back(f.first);
    }
    for (const auto& n : bad) res.headers().remove(n);
    res.headers().remove("Content-Range");
    res.headers().remove("Content-Disposition");
    res.headers().remove("Content-Type");
    res.headers().remove("ETag");
    res.headers().remove("Last-Modified");
}
} // namespace detail

// ---- Ctx ------------------------------------------------------------------------------------------------------

std::string Ctx::param(std::string_view name) const
{
    for (const auto& p : params)
        if (iequals(p.first, name)) return p.second;
    return {};
}

void Ctx::error(int status, std::string_view text)
{
    res.send(status, "application/json; charset=utf-8", json::errorBody(text));
}

JVal Ctx::readJson(int64_t limit)
{
    // body-parser json(): no body or another media type leaves req.body = {} and the body unread.
    if (!body.hasBody() || !json::isJsonContentType(req.headers.get("Content-Type"))) return JVal::object();
    std::string text = body.readAll(limit);
    try {
        return json::parseBody(text);
    } catch (const JsonError&) {
        throw HttpError(400, std::string(json::kInvalidJsonBody));
    }
}

} // namespace anfs::http
