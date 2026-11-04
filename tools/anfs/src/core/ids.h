// core/ids.h - identifiers, digests, encodings and constant-time compare. FROZEN (ANFS-F0); implemented by ANFS-F1b
// (core/ids.cpp, on Windows CNG/BCrypt: MD5, SHA-1, SHA-256, HMAC-SHA256, BCryptGenRandom).
//
// Id formats (mod-fscore.md "Ids", Node ids):
//   agentId  md5Hex12(hostname + "_" + ip)      PC1_192.168.1.20 -> cb0bff524238, SERVER_192.168.1.177 -> d013d23dcf03
//   log id   log_<epoch ms>_<4 base36>           ^log_\d{13}_[a-z0-9]{4}$
//   tx id    tx_<epoch ms>_<5 base36>
//   share id share_<epoch ms>
//   job id   uuid() (relay wire jobs, crypto.randomUUID form)
//   other    prefixedId("zip", 5) -> zip_<ms>_<5 base36>
// Time comes from anfs::clock() (core/clock.h), so ids are deterministic in their time part under a ManualClock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anfs {

namespace ids {
std::string md5Hex(std::string_view data);       // 32 lowercase hex
std::string md5Hex12(std::string_view data);     // first 12 hex chars
std::string sha1Hex(std::string_view data);      // 40 lowercase hex
std::string sha256Hex(std::string_view data);    // 64 lowercase hex
std::vector<uint8_t> sha256(std::string_view data);
std::vector<uint8_t> hmacSha256(std::string_view key, std::string_view data);

void randomBytes(void* out, size_t n);           // BCryptGenRandom (system RNG)
std::string randomHex(size_t bytes);
std::string randomBase36(size_t chars);          // [0-9a-z], like Math.random().toString(36) slices

std::string logId();
std::string txId();
std::string shareId();
std::string uuid();                              // RFC 4122 version 4, lowercase, 36 chars
std::string prefixedId(std::string_view prefix, size_t randomChars);

// Incremental digests for streamed files (wd-scan quickFileHash, thumbs sha256 key, swarm). Not thread-safe.
class Hasher {
public:
    enum class Algo { Md5, Sha1, Sha256 };
    explicit Hasher(Algo algo);
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    void update(const void* p, size_t n);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::vector<uint8_t> finish();               // the hasher cannot be reused afterwards
    std::string finishHex();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
} // namespace ids

namespace hex {
std::string encode(const void* p, size_t n);     // lowercase
inline std::string encode(const std::vector<uint8_t>& b) { return encode(b.data(), b.size()); }
bool isHex(std::string_view s);                  // non-empty, even length, [0-9a-fA-F]
std::vector<uint8_t> decode(std::string_view s); // empty on invalid input
// Upload ids: hex of the UTF-8 bytes of "<fileId>_<name>" (("f1","a b.txt") -> 66315f6120622e747874).
inline std::string ofText(std::string_view s) { return encode(s.data(), s.size()); }
} // namespace hex

namespace b64 {
std::string encode(const void* p, size_t n);     // standard alphabet, padded
inline std::string encode(std::string_view s) { return encode(s.data(), s.size()); }
inline std::string encode(const std::vector<uint8_t>& b) { return encode(b.data(), b.size()); }
bool decode(std::string_view s, std::vector<uint8_t>& out);   // false on invalid input (whitespace ignored)
} // namespace b64

// Length-independent-time equality for tokens and digests (the agent token check, HMAC compares).
bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace anfs
