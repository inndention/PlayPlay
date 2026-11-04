// core/ids.cpp - digests, random ids, hex/base64 and constant-time compare on Windows CNG (ANFS-F1b).
//
// Algorithm providers are opened once per process and never closed (CNG algorithm handles are thread-safe for
// creating hash objects); each digest creates and destroys its own hash object. The random alphabet slices follow
// the C# Ids class (log_<ms>_<4 base36>, tx_<ms>_<5 base36>, share_<ms>) with the system RNG instead of
// System.Random, and the time part comes from anfs::clock() so ids are deterministic under a ManualClock.
#include "core/ids.h"

#include "core/clock.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace anfs {

namespace {

enum class Alg { Md5, Sha1, Sha256, HmacSha256 };

struct Providers {
    BCRYPT_ALG_HANDLE md5 = nullptr, sha1 = nullptr, sha256 = nullptr, hmac = nullptr;
    Providers()
    {
        BCryptOpenAlgorithmProvider(&md5, BCRYPT_MD5_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&sha1, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    }
};

BCRYPT_ALG_HANDLE provider(Alg a)
{
    static const Providers p;   // thread-safe initialisation; leaked on purpose (process lifetime)
    BCRYPT_ALG_HANDLE h = a == Alg::Md5 ? p.md5 : a == Alg::Sha1 ? p.sha1 : a == Alg::Sha256 ? p.sha256 : p.hmac;
    if (!h) throw std::runtime_error("CNG algorithm provider unavailable");
    return h;
}

ULONG digestLen(Alg a) { return a == Alg::Md5 ? 16 : a == Alg::Sha1 ? 20 : 32; }

// One hash object; data is fed in ULONG-sized pieces (BCryptHashData takes a 32-bit length).
class HashObj {
public:
    HashObj(Alg a, std::string_view key = {}) : len_(digestLen(a))
    {
        // An HMAC with an empty key is valid; CNG wants a non-null pointer for it.
        static UCHAR emptyKey = 0;
        PUCHAR k = nullptr;
        ULONG kn = 0;
        if (a == Alg::HmacSha256) {
            k = key.empty() ? &emptyKey : reinterpret_cast<PUCHAR>(const_cast<char*>(key.data()));
            if (key.size() > std::numeric_limits<ULONG>::max()) throw std::runtime_error("HMAC key too large");
            kn = static_cast<ULONG>(key.size());
        }
        if (!BCRYPT_SUCCESS(BCryptCreateHash(provider(a), &h_, nullptr, 0, k, kn, 0)))
            throw std::runtime_error("BCryptCreateHash failed");
    }
    ~HashObj()
    {
        if (h_) BCryptDestroyHash(h_);
    }
    HashObj(const HashObj&) = delete;
    HashObj& operator=(const HashObj&) = delete;

    void update(const void* p, size_t n)
    {
        auto* b = static_cast<const UCHAR*>(p);
        while (n > 0) {
            const ULONG piece = static_cast<ULONG>(std::min<size_t>(n, 1u << 30));
            if (!BCRYPT_SUCCESS(BCryptHashData(h_, const_cast<PUCHAR>(b), piece, 0)))
                throw std::runtime_error("BCryptHashData failed");
            b += piece;
            n -= piece;
        }
    }
    std::vector<uint8_t> finish()
    {
        std::vector<uint8_t> out(len_);
        if (!BCRYPT_SUCCESS(BCryptFinishHash(h_, out.data(), len_, 0))) throw std::runtime_error("BCryptFinishHash failed");
        return out;
    }

private:
    BCRYPT_HASH_HANDLE h_ = nullptr;
    ULONG len_;
};

std::vector<uint8_t> digest(Alg a, std::string_view data, std::string_view key = {})
{
    HashObj h(a, key);
    h.update(data.data(), data.size());
    return h.finish();
}

constexpr char kBase36[] = "0123456789abcdefghijklmnopqrstuvwxyz";
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int b64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

namespace ids {

std::string md5Hex(std::string_view data) { return hex::encode(digest(Alg::Md5, data)); }
std::string md5Hex12(std::string_view data) { return md5Hex(data).substr(0, 12); }
std::string sha1Hex(std::string_view data) { return hex::encode(digest(Alg::Sha1, data)); }
std::string sha256Hex(std::string_view data) { return hex::encode(digest(Alg::Sha256, data)); }
std::vector<uint8_t> sha256(std::string_view data) { return digest(Alg::Sha256, data); }
std::vector<uint8_t> hmacSha256(std::string_view key, std::string_view data) { return digest(Alg::HmacSha256, data, key); }

void randomBytes(void* out, size_t n)
{
    auto* b = static_cast<PUCHAR>(out);
    while (n > 0) {
        const ULONG piece = static_cast<ULONG>(std::min<size_t>(n, 1u << 30));
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b, piece, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            throw std::runtime_error("BCryptGenRandom failed");
        b += piece;
        n -= piece;
    }
}

std::string randomHex(size_t bytes)
{
    std::vector<uint8_t> b(bytes);
    if (bytes) randomBytes(b.data(), bytes);
    return hex::encode(b);
}

std::string randomBase36(size_t chars)
{
    // Rejection sampling: bytes >= 252 (= 7 * 36) are discarded, so every character is equally likely.
    std::string out;
    out.reserve(chars);
    uint8_t buf[64];
    while (out.size() < chars) {
        randomBytes(buf, sizeof buf);
        for (uint8_t v : buf) {
            if (v >= 252) continue;
            out.push_back(kBase36[v % 36]);
            if (out.size() == chars) break;
        }
    }
    return out;
}

std::string logId() { return prefixedId("log", 4); }
std::string txId() { return prefixedId("tx", 5); }
std::string shareId() { return "share_" + std::to_string(anfs::nowMs()); }

std::string uuid()
{
    uint8_t b[16];
    randomBytes(b, sizeof b);
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);   // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);   // RFC 4122 variant
    const std::string h = hex::encode(b, sizeof b);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::string prefixedId(std::string_view prefix, size_t randomChars)
{
    std::string out(prefix);
    out += '_';
    out += std::to_string(anfs::nowMs());
    if (randomChars) {
        out += '_';
        out += randomBase36(randomChars);
    }
    return out;
}

struct Hasher::Impl {
    explicit Impl(Alg a) : h(a) {}
    HashObj h;
    bool done = false;
};

Hasher::Hasher(Algo algo)
    : d_(std::make_unique<Impl>(algo == Algo::Md5 ? Alg::Md5 : algo == Algo::Sha1 ? Alg::Sha1 : Alg::Sha256))
{
}

Hasher::~Hasher() = default;

void Hasher::update(const void* p, size_t n)
{
    if (d_->done) throw std::logic_error("ids::Hasher used after finish()");
    d_->h.update(p, n);
}

std::vector<uint8_t> Hasher::finish()
{
    if (d_->done) throw std::logic_error("ids::Hasher finished twice");
    d_->done = true;
    return d_->h.finish();
}

std::string Hasher::finishHex() { return hex::encode(finish()); }

} // namespace ids

namespace hex {

std::string encode(const void* p, size_t n)
{
    static constexpr char digits[] = "0123456789abcdef";
    const auto* b = static_cast<const uint8_t*>(p);
    std::string out(n * 2, '\0');
    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 0x0F];
    }
    return out;
}

bool isHex(std::string_view s)
{
    if (s.empty() || (s.size() & 1)) return false;
    for (char c : s)
        if (hexNibble(c) < 0) return false;
    return true;
}

std::vector<uint8_t> decode(std::string_view s)
{
    if (!isHex(s)) return {};
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>((hexNibble(s[2 * i]) << 4) | hexNibble(s[2 * i + 1]));
    return out;
}

} // namespace hex

namespace b64 {

std::string encode(const void* p, size_t n)
{
    const auto* b = static_cast<const uint8_t*>(p);
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t(b[i]) << 16) | (uint32_t(b[i + 1]) << 8) | b[i + 2];
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = uint32_t(b[i]) << 16;
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (n - i == 2) {
        const uint32_t v = (uint32_t(b[i]) << 16) | (uint32_t(b[i + 1]) << 8);
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool decode(std::string_view s, std::vector<uint8_t>& out)
{
    // Convert.FromBase64String rules: whitespace ignored, length a multiple of 4, at most two '=' and only at the end.
    out.clear();
    std::string clean;
    clean.reserve(s.size());
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean += c;
    if (clean.size() % 4) return false;
    size_t pad = 0;
    while (pad < 2 && pad < clean.size() && clean[clean.size() - 1 - pad] == '=') ++pad;
    out.reserve(clean.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < clean.size() - pad; ++i) {
        const int v = b64Value(clean[i]);
        if (v < 0) {
            out.clear();
            return false;
        }
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

} // namespace b64

bool constantTimeEquals(std::string_view a, std::string_view b)
{
    // Both sides go through HMAC-SHA256 under a per-process random key first, so the final compare runs over 32
    // bytes whatever the lengths are, and no timing difference can be correlated with the secret's bytes.
    static const std::string key = [] {
        std::string k(32, '\0');
        ids::randomBytes(&k[0], k.size());
        return k;
    }();
    const std::vector<uint8_t> ha = ids::hmacSha256(key, a), hb = ids::hmacSha256(key, b);
    unsigned diff = 0;
    for (size_t i = 0; i < ha.size(); ++i) diff |= unsigned(ha[i] ^ hb[i]);
    return diff == 0 && ha.size() == hb.size();
}

} // namespace anfs
