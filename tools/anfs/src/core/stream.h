// core/stream.h - byte sources and sinks shared by the HTTP server (request bodies, responses), the WinHTTP client,
// the relay pipes, files and the ZIP writer. FROZEN (ANFS-F0): change only through the AN-FS coordinator.
//
// Rules (CONVENTIONS.md section 2): streams are never buffered whole; copies use 64 KB buffers; a client abort ends a
// transfer with IoError{peerGone=true}, which handlers let propagate (the server closes the connection silently).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace anfs {

constexpr size_t kCopyBufferBytes = 64 * 1024;

// Transport or file failure while reading or writing a stream. peerGone: the other side closed or reset (client
// abort, peer disconnect); timeout: an inactivity timeout expired. Neither is logged as an error by the server.
class IoError : public std::runtime_error {
public:
    explicit IoError(const std::string& what, bool peerGone = false, bool timeout = false)
        : std::runtime_error(what), peerGone(peerGone), timeout(timeout) {}
    bool peerGone = false;
    bool timeout = false;
};

// A copy or a buffered read went past its byte limit. The HTTP layer maps it to 413 (CONVENTIONS D4).
class TooLarge : public IoError {
public:
    explicit TooLarge(int64_t limit) : IoError("request body too large"), limit(limit) {}
    int64_t limit = 0;
};

// Pull source. read() returns 1..n bytes, or 0 at the end of the stream; it blocks until data arrives and throws
// IoError on failure. length() is the total length when known up front (Content-Length, file size), else -1.
class Source {
public:
    virtual ~Source() = default;
    virtual size_t read(char* buf, size_t n) = 0;
    virtual int64_t length() const { return -1; }
};

// Push sink. write() blocks until the bytes are accepted (natural back-pressure) and throws IoError on failure.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const char* p, size_t n) = 0;
    virtual void flush() {}
    void write(std::string_view s) { write(s.data(), s.size()); }
};

// In-memory source over a string the caller keeps alive (or owns via the second constructor). Not copyable or
// movable: view_ may point into own_ (a copy, or a move of a short string in the small-string buffer, would leave
// it pointing at the old object); hold it by reference, unique_ptr or make_shared.
class StringSource final : public Source {
public:
    explicit StringSource(std::string_view s) : view_(s) {}
    explicit StringSource(std::string&& s) : own_(std::move(s)), view_(own_) {}
    explicit StringSource(const char* s) : view_(s ? s : "") {}   // else "lit" is ambiguous (string_view vs string&&)
    StringSource(const StringSource&) = delete;
    StringSource& operator=(const StringSource&) = delete;
    size_t read(char* buf, size_t n) override
    {
        size_t k = std::min(n, view_.size() - pos_);
        if (k) std::memcpy(buf, view_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    int64_t length() const override { return static_cast<int64_t>(view_.size()); }
private:
    std::string own_;
    std::string_view view_;
    size_t pos_ = 0;
};

// In-memory sink, optionally capped (TooLarge past maxBytes; -1 = no cap).
class StringSink final : public Sink {
public:
    explicit StringSink(int64_t maxBytes = -1) : max_(maxBytes) {}
    using Sink::write;
    void write(const char* p, size_t n) override
    {
        if (max_ >= 0 && static_cast<int64_t>(data.size() + n) > max_) throw TooLarge(max_);
        data.append(p, n);
    }
    std::string data;
private:
    int64_t max_;
};

// Discards everything (drains a body, counts bytes).
class NullSink final : public Sink {
public:
    using Sink::write;
    void write(const char*, size_t n) override { bytes += static_cast<int64_t>(n); }
    int64_t bytes = 0;
};

// Wraps a source: counts bytes, reports progress and stops on a cancel flag (srv-xfer CountingStream).
class CountingSource final : public Source {
public:
    CountingSource(Source& inner, std::function<void(int64_t)> progress = {}, std::function<bool()> cancelled = {})
        : inner_(inner), progress_(std::move(progress)), cancelled_(std::move(cancelled)) {}
    size_t read(char* buf, size_t n) override
    {
        if (cancelled_ && cancelled_()) throw IoError("cancelled", true);
        size_t k = inner_.read(buf, n);
        total_ += static_cast<int64_t>(k);
        if (k && progress_) progress_(total_);
        return k;
    }
    int64_t length() const override { return inner_.length(); }
    int64_t total() const { return total_; }
private:
    Source& inner_;
    std::function<void(int64_t)> progress_;
    std::function<bool()> cancelled_;
    int64_t total_ = 0;
};

// Copies src into dst with a 64 KB buffer. maxBytes < 0 means no limit; past the limit it throws TooLarge after
// writing at most maxBytes. progress(total) runs after each chunk. Returns the bytes copied.
inline int64_t copyStream(Source& src, Sink& dst, int64_t maxBytes = -1,
                          const std::function<void(int64_t)>& progress = {})
{
    std::string buf(kCopyBufferBytes, '\0');
    int64_t total = 0;
    for (;;) {
        size_t k = src.read(&buf[0], buf.size());
        if (k == 0) break;
        if (maxBytes >= 0 && total + static_cast<int64_t>(k) > maxBytes) throw TooLarge(maxBytes);
        dst.write(buf.data(), k);
        total += static_cast<int64_t>(k);
        if (progress) progress(total);
    }
    return total;
}

} // namespace anfs
