// core/zipstream.cpp - streaming ZIP writer with zip64 (ANFS-F2b): the archiver / ZipArchive replacement behind
// /api/download-dir (pfs-stream), zip-send (syncjobs) and the peer package (pfs-setup). See core/zipstream.h.
//
// Entry layout (the sink is never seeked; APPNOTE 6.3.10):
// - Whole entries. When the data ends within the first 64 KB (directories, empty and small files: most of a tree) the
//   entry is read first and written in one piece: local header with the real CRC and sizes, the data, no data
//   descriptor, so a streaming reader (Java's ZipInputStream, libarchive on a pipe) needs nothing from the central
//   directory. Deflate that does not shrink such an entry is replaced by store, as Info-ZIP and archiver do.
// - Streamed entries. Longer data: local header with flag bit 3 (CRC and sizes 0), the data, then a data descriptor
//   with its signature. The descriptor has 8-byte sizes when the local header carries a zip64 extra field (APPNOTE
//   4.3.9.2) or when the sizes need them.
// - zip64 per entry. The local header gets a zip64 extra field (sizes 0xFFFFFFFF in the header, 0 in the field) when
//   the source announces 0xF0000000 bytes or more (3.75 GiB: headroom for deflate's stored-block overhead and a file
//   that grows while it is read; the C# StreamingZip threshold). A source of unknown length, or one that grows past
//   4 GiB after announcing less, gets no local field; its descriptor and central sizes switch to zip64 afterwards, as
//   Go's archive/zip, Java's ZipOutputStream and archiver (compress-commons) write it (Java's ZipInputStream picks the
//   descriptor form from the byte counts, which matches). The C# port threw "file grew past 4 GB" there instead.
// - Central directory: zip64 sizes when the local header had the field or a size is >= 0xFFFFFFFF, a zip64 offset when
//   the entry starts at or past 0xFFFFFFFF. The zip64 end record and locator follow when the entry count reaches 0xFFFF
//   or the directory's offset or size reaches 0xFFFFFFFF; the classic end record then holds 0xFFFF / 0xFFFFFFFF in all
//   three fields (Go and the C# port do the same), and those markers are what make .NET look for the locator.
// - Every header: UTF-8 name with flag bit 11 (invalid UTF-8 becomes U+FFFD; '\' becomes '/'; empty, '.', '..' and a
//   leading drive segment are dropped, so no name can leave the extraction folder), MS-DOS host, DOS date/time in the
//   local time of the mtime's own date (what .NET's LastWriteTime gives and Expand-Archive sets back; clamped to
//   1980..2107), plus an Info-ZIP extended timestamp (0x5455) with the UTC mtime in seconds, which 7-Zip, Info-ZIP and
//   bsdtar prefer. Directories carry external attribute 0x10 (FILE_ATTRIBUTE_DIRECTORY).
// - Errors. A source failure while the entry is still being read ahead (anything within its first 64 KB) writes
//   nothing, so the caller may skip that file and go on (the C# DownloadDir skipped unreadable files). A failure after
//   the header went out, and any sink failure, leaves a partial entry: the writer is then broken and every later call
//   throws IoError (peerGone/timeout kept from the first error), so a damaged archive is never finished; the caller
//   aborts the transfer. Misuse (any call after finish()) is std::logic_error; an empty or over-long name is
//   std::invalid_argument and writes nothing.
// - Output: headers and data are gathered in one 64 KB buffer and handed to the sink in 64 KB pieces; finish() writes
//   the rest and calls sink.flush(). bytesWritten() counts archive bytes produced, so it can run up to 64 KB ahead of
//   the sink until finish(). The central directory stays in memory (46 bytes + name + extras per entry). CRC-32 is an
//   own slice-by-8 table, so builds with and without zlib write identical CRCs.
#include "core/zipstream.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if ANFS_HAVE_ZLIB
#include <zlib.h>
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace anfs {

namespace {

constexpr uint32_t kMax32 = 0xFFFFFFFFu;
constexpr int64_t kZip64Hint = 0xF0000000LL;      // announced length that gets a zip64 local header (C# threshold)
constexpr size_t kBuf = kCopyBufferBytes;        // read-ahead, copy and output buffer (64 KB)
constexpr uint16_t kFlagDescriptor = 0x0008;     // bit 3: CRC and sizes follow the data
constexpr uint16_t kFlagUtf8 = 0x0800;           // bit 11: name is UTF-8
constexpr uint16_t kMethodStore = 0;
constexpr uint16_t kMethodDeflate = 8;
constexpr uint16_t kVersionBase = 20;            // 2.0: deflate, folders
constexpr uint16_t kVersionZip64 = 45;           // 4.5: zip64
constexpr uint16_t kTagZip64 = 0x0001;
constexpr uint16_t kTagTime = 0x5455;            // Info-ZIP extended timestamp ("UT")
constexpr uint32_t kAttrDirectory = 0x10;

// ---- CRC-32 (IEEE, reflected 0xEDB88320), slice-by-8 ----------------------------------------------------------------

struct CrcTables {
    uint32_t t[8][256];
    CrcTables()
    {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
    }
};

const CrcTables& crcTables()
{
    static const CrcTables tables;   // magic static: built once, thread-safe
    return tables;
}

uint32_t crc32Update(uint32_t crc, const char* data, size_t n)
{
    const auto& T = crcTables().t;
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    uint32_t c = ~crc;
    while (n >= 8) {                 // little-endian words (every Windows target)
        uint32_t a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + 4, 4);
        a ^= c;
        c = T[7][a & 0xFF] ^ T[6][(a >> 8) & 0xFF] ^ T[5][(a >> 16) & 0xFF] ^ T[4][a >> 24] ^
            T[3][b & 0xFF] ^ T[2][(b >> 8) & 0xFF] ^ T[1][(b >> 16) & 0xFF] ^ T[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = T[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---- little-endian fields -------------------------------------------------------------------------------------------

void le16(std::string& s, uint32_t v)
{
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void le32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void le64(std::string& s, uint64_t v)
{
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t clamp32(uint64_t v) { return v >= kMax32 ? kMax32 : static_cast<uint32_t>(v); }

// ---- entry names ----------------------------------------------------------------------------------------------------

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// A complete WTF-8 surrogate half at s[i] (ED A0..BF 80..BF, how fs::narrow keeps a lone surrogate of a file name):
// its UTF-16 unit, else 0.
uint32_t wtf8Surrogate(const unsigned char* s, size_t n, size_t i)
{
    if (i + 3 > n) return 0;
    if (s[i] != 0xED || s[i + 1] < 0xA0 || s[i + 1] > 0xBF || s[i + 2] < 0x80 || s[i + 2] > 0xBF) return 0;
    return 0xD000u | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
}

// Valid UTF-8 for the name field (bit 11 promises it). Invalid bytes become U+FFFD per maximal subpart (WHATWG, what
// Node's decoder does); a lone WTF-8 surrogate becomes one U+FFFD and a WTF-8 high+low pair the character it encodes,
// which is what Buffer.from(jsString) gives archiver for the same name.
std::string cleanUtf8(std::string_view in)
{
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    std::string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if (const uint32_t hi = wtf8Surrogate(s, n, i)) {
            const uint32_t lo = hi < 0xDC00 ? wtf8Surrogate(s, n, i + 3) : 0;
            if (lo >= 0xDC00) {
                appendUtf8(out, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
                i += 6;
            } else {
                appendUtf8(out, 0xFFFD);
                i += 3;
            }
            continue;
        }
        size_t need = 0;
        uint32_t cp = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0Fu; if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07u; if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F; }
        size_t k = 1;
        bool ok = need > 0;
        for (; ok && k <= need; ++k) {
            if (i + k >= n || s[i + k] < lo || s[i + k] > hi) { ok = false; break; }
            lo = 0x80;
            hi = 0xBF;
            cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if (ok) {
            out.append(in.data() + i, need + 1);
            i += need + 1;
        } else {
            appendUtf8(out, 0xFFFD);
            i += need > 0 ? k : 1;   // resume at the offending byte
        }
    }
    return out;
}

// The stored name: '/' separators, no leading '/', no empty / '.' / '..' segment and no leading drive ("C:"), so an
// extractor can never be steered outside its folder; directories end with '/'.
// Until the first segment is kept, every "X:" prefix goes (a drive segment "C:" and a drive-relative "d:x" -> "x"),
// also after a dropped segment: deciding on the first raw segment kept "./C:/x", "../C:/x" and "C:/D:/x" as an
// absolute "C:/x" / "D:/x". A later "x/C:/y" is relative and stays, as before.
std::string entryName(std::string_view raw, bool dir)
{
    const auto drivePrefix = [](std::string_view seg) {
        return seg.size() >= 2 && seg[1] == ':' && ((seg[0] >= 'A' && seg[0] <= 'Z') || (seg[0] >= 'a' && seg[0] <= 'z'));
    };
    const std::string s = cleanUtf8(raw);
    std::string out;
    out.reserve(s.size() + 1);
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find_first_of("/\\", i);
        if (j == std::string::npos) j = s.size();
        std::string_view seg(s.data() + i, j - i);
        if (out.empty())
            while (drivePrefix(seg)) seg.remove_prefix(2);
        if (!seg.empty() && seg != "." && seg != "..") {
            if (!out.empty()) out.push_back('/');
            out.append(seg);
        }
        i = j + 1;
    }
    if (out.empty()) throw std::invalid_argument("zip entry name is empty");
    if (dir) out.push_back('/');
    if (out.size() > 0xFFFF) throw std::invalid_argument("zip entry name is longer than 65535 bytes");
    return out;
}

} // namespace

// ---- writer ---------------------------------------------------------------------------------------------------------

struct ZipWriter::Impl {
    struct Entry {
        std::string name;
        uint16_t flags = kFlagUtf8;
        uint16_t method = kMethodStore;
        uint16_t dosTime = 0;
        uint16_t dosDate = 0;
        bool hasUt = false;
        uint32_t utSeconds = 0;
        uint32_t crc = 0;
        uint64_t csize = 0;
        uint64_t usize = 0;
        uint64_t offset = 0;
        bool zip64Local = false;
        bool dir = false;
    };

    Impl(Sink& s, int lvl) : out(s), level(std::min(lvl, 9)), obuf(new char[kBuf]), ibuf(new char[kBuf])
    {
        tzOk = GetDynamicTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID;
    }
    ~Impl()
    {
#if ANFS_HAVE_ZLIB
        if (zInit) deflateEnd(&z);
#endif
    }

    Sink& out;
    int level;                        // <= 0: everything stored
    std::unique_ptr<char[]> obuf;     // output gathering buffer
    size_t olen = 0;
    std::unique_ptr<char[]> ibuf;     // read-ahead / copy buffer
    std::string zbuf;                 // deflate output of a whole entry (deflateBound(64 KB))
    std::string cd;                   // central directory records
    uint64_t pos = 0;                 // archive bytes produced (handed to the sink or buffered)
    int count = 0;
    bool finished = false;
    bool broken = false;
    std::string brokenWhy;
    bool brokenPeerGone = false;
    bool brokenTimeout = false;
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    bool tzOk = false;
#if ANFS_HAVE_ZLIB
    z_stream z{};
    bool zInit = false;
#endif

    void checkUsable() const
    {
        if (finished) throw std::logic_error("ZipWriter: the archive is already finished");
        if (broken) throw IoError("zip archive is broken: " + brokenWhy, brokenPeerGone, brokenTimeout);
    }

    // Runs f, which writes to the archive; any exception marks the writer broken (a partial entry is out).
    template <class F>
    void writing(F&& f)
    {
        try {
            f();
        } catch (const IoError& e) {
            markBroken(e.what(), e.peerGone, e.timeout);
            throw;
        } catch (const std::exception& e) {
            markBroken(e.what(), false, false);
            throw;
        } catch (...) {
            markBroken("write failed", false, false);
            throw;
        }
    }
    void markBroken(const char* why, bool peerGone, bool timeout)
    {
        if (broken) return;
        broken = true;
        brokenWhy = why && *why ? why : "write failed";
        brokenPeerGone = peerGone;
        brokenTimeout = timeout;
    }

    void flushOut()
    {
        if (!olen) return;
        const size_t k = olen;
        olen = 0;
        out.write(obuf.get(), k);
    }
    void put(const char* p, size_t n)
    {
        pos += n;
        while (n) {
            const size_t k = std::min(n, kBuf - olen);
            std::memcpy(obuf.get() + olen, p, k);
            olen += k;
            p += k;
            n -= k;
            if (olen == kBuf) flushOut();
        }
    }
    void put(const std::string& s) { put(s.data(), s.size()); }

    // Fills ibuf until it is full or the source ends. Nothing is written, so a throw here leaves the archive intact.
    size_t readAhead(Source& src)
    {
        size_t n = 0;
        while (n < kBuf) {
            const size_t k = src.read(ibuf.get() + n, kBuf - n);
            if (!k) break;
            n += k;
        }
        return n;
    }

    // DOS date/time in local time (the mtime date's own DST rule, as .NET's DateTime.ToLocalTime), and the UT field.
    void stamp(Entry& e, int64_t mtimeMs) const
    {
        e.hasUt = mtimeMs >= 0 && mtimeMs / 1000 <= std::numeric_limits<int32_t>::max();
        e.utSeconds = e.hasUt ? static_cast<uint32_t>(mtimeMs / 1000) : 0;
        SYSTEMTIME utc{}, local{};
        bool ok = false, late = false;
        constexpr int64_t kEpochMs = 11644473600000LL;          // 1601-01-01 .. 1970-01-01
        if (mtimeMs >= 253402300800000LL) {                      // year 10000 on (FILETIME conversion range)
            late = true;
        } else if (mtimeMs > -kEpochMs) {
            ULARGE_INTEGER u;
            u.QuadPart = static_cast<uint64_t>(mtimeMs + kEpochMs) * 10000ULL;
            FILETIME ft;
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;
            if (FileTimeToSystemTime(&ft, &utc)) {
                ok = tzOk ? SystemTimeToTzSpecificLocalTimeEx(&tz, &utc, &local) != FALSE
                          : SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) != FALSE;
            }
            late = ok && local.wYear > 2107;
        }
        if (!late && (!ok || local.wYear < 1980)) {              // 1980-01-01 00:00:00 (C# StreamingZip clamp)
            e.dosDate = (1 << 5) | 1;
            e.dosTime = 0;
        } else if (late) {                                       // 2107-12-31 23:59:58
            e.dosDate = static_cast<uint16_t>((127 << 9) | (12 << 5) | 31);
            e.dosTime = static_cast<uint16_t>((23 << 11) | (59 << 5) | 29);
        } else {
            e.dosDate = static_cast<uint16_t>(((local.wYear - 1980) << 9) | (local.wMonth << 5) | local.wDay);
            e.dosTime = static_cast<uint16_t>((local.wHour << 11) | (local.wMinute << 5) | (local.wSecond / 2));
        }
    }

    void writeLocal(const Entry& e)
    {
        std::string h;
        h.reserve(30 + e.name.size() + 29);
        le32(h, 0x04034b50);
        le16(h, e.zip64Local ? kVersionZip64 : kVersionBase);
        le16(h, e.flags);
        le16(h, e.method);
        le16(h, e.dosTime);
        le16(h, e.dosDate);
        le32(h, e.crc);                                          // 0 with a descriptor
        le32(h, e.zip64Local ? kMax32 : static_cast<uint32_t>(e.csize));
        le32(h, e.zip64Local ? kMax32 : static_cast<uint32_t>(e.usize));
        le16(h, static_cast<uint32_t>(e.name.size()));
        le16(h, (e.zip64Local ? 20u : 0u) + (e.hasUt ? 9u : 0u));
        h += e.name;
        if (e.zip64Local) {                                      // sizes follow in the zip64 descriptor
            le16(h, kTagZip64);
            le16(h, 16);
            le64(h, 0);
            le64(h, 0);
        }
        if (e.hasUt) {
            le16(h, kTagTime);
            le16(h, 5);
            h.push_back(1);                                      // flags: mtime present
            le32(h, e.utSeconds);
        }
        put(h);
    }

    void writeDescriptor(const Entry& e)
    {
        std::string d;
        le32(d, 0x08074b50);
        le32(d, e.crc);
        if (e.zip64Local || e.csize >= kMax32 || e.usize >= kMax32) {
            le64(d, e.csize);
            le64(d, e.usize);
        } else {
            le32(d, static_cast<uint32_t>(e.csize));
            le32(d, static_cast<uint32_t>(e.usize));
        }
        put(d);
    }

    void addCentral(const Entry& e)
    {
        const bool sizes64 = e.zip64Local || e.csize >= kMax32 || e.usize >= kMax32;
        const bool off64 = e.offset >= kMax32;
        std::string extra;
        if (sizes64 || off64) {                                  // APPNOTE 4.5.3 field order: usize, csize, offset
            le16(extra, kTagZip64);
            le16(extra, (sizes64 ? 16u : 0u) + (off64 ? 8u : 0u));
            if (sizes64) {
                le64(extra, e.usize);
                le64(extra, e.csize);
            }
            if (off64) le64(extra, e.offset);
        }
        if (e.hasUt) {
            le16(extra, kTagTime);
            le16(extra, 5);
            extra.push_back(1);
            le32(extra, e.utSeconds);
        }
        const uint16_t version = (sizes64 || off64) ? kVersionZip64 : kVersionBase;
        le32(cd, 0x02014b50);
        le16(cd, version);                                       // made by: host 0 (MS-DOS / NTFS attributes)
        le16(cd, version);                                       // needed
        le16(cd, e.flags);
        le16(cd, e.method);
        le16(cd, e.dosTime);
        le16(cd, e.dosDate);
        le32(cd, e.crc);
        le32(cd, sizes64 ? kMax32 : static_cast<uint32_t>(e.csize));
        le32(cd, sizes64 ? kMax32 : static_cast<uint32_t>(e.usize));
        le16(cd, static_cast<uint32_t>(e.name.size()));
        le16(cd, static_cast<uint32_t>(extra.size()));
        le16(cd, 0);                                             // comment
        le16(cd, 0);                                             // disk number start
        le16(cd, 0);                                             // internal attributes
        le32(cd, e.dir ? kAttrDirectory : 0u);
        le32(cd, off64 ? kMax32 : static_cast<uint32_t>(e.offset));
        cd += e.name;
        cd += extra;
        ++count;
    }

#if ANFS_HAVE_ZLIB
    // Raw deflate (no zlib header), one stream per entry; the state is kept and reset between entries.
    void zStart()
    {
        if (zInit) {
            deflateReset(&z);
            return;
        }
        std::memset(&z, 0, sizeof z);
        if (deflateInit2(&z, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw std::bad_alloc();
        zInit = true;
        zbuf.resize(deflateBound(&z, static_cast<uLong>(kBuf)));
    }

    // Feeds z.next_in/avail_in (Z_NO_FLUSH: until consumed; Z_FINISH: until the stream ends), deflating straight into
    // the output buffer. Returns the compressed bytes produced.
    uint64_t zRun(int flush)
    {
        uint64_t made = 0;
        for (;;) {
            if (olen == kBuf) flushOut();
            const size_t room = kBuf - olen;
            z.next_out = reinterpret_cast<Bytef*>(obuf.get() + olen);
            z.avail_out = static_cast<uInt>(room);
            const int rc = deflate(&z, flush);
            if (rc == Z_STREAM_ERROR) throw IoError("zip: deflate failed");
            const size_t k = room - z.avail_out;
            olen += k;
            pos += k;
            made += k;
            if (flush == Z_FINISH ? rc == Z_STREAM_END : (z.avail_in == 0 && z.avail_out != 0)) return made;
        }
    }
#endif
};

ZipWriter::ZipWriter(Sink& out, int deflateLevel) : d_(std::make_unique<Impl>(out, deflateLevel)) {}

ZipWriter::~ZipWriter() = default;

void ZipWriter::addDirectory(std::string_view nameInZip, int64_t mtimeMs)
{
    d_->checkUsable();
    Impl::Entry e;
    e.name = entryName(nameInZip, true);
    e.dir = true;
    d_->stamp(e, mtimeMs);
    e.offset = d_->pos;
    d_->writing([&] {
        d_->writeLocal(e);
        d_->addCentral(e);
    });
}

void ZipWriter::addFile(std::string_view nameInZip, Source& data, int64_t mtimeMs, Method method)
{
    Impl& d = *d_;
    d.checkUsable();
    Impl::Entry e;
    e.name = entryName(nameInZip, false);
    d.stamp(e, mtimeMs);
    const bool useDeflate = method == Method::Deflate && d.level > 0 && deflateAvailable();
    size_t n = d.readAhead(data);
    e.offset = d.pos;

    if (n < kBuf) {                                              // whole entry: sizes and CRC in the local header
        e.crc = crc32Update(0, d.ibuf.get(), n);
        e.usize = n;
        e.csize = n;
        const char* body = d.ibuf.get();
#if ANFS_HAVE_ZLIB
        if (useDeflate && n > 0) {                               // nothing written yet: a failure here is harmless
            d.zStart();
            d.z.next_in = reinterpret_cast<Bytef*>(d.ibuf.get());
            d.z.avail_in = static_cast<uInt>(n);
            d.z.next_out = reinterpret_cast<Bytef*>(&d.zbuf[0]);
            d.z.avail_out = static_cast<uInt>(d.zbuf.size());
            if (deflate(&d.z, Z_FINISH) != Z_STREAM_END) throw std::runtime_error("zip: deflate failed");
            const size_t c = d.zbuf.size() - d.z.avail_out;
            if (c < n) {                                         // keep deflate only when it shrinks the data
                e.method = kMethodDeflate;
                e.csize = c;
                body = d.zbuf.data();
            }
        }
#endif
        d.writing([&] {
            d.writeLocal(e);
            d.put(body, static_cast<size_t>(e.csize));
            d.addCentral(e);
        });
        return;
    }

    // Streamed entry: data descriptor after the data.
    e.flags = kFlagUtf8 | kFlagDescriptor;
    e.method = useDeflate ? kMethodDeflate : kMethodStore;
    e.zip64Local = data.length() >= kZip64Hint;
#if ANFS_HAVE_ZLIB
    if (useDeflate) d.zStart();
#endif
    d.writing([&] {
        d.writeLocal(e);
        const uint64_t start = d.pos;
        uint32_t crc = 0;
        uint64_t usize = 0;
        while (n) {
            crc = crc32Update(crc, d.ibuf.get(), n);
            usize += n;
#if ANFS_HAVE_ZLIB
            if (useDeflate) {
                d.z.next_in = reinterpret_cast<Bytef*>(d.ibuf.get());
                d.z.avail_in = static_cast<uInt>(n);
                d.zRun(Z_NO_FLUSH);
            } else
#endif
            {
                d.put(d.ibuf.get(), n);
            }
            n = data.read(d.ibuf.get(), kBuf);
        }
#if ANFS_HAVE_ZLIB
        if (useDeflate) {
            d.z.next_in = nullptr;
            d.z.avail_in = 0;
            d.zRun(Z_FINISH);
        }
#endif
        e.crc = crc;
        e.usize = usize;
        e.csize = d.pos - start;
        d.writeDescriptor(e);
        d.addCentral(e);
    });
}

void ZipWriter::finish()
{
    Impl& d = *d_;
    d.checkUsable();
    d.writing([&] {
        const uint64_t cdStart = d.pos;
        d.put(d.cd);
        const uint64_t cdSize = d.pos - cdStart;
        const uint64_t n = static_cast<uint64_t>(d.count);
        const bool z64 = n >= 0xFFFF || cdStart >= kMax32 || cdSize >= kMax32;
        std::string t;
        if (z64) {
            const uint64_t recordAt = d.pos;
            le32(t, 0x06064b50);                                 // zip64 end of central directory record
            le64(t, 44);                                         // size of the rest of the record
            le16(t, kVersionZip64);
            le16(t, kVersionZip64);
            le32(t, 0);                                          // this disk
            le32(t, 0);                                          // disk with the central directory
            le64(t, n);
            le64(t, n);
            le64(t, cdSize);
            le64(t, cdStart);
            le32(t, 0x07064b50);                                 // zip64 end of central directory locator
            le32(t, 0);
            le64(t, recordAt);
            le32(t, 1);                                          // total disks
        }
        le32(t, 0x06054b50);
        le16(t, 0);
        le16(t, 0);
        le16(t, z64 ? 0xFFFFu : static_cast<uint32_t>(n));
        le16(t, z64 ? 0xFFFFu : static_cast<uint32_t>(n));
        le32(t, z64 ? kMax32 : clamp32(cdSize));
        le32(t, z64 ? kMax32 : clamp32(cdStart));
        le16(t, 0);                                              // comment length
        d.put(t);
        d.flushOut();
        d.out.flush();
    });
    d.finished = true;
    std::string().swap(d.cd);
}

int64_t ZipWriter::bytesWritten() const { return static_cast<int64_t>(d_->pos); }

int ZipWriter::entries() const { return d_->count; }

bool ZipWriter::deflateAvailable() { return ANFS_HAVE_ZLIB != 0; }

} // namespace anfs
