// test_anfs_core_fs - Node path.win32 (core/nodepath), the Win32 file-system helpers (core/fsutil) and the content
// type tables (core/mime), ANFS-F2a.
//
// The literal path vectors were produced by the local Node 22.13.1 (path.win32), the mime vectors by mime-types
// 2.1.35 / mime 1.6.0. Three live oracles repeat the comparisons and are skipped when the tool is missing:
// - node.exe: ~3,000 random paths through every path function, readdirSync order, and both mime tables entry by entry
//   (needs a local node_modules with mime-types 2.1.35 + mime-db 1.52.0 + mime 1.6.0; ANFS_MIME_NODE_MODULES or
//   C:/libs/services/broadcast-playout/node_modules);
// - powershell.exe 5.1 (.NET Framework, the C# port's runtime): ToLowerInvariant over the whole BMP and the elevation
//   rule (IsSystem || IsInRole(Administrator));
// - cmd.exe mklink /J for the junction tests (no privilege needed); a file symlink only when the account may create one.
//
// The streaming ZIP writer (core/zipstream, ANFS-F2b) is checked by a strict in-test parser (every header, descriptor
// and zip64 record against the writer's layout rules, data inflated with zlib and CRCs from zlib's crc32, not the
// writer's own table) and read back by the real consumers: Expand-Archive in PowerShell 7 (.NET 9) and Windows
// PowerShell 5.1 (.NET Framework), .NET's ZipFile listing, and bsdtar (C:\Windows\System32\tar.exe, which verifies
// CRCs). zipZip64Over4GiB writes a stored entry of 4 GiB + 1 MiB from a sparse file (and one from a source of unknown
// length) and extracts it; it needs ANFS_HEAVY_TESTS=1, about 9 GB free next to the test executable (or in
// ANFS_HEAVY_DIR) and is run under the machine-wide build/test lock.
#include "core/fsutil.h"
#include "core/json.h"
#include "core/mime.h"
#include "core/nodepath.h"
#include "core/zipstream.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>
#include <QtCore/QStorageInfo>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <winioctl.h>

#if ANFS_HAVE_ZLIB
#include <zlib.h>
#endif

using namespace anfs;

namespace {

QByteArray qb(const std::string& s) { return QByteArray(s.data(), static_cast<qsizetype>(s.size())); }
std::string u8(const QString& s) { return s.toStdString(); }
QString qs(const std::string& s) { return QString::fromStdString(s); }

// QByteArray comparison prints both sides escaped when it fails.
#define SEQ(actual, expected) QCOMPARE(qb(actual), qb(std::string(expected)))

QString findNode()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (p.isEmpty() && QFileInfo::exists(QStringLiteral("C:/Program Files/nodejs/node.exe")))
        p = QStringLiteral("C:/Program Files/nodejs/node.exe");
    return p;
}

QString findPowerShell51()
{
    const QString p = QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe");
    return QFileInfo::exists(p) ? p : QString();
}

bool runTool(const QString& exe, const QStringList& args, QByteArray* out = nullptr, int timeoutMs = 60000)
{
    QProcess p;
    p.start(exe, args);
    if (!p.waitForStarted(10000)) return false;
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        return false;
    }
    if (out) *out = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        qWarning("%s failed (%d): %s", qPrintable(QFileInfo(exe).fileName()), p.exitCode(),
                 p.readAllStandardError().left(2000).constData());
        return false;
    }
    return true;
}

bool writeBytes(const std::string& path, const std::string& data)
{
    QFile f(qs(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data.data(), static_cast<qint64>(data.size())) == static_cast<qint64>(data.size());
}

std::string readBytes(const std::string& path)
{
    QFile f(qs(path));
    if (!f.open(QIODevice::ReadOnly)) return "<unreadable>";
    return f.readAll().toStdString();
}

// The raw FindFirstFileW order of a directory, "." and ".." skipped: the reference listDir must equal.
std::vector<std::string> rawListing(const std::string& dir)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((fs::widen(dir) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        const std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") names.push_back(fs::narrow(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}

// Raw existence through the \\?\ form (independent of fsutil's own long-path handling).
bool rawExists(const std::string& path)
{
    const std::wstring w = L"\\\\?\\" + fs::widen(path);
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool makeJunction(const std::string& link, const std::string& to)
{
    return runTool(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                               QDir::toNativeSeparators(qs(link)), QDir::toNativeSeparators(qs(to))});
}

// Entries made through \\?\, as Node (or any \\?\ caller) makes them: names Win32 would trim ('report ', 'a.') or map
// ('nul') are kept.
bool makeExactDir(const std::string& path)
{
    return CreateDirectoryW((L"\\\\?\\" + fs::widen(path)).c_str(), nullptr) != FALSE;
}

bool makeExactFile(const std::string& path)
{
    HANDLE h = CreateFileW((L"\\\\?\\" + fs::widen(path)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(h, "x", 1, &put, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

// makeDirs on its own thread, at most 5 s: a hang (the loop the ANFS-F2a review found) fails the test instead of
// blocking the run. A thread that never returns is left detached; the process ends it.
bool makeDirsWithin(const std::string& path, std::string* err, bool* finished)
{
    using Result = std::pair<bool, std::string>;
    auto task = std::make_shared<std::packaged_task<Result()>>([path] {
        std::string e;
        const bool ok = fs::makeDirs(path, &e);
        return Result(ok, e);
    });
    std::future<Result> f = task->get_future();
    std::thread([task] { (*task)(); }).detach();
    *finished = f.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!*finished) return false;
    const Result r = f.get();
    if (err) *err = r.second;
    return r.first;
}

std::string freeDriveLetter()
{
    const DWORD mask = GetLogicalDrives();
    for (int i = 25; i >= 3; --i)
        if (!(mask & (1u << i))) return std::string(1, static_cast<char>('A' + i)) + ":";
    return {};
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
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

// The oracle scripts are defined at the end of the file (moc loses track of raw strings with quotes).
const char* nodePathJs();
const char* nodeMimeJs();

} // namespace

// ---- ZIP helpers (ANFS-F2b) -------------------------------------------------------------------------------------------
namespace {

constexpr uint64_t kM32 = 0xFFFFFFFFull;

// CRC-32 from zlib (an implementation independent of the writer's own slice-by-8 table); bitwise without zlib.
uint32_t refCrc(const char* p, size_t n, uint32_t crc = 0)
{
#if ANFS_HAVE_ZLIB
    while (n) {
        const uInt k = static_cast<uInt>(std::min<size_t>(n, size_t(1) << 30));
        crc = static_cast<uint32_t>(::crc32(crc, reinterpret_cast<const Bytef*>(p), k));
        p += k;
        n -= k;
    }
    return crc;
#else
    crc = ~crc;
    while (n--) {
        crc ^= static_cast<unsigned char>(*p++);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
#endif
}
uint32_t refCrc(const std::string& s) { return refCrc(s.data(), s.size()); }

bool inflateRaw(const std::string& in, std::string& out)
{
#if ANFS_HAVE_ZLIB
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    std::string buf(65536, '\0');
    int rc;
    do {
        z.next_out = reinterpret_cast<Bytef*>(&buf[0]);
        z.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&z, Z_NO_FLUSH);
        out.append(buf.data(), buf.size() - z.avail_out);
    } while (rc == Z_OK);
    const bool ok = rc == Z_STREAM_END && z.avail_in == 0;
    inflateEnd(&z);
    return ok;
#else
    (void)in;
    (void)out;
    return false;
#endif
}

uint64_t le(const std::string& s, size_t at, int bytes)
{
    uint64_t v = 0;
    if (at + static_cast<size_t>(bytes) > s.size()) return ~0ull;   // never a valid field value in these archives
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(s[at + static_cast<size_t>(i)]);
    return v;
}
void put16(std::string& s, uint32_t v) { for (int i = 0; i < 2; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }

// One entry as its central record, local header and descriptor describe it.
struct ZipRec {
    std::string name;
    uint16_t madeBy = 0, needed = 0, flags = 0, method = 0, dosTime = 0, dosDate = 0;
    uint32_t crc = 0, extAttr = 0;
    uint64_t csize = 0, usize = 0, offset = 0;
    bool z64Sizes = false, z64Offset = false;    // the central zip64 field carried the sizes / the offset
    bool utCentral = false, utLocal = false;
    uint32_t utTime = 0, utLocalTime = 0;
    uint16_t lNeeded = 0;
    bool lZip64 = false;                         // zip64 extra field in the local header
    uint64_t dataAt = 0;
    bool descriptor = false, descriptor64 = false;
    uint64_t end = 0;                            // just past the data (and descriptor)
};

struct ZipInfo {
    std::vector<ZipRec> recs;
    bool zip64End = false;
    uint64_t cdStart = 0, cdSize = 0;
    const ZipRec* find(const std::string& name) const
    {
        for (const ZipRec& r : recs)
            if (r.name == name) return &r;
        return nullptr;
    }
};

using ReadAt = std::function<std::string(uint64_t offset, size_t n)>;   // n bytes, fewer only past the end

ReadAt memReader(const std::string& s)
{
    return [&s](uint64_t off, size_t n) { return off >= s.size() ? std::string() : s.substr(static_cast<size_t>(off), n); };
}

// Reads an archive file at any offset (also past 4 GiB).
class FileAt {
public:
    explicit FileAt(const std::string& path)
    {
        h_ = CreateFileW(fs::widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER sz{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &sz)) size_ = static_cast<uint64_t>(sz.QuadPart);
    }
    ~FileAt() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    FileAt(const FileAt&) = delete;
    FileAt& operator=(const FileAt&) = delete;
    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t size() const { return size_; }
    std::string read(uint64_t off, size_t n) const
    {
        std::string out(n, '\0');
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, &out[0], static_cast<DWORD>(n), &got, &ov)) got = 0;
        out.resize(got);
        return out;
    }
    ReadAt reader() const { return [this](uint64_t off, size_t n) { return read(off, n); }; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

// Parses an archive strictly and checks the writer's layout rules (core/zipstream.cpp file comment). Returns "" or the
// first problem.
std::string parseZip(const ReadAt& rd, uint64_t total, ZipInfo& z)
{
    if (total < 22) return "shorter than an end record";
    const std::string eocd = rd(total - 22, 22);
    if (le(eocd, 0, 4) != 0x06054b50) return "no end record at the very end";
    if (le(eocd, 4, 2) != 0 || le(eocd, 6, 2) != 0 || le(eocd, 20, 2) != 0) return "end record: disk or comment fields";
    uint64_t n = le(eocd, 8, 2);
    if (le(eocd, 10, 2) != n) return "end record: entry counts differ";
    uint64_t cdSize = le(eocd, 12, 4), cdStart = le(eocd, 16, 4);
    uint64_t endAt = total - 22;
    if (n == 0xFFFF || cdSize == kM32 || cdStart == kM32) {
        if (n != 0xFFFF || cdSize != kM32 || cdStart != kM32) return "end record: zip64 markers not in all three fields";
        if (total < 22 + 20 + 56) return "zip64: too short";
        const std::string loc = rd(total - 42, 20);
        if (le(loc, 0, 4) != 0x07064b50 || le(loc, 4, 4) != 0 || le(loc, 16, 4) != 1) return "zip64 locator";
        const uint64_t recAt = le(loc, 8, 8);
        if (recAt + 56 != total - 42) return "zip64 record is not right before its locator";
        const std::string r = rd(recAt, 56);
        if (le(r, 0, 4) != 0x06064b50 || le(r, 4, 8) != 44 || le(r, 12, 2) != 45 || le(r, 14, 2) != 45 ||
            le(r, 16, 4) != 0 || le(r, 20, 4) != 0)
            return "zip64 record header";
        n = le(r, 24, 8);
        if (le(r, 32, 8) != n) return "zip64 record: entry counts differ";
        cdSize = le(r, 40, 8);
        cdStart = le(r, 48, 8);
        endAt = recAt;
        z.zip64End = true;
        if (n < 0xFFFF && cdSize < kM32 && cdStart < kM32) return "zip64 end record although nothing overflowed";
    } else if (n >= 0xFFFF || cdStart >= kM32 || cdSize >= kM32) {
        return "an overflowing end record without zip64";
    }
    if (cdStart + cdSize != endAt) return "the central directory does not end at the end record";
    z.cdStart = cdStart;
    z.cdSize = cdSize;
    const std::string cd = rd(cdStart, static_cast<size_t>(cdSize));
    if (cd.size() != cdSize) return "central directory cut off";
    size_t p = 0;
    for (uint64_t i = 0; i < n; ++i) {
        const std::string at = "central record " + std::to_string(i) + ": ";
        if (le(cd, p, 4) != 0x02014b50) return at + "signature";
        ZipRec r;
        r.madeBy = static_cast<uint16_t>(le(cd, p + 4, 2));
        r.needed = static_cast<uint16_t>(le(cd, p + 6, 2));
        r.flags = static_cast<uint16_t>(le(cd, p + 8, 2));
        r.method = static_cast<uint16_t>(le(cd, p + 10, 2));
        r.dosTime = static_cast<uint16_t>(le(cd, p + 12, 2));
        r.dosDate = static_cast<uint16_t>(le(cd, p + 14, 2));
        r.crc = static_cast<uint32_t>(le(cd, p + 16, 4));
        r.csize = le(cd, p + 20, 4);
        r.usize = le(cd, p + 24, 4);
        const size_t nl = static_cast<size_t>(le(cd, p + 28, 2)), el = static_cast<size_t>(le(cd, p + 30, 2));
        if (le(cd, p + 32, 2) != 0 || le(cd, p + 34, 2) != 0 || le(cd, p + 36, 2) != 0) return at + "comment/disk/internal";
        r.extAttr = static_cast<uint32_t>(le(cd, p + 38, 4));
        r.offset = le(cd, p + 42, 4);
        if (p + 46 + nl + el > cd.size()) return at + "overruns the directory";
        r.name = cd.substr(p + 46, nl);
        const std::string ex = cd.substr(p + 46 + nl, el);
        const bool uMark = r.usize == kM32, cMark = r.csize == kM32;
        if (uMark != cMark) return at + "only one size marked zip64";
        for (size_t q = 0; q < ex.size();) {
            const uint64_t tag = le(ex, q, 2), len = le(ex, q + 2, 2);
            if (q + 4 + len > ex.size()) return at + "extra field overruns";
            size_t f = q + 4;
            if (tag == 1) {
                if (uMark) { r.usize = le(ex, f, 8); f += 8; r.z64Sizes = true; }
                if (cMark) { r.csize = le(ex, f, 8); f += 8; }
                if (r.offset == kM32) { r.offset = le(ex, f, 8); f += 8; r.z64Offset = true; }
                if (f != q + 4 + len) return at + "zip64 field length";
            } else if (tag == 0x5455) {
                if (len != 5 || ex[f] != 1) return at + "UT field";
                r.utCentral = true;
                r.utTime = static_cast<uint32_t>(le(ex, f + 1, 4));
            } else {
                return at + "unexpected extra field " + std::to_string(tag);
            }
            q += 4 + static_cast<size_t>(len);
        }
        if (r.usize == kM32 || r.offset == kM32) return at + "zip64 marker without a value";
        p += 46 + nl + el;
        z.recs.push_back(std::move(r));
    }
    if (p != cd.size()) return "trailing bytes in the central directory";

    uint64_t expectAt = 0;
    for (ZipRec& r : z.recs) {
        const std::string at = "entry '" + r.name + "': ";
        if (r.offset != expectAt) return at + "does not start where the previous entry ended";
        const std::string lh = rd(r.offset, 30);
        if (le(lh, 0, 4) != 0x04034b50) return at + "local signature";
        r.lNeeded = static_cast<uint16_t>(le(lh, 4, 2));
        if (le(lh, 6, 2) != r.flags || le(lh, 8, 2) != r.method) return at + "local flags or method differ";
        if (le(lh, 10, 2) != r.dosTime || le(lh, 12, 2) != r.dosDate) return at + "local time differs";
        const uint64_t lCrc = le(lh, 14, 4), lC = le(lh, 18, 4), lU = le(lh, 22, 4);
        const size_t nl = static_cast<size_t>(le(lh, 26, 2)), el = static_cast<size_t>(le(lh, 28, 2));
        const std::string rest = rd(r.offset + 30, nl + el);
        if (rest.size() != nl + el || rest.compare(0, nl, r.name) != 0) return at + "local name differs";
        const std::string ex = rest.substr(nl);
        for (size_t q = 0; q < ex.size();) {
            const uint64_t tag = le(ex, q, 2), len = le(ex, q + 2, 2);
            if (q + 4 + len > ex.size()) return at + "local extra overruns";
            if (tag == 1) {
                if (len != 16 || le(ex, q + 4, 8) != 0 || le(ex, q + 12, 8) != 0) return at + "local zip64 field";
                r.lZip64 = true;
            } else if (tag == 0x5455) {
                if (len != 5 || ex[q + 4] != 1) return at + "local UT field";
                r.utLocal = true;
                r.utLocalTime = static_cast<uint32_t>(le(ex, q + 5, 4));
            } else {
                return at + "unexpected local extra field " + std::to_string(tag);
            }
            q += 4 + static_cast<size_t>(len);
        }
        r.dataAt = r.offset + 30 + nl + el;
        if (r.flags & 0x0008) {
            r.descriptor = true;
            if (lCrc != 0) return at + "local CRC is not 0 although a descriptor follows";
            if (r.lZip64 ? (lC != kM32 || lU != kM32) : (lC != 0 || lU != 0)) return at + "local sizes with a descriptor";
            r.descriptor64 = r.lZip64 || r.csize >= kM32 || r.usize >= kM32;
            const size_t dl = r.descriptor64 ? 24 : 16;
            const std::string dd = rd(r.dataAt + r.csize, dl);
            if (le(dd, 0, 4) != 0x08074b50) return at + "descriptor signature";
            if (le(dd, 4, 4) != r.crc) return at + "descriptor CRC differs";
            const uint64_t dc = r.descriptor64 ? le(dd, 8, 8) : le(dd, 8, 4);
            const uint64_t du = r.descriptor64 ? le(dd, 16, 8) : le(dd, 12, 4);
            if (dc != r.csize || du != r.usize) return at + "descriptor sizes differ";
            r.end = r.dataAt + r.csize + dl;
        } else {
            if (r.lZip64) return at + "zip64 local field without a descriptor";
            if (lCrc != r.crc || lC != r.csize || lU != r.usize) return at + "local CRC or sizes differ";
            r.end = r.dataAt + r.csize;
        }
        const bool sizes64 = r.lZip64 || r.csize >= kM32 || r.usize >= kM32;
        if (r.z64Sizes != sizes64) return at + "central zip64 sizes rule";
        if (r.z64Offset != (r.offset >= kM32)) return at + "central zip64 offset rule";
        const uint16_t ver = (sizes64 || r.z64Offset) ? 45 : 20;
        if (r.needed != ver || r.madeBy != ver) return at + "central versions";
        if (r.lNeeded != (r.lZip64 ? 45 : 20)) return at + "local version";
        if ((r.flags & ~0x0808) != 0 || !(r.flags & 0x0800)) return at + "flags";
        if (r.method != 0 && r.method != 8) return at + "method";
        if (r.method == 0 && r.csize != r.usize) return at + "stored sizes differ";
        const bool dir = !r.name.empty() && r.name.back() == '/';
        if (r.extAttr != (dir ? 0x10u : 0u)) return at + "external attributes";
        if (dir && (r.csize || r.usize || r.method || r.descriptor || r.crc)) return at + "directory with data";
        if (r.utCentral != r.utLocal || r.utTime != r.utLocalTime) return at + "local and central UT differ";
        expectAt = r.end;
    }
    if (expectAt != z.cdStart) return "the central directory does not follow the last entry";
    return {};
}

// The entry's data, inflated, with its size and CRC checked; "" plus *err on a problem.
std::string zipContent(const std::string& zip, const ZipRec& r, std::string* err)
{
    std::string raw = zip.substr(static_cast<size_t>(r.dataAt), static_cast<size_t>(r.csize)), out;
    if (r.method == 8) {
        if (!inflateRaw(raw, out)) { *err = r.name + ": does not inflate"; return {}; }
    } else {
        out.swap(raw);
    }
    if (out.size() != r.usize) { *err = r.name + ": size differs"; return {}; }
    if (refCrc(out) != r.crc) { *err = r.name + ": CRC differs"; return {}; }
    err->clear();
    return out;
}

// DOS date and time of an epoch-ms instant in local time, computed through Qt (independent of the writer's Win32 path).
std::pair<uint16_t, uint16_t> qtDos(int64_t ms)
{
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(ms).toLocalTime();
    const uint16_t date = static_cast<uint16_t>(((t.date().year() - 1980) << 9) | (t.date().month() << 5) | t.date().day());
    const uint16_t time = static_cast<uint16_t>((t.time().hour() << 11) | (t.time().minute() << 5) | (t.time().second() / 2));
    return {time, date};
}

int64_t localMs(int y, int mo, int d, int h, int mi, int s, int ms = 0)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms)).toMSecsSinceEpoch();
}

std::string randomBytes(size_t n, uint32_t seed)
{
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[i] = static_cast<char>(x & 0xFF);
    }
    return s;
}

std::string textBytes(size_t n)
{
    static const char kWords[] = "station peer bridge mesh share folder upload stream archive deflate zip64 ";
    std::string s;
    s.reserve(n);
    for (size_t i = 0; s.size() < n; ++i) {
        s += kWords + (i * 7) % 60;
        s += std::to_string(i % 97);
        s += '\n';
    }
    s.resize(n);
    return s;
}

// Hands out its data in pieces of 1..maxPiece bytes (Source::read may return any 1..n) and announces `announce` as its
// length (-1: unknown).
class PieceSource final : public Source {
public:
    PieceSource(std::string data, int64_t announce, size_t maxPiece = 7919)
        : data_(std::move(data)), announce_(announce), max_(maxPiece) {}
    size_t read(char* buf, size_t n) override
    {
        if (pos_ >= data_.size()) return 0;
        x_ = x_ * 1103515245u + 12345u;
        const size_t k = std::min({n, data_.size() - pos_, size_t(1) + (x_ >> 8) % max_});
        std::memcpy(buf, data_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    int64_t length() const override { return announce_; }
private:
    std::string data_;
    int64_t announce_;
    size_t max_;
    size_t pos_ = 0;
    uint32_t x_ = 7;
};

// Delivers data until failAt bytes went out, then throws IoError.
class FailingSource final : public Source {
public:
    FailingSource(std::string data, size_t failAt, bool peerGone = false)
        : data_(std::move(data)), failAt_(failAt), peerGone_(peerGone) {}
    size_t read(char* buf, size_t n) override
    {
        if (pos_ >= failAt_) throw IoError("disk gone", peerGone_);
        const size_t k = std::min({n, failAt_ - pos_, data_.size() - pos_});
        std::memcpy(buf, data_.data() + pos_, k);
        pos_ += k;
        return k;
    }
    int64_t length() const override { return static_cast<int64_t>(data_.size()); }
private:
    std::string data_;
    size_t failAt_;
    bool peerGone_;
    size_t pos_ = 0;
};

// Records every write and flush; with failAfter >= 0 a write that would pass that many bytes throws (peer gone).
class RecordingSink final : public Sink {
public:
    explicit RecordingSink(int64_t failAfter = -1) : failAfter_(failAfter) {}
    using Sink::write;
    void write(const char* p, size_t n) override
    {
        if (failAfter_ >= 0 && static_cast<int64_t>(data.size() + n) > failAfter_) throw IoError("connection reset", true);
        data.append(p, n);
        writes.push_back(n);
    }
    void flush() override { ++flushes; }
    std::string data;
    std::vector<size_t> writes;
    int flushes = 0;
private:
    int64_t failAfter_;
};

// size bytes of zeros with marks laid over them (a sparse file's content without the file).
class ZeroMarkSource final : public Source {
public:
    ZeroMarkSource(uint64_t size, std::vector<std::pair<uint64_t, std::string>> marks, int64_t announce)
        : size_(size), marks_(std::move(marks)), announce_(announce) {}
    size_t read(char* buf, size_t n) override
    {
        if (pos_ >= size_) return 0;
        const size_t k = static_cast<size_t>(std::min<uint64_t>(n, size_ - pos_));
        std::memset(buf, 0, k);
        for (const auto& m : marks_) {
            const uint64_t a = std::max(pos_, m.first), b = std::min(pos_ + k, m.first + m.second.size());
            if (a < b) std::memcpy(buf + (a - pos_), m.second.data() + (a - m.first), static_cast<size_t>(b - a));
        }
        pos_ += k;
        return k;
    }
    int64_t length() const override { return announce_; }
private:
    uint64_t size_;
    std::vector<std::pair<uint64_t, std::string>> marks_;
    int64_t announce_;
    uint64_t pos_ = 0;
};

// File sink for archives of mostly zeros: the file is sparse and all-zero 4 KB blocks are left as holes, so a stored
// 4 GiB entry of a sparse file costs a few MB of disk.
class SparseFileSink final : public Sink {
public:
    bool open(const std::string& path)
    {
        h_ = CreateFileW(fs::widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        DWORD ret = 0;
        return DeviceIoControl(h_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr) != FALSE;
    }
    using Sink::write;
    void write(const char* p, size_t n) override
    {
        static const std::string zeros(4096, '\0');
        while (n) {
            const size_t k = static_cast<size_t>(std::min<uint64_t>(n, 4096 - pos_ % 4096));
            if (std::memcmp(p, zeros.data(), k) != 0) {
                LARGE_INTEGER li;
                li.QuadPart = static_cast<LONGLONG>(pos_);
                DWORD put = 0;
                if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) || !WriteFile(h_, p, static_cast<DWORD>(k), &put, nullptr) ||
                    put != k)
                    throw IoError("sparse sink write failed");
            }
            pos_ += k;
            p += k;
            n -= k;
        }
    }
    bool close()
    {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(pos_);
        const bool ok = SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) && SetEndOfFile(h_);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        return ok;
    }
    ~SparseFileSink() override { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    uint64_t size() const { return pos_; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t pos_ = 0;
};

bool makeSparseFile(const std::string& path, uint64_t size, const std::vector<std::pair<uint64_t, std::string>>& marks)
{
    HANDLE h = CreateFileW(fs::widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr) != FALSE;
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    ok = ok && SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    for (const auto& m : marks) {
        li.QuadPart = static_cast<LONGLONG>(m.first);
        DWORD put = 0;
        ok = ok && SetFilePointerEx(h, li, nullptr, FILE_BEGIN) &&
             WriteFile(h, m.second.data(), static_cast<DWORD>(m.second.size()), &put, nullptr) && put == m.second.size();
    }
    CloseHandle(h);
    return ok;
}

bool setMtime(const std::string& path, int64_t ms)
{
    HANDLE h = CreateFileW(fs::widen(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<uint64_t>(ms + 11644473600000LL) * 10000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    const bool ok = SetFileTime(h, nullptr, nullptr, &ft) != FALSE;
    CloseHandle(h);
    return ok;
}

QString findPwsh()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
    if (p.isEmpty() && QFileInfo::exists(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe")))
        p = QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe");
    return p;
}

QString findBsdtar()
{
    const QString p = QStringLiteral("C:/Windows/System32/tar.exe");
    return QFileInfo::exists(p) ? p : QString();
}

QString psQuote(const std::string& s) { return QLatin1Char('\'') + qs(s).replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\''); }

// Runs a tool and returns its exit code (-1: did not start or timed out); stdout can go to the null device.
int runCode(const QString& exe, const QStringList& args, QByteArray* out = nullptr, QByteArray* err = nullptr,
            int timeoutMs = 120000, bool discardOut = false)
{
    QProcess p;
    if (discardOut) p.setStandardOutputFile(QProcess::nullDevice());
    p.start(exe, args);
    if (!p.waitForStarted(10000)) return -1;
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        return -1;
    }
    if (out && !discardOut) *out = p.readAllStandardOutput();
    if (err) *err = p.readAllStandardError();
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

int runPs(const QString& exe, const QString& script, QByteArray* out = nullptr, QByteArray* err = nullptr,
          int timeoutMs = 300000)
{
    return runCode(exe, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
                         QStringLiteral("$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Stop'; ") + script},
                   out, err, timeoutMs);
}

// .NET's view of an archive: "<hex(UTF-8 name)> <Length> <CompressedLength> <Crc32 or ->" per entry.
QString dotnetListScript(const std::string& zip)
{
    return QStringLiteral(
               "try { Add-Type -AssemblyName System.IO.Compression.FileSystem } catch { Add-Type -AssemblyName System.IO.Compression.ZipFile }; "
               "$z = [System.IO.Compression.ZipFile]::OpenRead(%1); try { $o = New-Object System.Text.StringBuilder; "
               "foreach ($e in $z.Entries) { $c = '-'; if ($e.PSObject.Properties['Crc32']) { $c = [string]$e.Crc32 }; "
               "[void]$o.Append(([BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($e.FullName))).Replace('-','')).Append(' ')"
               ".Append($e.Length).Append(' ').Append($e.CompressedLength).Append(' ').Append($c).Append([char]10) }; "
               "[Console]::Out.Write($o.ToString()) } finally { $z.Dispose() }")
        .arg(psQuote(zip));
}

struct DotnetEntry {
    std::string name;
    uint64_t length = 0, compressed = 0;
    QByteArray crc;
};

std::vector<DotnetEntry> parseDotnetList(const QByteArray& out)
{
    std::vector<DotnetEntry> v;
    for (const QByteArray& line : out.split('\n')) {
        const QList<QByteArray> f = line.trimmed().split(' ');
        if (f.size() != 4) continue;
        DotnetEntry e;
        e.name = QByteArray::fromHex(f[0]).toStdString();
        e.length = f[1].toULongLong();
        e.compressed = f[2].toULongLong();
        e.crc = f[3];
        v.push_back(std::move(e));
    }
    return v;
}

// Compares two files in 1 MB pieces; "" when equal.
std::string compareFiles(const std::string& a, const std::string& b)
{
    fs::FileReader ra, rb;
    std::string err;
    if (!ra.open(a, &err) || !rb.open(b, &err)) return "cannot open: " + err;
    if (ra.size() != rb.size()) return "sizes differ: " + std::to_string(ra.size()) + " vs " + std::to_string(rb.size());
    std::string x(1 << 20, '\0'), y(1 << 20, '\0');
    uint64_t at = 0;
    for (;;) {
        size_t kx = 0, ky = 0;
        while (kx < x.size()) { const size_t k = ra.read(&x[kx], x.size() - kx); if (!k) break; kx += k; }
        while (ky < y.size()) { const size_t k = rb.read(&y[ky], y.size() - ky); if (!k) break; ky += k; }
        if (kx != ky) return "short read at " + std::to_string(at);
        if (std::memcmp(x.data(), y.data(), kx) != 0) return "content differs in the MB at " + std::to_string(at);
        if (kx < x.size()) return {};
        at += kx;
    }
}

} // namespace

class TestAnfsCoreFs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void pathVectors();
    void resolveRelativeAndDriveCwd();
    void lowerKeyAndIsUnder();
    void lowerInvariantMatchesDotNetFramework();
    void safeRelDirVectors();
    void conflictNameVectors();
    void nodePathDifferential();
    void widenNarrowWtf8();
    void listDirRawOrder();
    void listDirErrorsAndEarlyStop();
    void reparsePointsFlaggedNotFollowed();
    void statExistsAndLockedFiles();
    void makeDirsRemoveRename();
    void makeDirsOddNamesNeverHang();
    void removeTreeExactNames();
    void readerWriterAndReadFile();
    void longAndDeepPaths();
    void namesAndFolders();
    void nodeErrorTexts();
    void drivesAndElevation();
    void mimeVectors();
    void mimeMatchesPackages();
    void zipGoldenBytes();
    void zipEntryKinds();
    void zipNamesAndTimes();
    void zipErrorsAndMisuse();
    void zipManyEntriesZip64End();
    void zipRoundTripExpandArchive();
    void zipZip64Over4GiB();

private:
    QTemporaryDir tmp_;
    std::string root_;   // native UTF-8 path of tmp_
    std::string sub(const std::string& name) const { return root_ + "\\" + name; }
};

void TestAnfsCoreFs::initTestCase()
{
    QVERIFY(tmp_.isValid());
    root_ = u8(QDir::toNativeSeparators(tmp_.path()));
    // Expand an 8.3 temp folder so names compare as the APIs return them.
    root_ = fs::longPath(root_);
    QVERIFY(fs::isDir(root_));
}

// ---- nodepath -----------------------------------------------------------------------------------------------------

void TestAnfsCoreFs::pathVectors()
{
    // Header vectors (nodepath.h, CONVENTIONS.md section 2).
    SEQ(nodepath::resolve("C:\\a\\..\\b\\"), "C:\\b");
    SEQ(nodepath::basename("C:\\a\\b\\"), "b");
    SEQ(nodepath::extname(".bashrc"), "");
    SEQ(nodepath::extname("a.tar.gz"), ".gz");
    SEQ(nodepath::safeRelDir("C:\\x/../y\\.\\z"), "x\\y\\z");

    // Produced by Node 22.13.1 path.win32.
    // normalize
    SEQ(nodepath::normalize(""), ".");
    SEQ(nodepath::normalize("."), ".");
    SEQ(nodepath::normalize("/"), "\\");
    SEQ(nodepath::normalize("\\"), "\\");
    SEQ(nodepath::normalize("C:"), "C:.");
    SEQ(nodepath::normalize("C:\\"), "C:\\");
    SEQ(nodepath::normalize("c:/"), "c:\\");
    SEQ(nodepath::normalize("C:\\a\\..\\b\\"), "C:\\b\\");
    SEQ(nodepath::normalize("C:/a//b/"), "C:\\a\\b\\");
    SEQ(nodepath::normalize("\\\\srv\\share"), "\\\\srv\\share\\");
    SEQ(nodepath::normalize("\\\\srv\\share\\"), "\\\\srv\\share\\");
    SEQ(nodepath::normalize("\\\\srv\\share\\a\\..\\.."), "\\\\srv\\share\\");
    SEQ(nodepath::normalize("//srv/share/x/./y"), "\\\\srv\\share\\x\\y");
    SEQ(nodepath::normalize("a\\..\\..\\b"), "..\\b");
    SEQ(nodepath::normalize("..\\a"), "..\\a");
    SEQ(nodepath::normalize("a/b/../../.."), "..");
    SEQ(nodepath::normalize("C:..\\x"), "C:..\\x");
    SEQ(nodepath::normalize("C:a\\..\\..\\b"), "C:..\\b");
    SEQ(nodepath::normalize("\\\\"), "\\");
    SEQ(nodepath::normalize("\\\\\\x"), "\\x");
    SEQ(nodepath::normalize("///a"), "\\a");
    SEQ(nodepath::normalize("a:b"), "a:b");
    SEQ(nodepath::normalize("x\\..\\c:\\y"), ".\\c:\\y");   // CVE-2025-23084
    SEQ(nodepath::normalize("foo:bar\\baz"), "foo:bar\\baz");
    SEQ(nodepath::normalize("a\\b:"), ".\\a\\b:");           // CVE-2025-23084
    SEQ(nodepath::normalize("\\\\?\\C:\\a\\..\\b"), "\\\\?\\C:\\b");
    SEQ(nodepath::normalize("\\\\.\\pipe\\x"), "\\\\.\\pipe\\x");
    SEQ(nodepath::normalize("é\\..\\ü\\"), "ü\\");
    SEQ(nodepath::normalize("a\\.\\b\\."), "a\\b");
    SEQ(nodepath::normalize("C:\\.."), "C:\\");
    SEQ(nodepath::normalize("C:\\a\\b\\..\\..\\..\\c"), "C:\\c");
    SEQ(nodepath::normalize("a\\.."), ".");
    SEQ(nodepath::normalize("a\\..\\"), ".\\");
    SEQ(nodepath::normalize(".."), "..");
    SEQ(nodepath::normalize("..\\..\\"), "..\\..\\");
    // join
    SEQ(nodepath::join("C:\\a", "..\\b"), "C:\\b");
    SEQ(nodepath::join("C:\\a\\", "\\b"), "C:\\a\\b");
    SEQ(nodepath::join("C:\\", "x"), "C:\\x");
    SEQ(nodepath::join("a", "b/c"), "a\\b\\c");
    SEQ(nodepath::join("C:\\a", "../../x"), "C:\\x");
    SEQ(nodepath::join("a", "..\\..\\x"), "..\\x");
    SEQ(nodepath::join("", ""), ".");
    SEQ(nodepath::join("C:\\a", ""), "C:\\a");
    SEQ(nodepath::join("C:\\a\\b", ".."), "C:\\a");
    SEQ(nodepath::join("C:\\a", "b\\"), "C:\\a\\b\\");
    SEQ(nodepath::join("\\\\srv\\share", "x"), "\\\\srv\\share\\x");
    SEQ(nodepath::join("//server", "share"), "\\\\server\\share\\");
    SEQ(nodepath::join("\\\\\\x", "y"), "\\x\\y");
    SEQ(nodepath::join("//", "x"), "\\x");
    SEQ(nodepath::join("", "\\\\a\\b"), "\\\\a\\b\\");
    SEQ(nodepath::join("C:", "x"), "C:\\x");
    SEQ(nodepath::join("C:", "..\\x"), "C:\\x");
    SEQ(nodepath::join("x", "c:y"), "x\\c:y");
    SEQ(nodepath::join("\\a", "\\\\b"), "\\a\\b");
    SEQ(nodepath::join("x", ":y\\z"), "x\\:y\\z");
    // basename
    SEQ(nodepath::basename("C:\\a\\b\\"), "b");
    SEQ(nodepath::basename("C:\\"), "");
    SEQ(nodepath::basename("a/b.txt"), "b.txt");
    SEQ(nodepath::basename("noslash"), "noslash");
    SEQ(nodepath::basename("C:\\u\\.bashrc"), ".bashrc");
    SEQ(nodepath::basename("C:"), "");
    SEQ(nodepath::basename("C:x"), "x");
    SEQ(nodepath::basename("\\\\srv\\share\\"), "share");
    SEQ(nodepath::basename(""), "");
    SEQ(nodepath::basename("\\"), "");
    SEQ(nodepath::basename("C:\\a\\b\\\\\\"), "b");
    SEQ(nodepath::basename("é.txt"), "é.txt");
    SEQ(nodepath::basename("\\\\srv\\share\\f.mp4"), "f.mp4");
    SEQ(nodepath::basename("C:\\a\\b.txt", ".txt"), "b");
    SEQ(nodepath::basename("b.txt", "b.txt"), "");
    SEQ(nodepath::basename("a.tar.gz", ".gz"), "a.tar");
    SEQ(nodepath::basename("c\\", "bc"), "c\\");   // Node quirk: a partial suffix match keeps the trailing separator
    SEQ(nodepath::basename(".bashrc", ".bashrc"), "");
    SEQ(nodepath::basename("aaa", "a"), "aa");
    SEQ(nodepath::basename("a\\b.txt\\\\", ".txt"), "b");
    SEQ(nodepath::basename("C:x.js", ".js"), "x");
    SEQ(nodepath::basename("x.JS", ".js"), "x.JS");
    SEQ(nodepath::basename("é.é", "é"), "é.");
    SEQ(nodepath::basename("dir\\é", "xé"), "é");
    SEQ(nodepath::basename("a", "é"), "a");   // the suffix is longer in bytes, not in UTF-16 units
    SEQ(nodepath::basename("C:\\a.b\\", ".b"), "a");
    SEQ(nodepath::basename("x.txt", ""), "x.txt");
    // dirname
    SEQ(nodepath::dirname("C:\\a\\b"), "C:\\a");
    SEQ(nodepath::dirname("C:\\a\\b\\"), "C:\\a");
    SEQ(nodepath::dirname("C:\\"), "C:\\");
    SEQ(nodepath::dirname("C:"), "C:");
    SEQ(nodepath::dirname("C:a"), "C:");
    SEQ(nodepath::dirname("\\\\srv\\share"), "\\\\srv\\share");
    SEQ(nodepath::dirname("\\\\srv\\share\\x"), "\\\\srv\\share\\");
    SEQ(nodepath::dirname("\\\\srv\\share\\x\\y"), "\\\\srv\\share\\x");
    SEQ(nodepath::dirname("a"), ".");
    SEQ(nodepath::dirname(""), ".");
    SEQ(nodepath::dirname("\\"), "\\");
    SEQ(nodepath::dirname("/a/b"), "/a");
    SEQ(nodepath::dirname("a\\b\\\\"), "a");
    SEQ(nodepath::dirname("\\\\srv"), "\\");
    SEQ(nodepath::dirname("C:\\a"), "C:\\");
    // extname
    SEQ(nodepath::extname(".bashrc"), "");
    SEQ(nodepath::extname("a.tar.gz"), ".gz");
    SEQ(nodepath::extname("noext"), "");
    SEQ(nodepath::extname("a."), ".");
    SEQ(nodepath::extname(".."), "");
    SEQ(nodepath::extname("..."), ".");
    SEQ(nodepath::extname("a.."), ".");
    SEQ(nodepath::extname("C:\\u\\.bashrc"), "");
    SEQ(nodepath::extname("C:.x"), "");
    SEQ(nodepath::extname("a\\b.c\\"), ".c");
    SEQ(nodepath::extname(".a.b"), ".b");
    SEQ(nodepath::extname("x\\."), "");
    SEQ(nodepath::extname("C:\\x..y"), ".y");
    SEQ(nodepath::extname("file.MP4"), ".MP4");
    SEQ(nodepath::extname("C:\\dir.d\\file"), "");
    // isAbsolute
    QCOMPARE(nodepath::isAbsolute("C:\\"), true);
    QCOMPARE(nodepath::isAbsolute("C:/"), true);
    QCOMPARE(nodepath::isAbsolute("C:"), false);
    QCOMPARE(nodepath::isAbsolute("C:x"), false);
    QCOMPARE(nodepath::isAbsolute("\\x"), true);
    QCOMPARE(nodepath::isAbsolute("/x"), true);
    QCOMPARE(nodepath::isAbsolute("\\\\srv\\s"), true);
    QCOMPARE(nodepath::isAbsolute("x"), false);
    QCOMPARE(nodepath::isAbsolute(""), false);
    QCOMPARE(nodepath::isAbsolute("c:\\"), true);
    QCOMPARE(nodepath::isAbsolute("1:\\x"), false);
    // resolve (absolute inputs only: no cwd involved)
    SEQ(nodepath::resolve("C:\\a\\..\\b\\"), "C:\\b");
    SEQ(nodepath::resolve("C:\\"), "C:\\");
    SEQ(nodepath::resolve("c:/"), "c:\\");
    SEQ(nodepath::resolve("C:/a//b/"), "C:\\a\\b");
    SEQ(nodepath::resolve("\\\\srv\\share\\x\\.."), "\\\\srv\\share\\");
    SEQ(nodepath::resolve("C:\\a", "..\\b"), "C:\\b");
    SEQ(nodepath::resolve("C:\\a", "D:\\b"), "D:\\b");
    SEQ(nodepath::resolve("C:\\a", "\\b"), "C:\\b");
    SEQ(nodepath::resolve("C:\\a", "C:b"), "C:\\a\\b");
    SEQ(nodepath::resolve("\\\\srv\\share", "x"), "\\\\srv\\share\\x");
    SEQ(nodepath::resolve("C:\\x", "\\\\srv\\s\\y"), "\\\\srv\\s\\y");
    SEQ(nodepath::resolve("C:\\a\\b", "..\\..\\..\\.."), "C:\\");
    SEQ(nodepath::resolve("\\\\srv\\share"), "\\\\srv\\share\\");
    // The C# PathRules vectors that Node answers the same way.
    SEQ(nodepath::join("\\\\srv\\share", "x"), "\\\\srv\\share\\x");
    SEQ(nodepath::extname("a."), ".");
}

void TestAnfsCoreFs::resolveRelativeAndDriveCwd()
{
    const std::string cwd = fs::cwd();
    QVERIFY(!cwd.empty());
    SEQ(fs::cwd(), u8(QDir::toNativeSeparators(QDir::currentPath())));
    SEQ(nodepath::resolve(""), cwd);
    SEQ(nodepath::resolve("."), cwd);
    SEQ(nodepath::resolve("a\\b\\..\\c"), nodepath::join(cwd, "a\\c"));
    // A rooted path without a drive takes the cwd's drive.
    SEQ(nodepath::resolve("\\x\\y"), cwd.substr(0, 2) + "\\x\\y");

    // "Q:rel" resolves against the hidden =Q: variable, else the drive root (the process cwd is on another drive).
    const std::string q = freeDriveLetter();
    if (q.empty() || q == cwd.substr(0, 2)) QSKIP("no free drive letter");
    const std::wstring var = L"=" + fs::widen(q);
    if (!SetEnvironmentVariableW(var.c_str(), (fs::widen(q) + L"\\deep\\dir").c_str()))
        QSKIP("the hidden =X: variables cannot be set here");
    SEQ(nodepath::resolve(q + "x\\y"), q + "\\deep\\dir\\x\\y");
    SEQ(nodepath::resolve(q + "..\\z"), q + "\\deep\\z");
    SetEnvironmentVariableW(var.c_str(), L"C:\\elsewhere");   // names another drive: the root wins
    SEQ(nodepath::resolve(q + "x"), q + "\\x");
    SetEnvironmentVariableW(var.c_str(), nullptr);
    SEQ(nodepath::resolve(q + "x"), q + "\\x");
    SEQ(nodepath::resolve(q), q + "\\");
}

void TestAnfsCoreFs::lowerKeyAndIsUnder()
{
    SEQ(nodepath::toLowerInvariant("ABC xyz 019"), "abc xyz 019");
    SEQ(nodepath::toLowerInvariant("ÄÖÜ ÉÈ"), "äöü éè");
    SEQ(nodepath::toLowerInvariant("ΣΑΣ"), "σασ");          // simple mapping: no final sigma
    SEQ(nodepath::toLowerInvariant("ÀB\xF0\x90\x90\x80"), "àb\xF0\x90\x90\xA8");   // Deseret pair
    SEQ(nodepath::toLowerInvariant(""), "");
    SEQ(nodepath::key("C:\\Foo\\..\\BAR\\Ä"), "c:\\bar\\ä");
    SEQ(nodepath::key("c:/X/"), "c:\\x");

    QVERIFY(nodepath::isUnder("c:\\", "c:\\x"));
    QVERIFY(nodepath::isUnder("c:\\", "c:\\"));
    QVERIFY(nodepath::isUnder("c:\\a", "c:\\a"));
    QVERIFY(nodepath::isUnder("c:\\a", "c:\\a\\b"));
    QVERIFY(!nodepath::isUnder("c:\\a", "c:\\ab"));
    QVERIFY(!nodepath::isUnder("c:\\a", "c:\\"));
    QVERIFY(!nodepath::isUnder("", "c:\\"));
    QVERIFY(!nodepath::isUnder("c:\\a\\b", "c:\\a"));
    QVERIFY(nodepath::isUnder("\\\\srv\\share\\", "\\\\srv\\share\\x"));
    // Keys from different spellings meet.
    QVERIFY(nodepath::isUnder(nodepath::key("C:\\Shares\\Media"), nodepath::key("c:/shares/MEDIA/x/../y.mp4")));
    QVERIFY(!nodepath::isUnder(nodepath::key("C:\\Shares\\Media"), nodepath::key("C:\\Shares\\Media\\..\\Other")));
}

void TestAnfsCoreFs::lowerInvariantMatchesDotNetFramework()
{
    const QString ps = findPowerShell51();
    if (ps.isEmpty()) QSKIP("powershell.exe 5.1 not found");
    std::vector<uint32_t> cps;
    for (uint32_t c = 1; c < 0x10000; ++c)
        if (c < 0xD800 || c > 0xDFFF) cps.push_back(c);
    for (uint32_t c = 0x10400; c < 0x10450; ++c) cps.push_back(c);   // Deseret
    for (uint32_t c = 0x104B0; c < 0x104D4; ++c) cps.push_back(c);   // Osage
    for (uint32_t c = 0x10C80; c < 0x10CB3; ++c) cps.push_back(c);   // Old Hungarian
    for (uint32_t c = 0x118A0; c < 0x118C0; ++c) cps.push_back(c);   // Warang Citi
    for (uint32_t c = 0x16E40; c < 0x16E60; ++c) cps.push_back(c);   // Medefaidrin
    for (uint32_t c = 0x1E900; c < 0x1E922; ++c) cps.push_back(c);   // Adlam
    cps.push_back(0x1F600);
    std::string utf8;
    for (uint32_t c : cps) appendUtf8(utf8, c);
    const std::wstring in = fs::widen(utf8);

    const std::string inPath = sub("lower-in.txt"), outPath = sub("lower-out.txt");
    QVERIFY(writeBytes(inPath, std::string(reinterpret_cast<const char*>(in.data()), in.size() * 2)));
    const QString script = QStringLiteral("$s=[IO.File]::ReadAllText('%1',[Text.Encoding]::Unicode);"
                                          "[IO.File]::WriteAllText('%2',$s.ToLowerInvariant(),[Text.Encoding]::Unicode)")
                               .arg(qs(inPath), qs(outPath));
    if (!runTool(ps, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"), script}))
        QSKIP("powershell.exe did not run");
    std::string raw = readBytes(outPath);
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) raw.erase(0, 2);
    const std::wstring want(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
    const std::wstring got = fs::widen(nodepath::toLowerInvariant(utf8));
    QCOMPARE(got.size(), want.size());
    int bad = 0;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != want[i] && ++bad <= 10)
            qWarning("unit %zu: U+%04X -> ours U+%04X, .NET U+%04X", i, static_cast<unsigned>(in[i]),
                     static_cast<unsigned>(got[i]), static_cast<unsigned>(want[i]));
    QCOMPARE(bad, 0);
    qInfo(".NET Framework ToLowerInvariant: %zu UTF-16 units identical", got.size());
}

void TestAnfsCoreFs::safeRelDirVectors()
{
    // mod-fscore.md and the C# FsCoreTests vectors.
    SEQ(nodepath::safeRelDir("C:\\x/../y\\.\\z"), "x\\y\\z");
    SEQ(nodepath::safeRelDir("..\\..\\a"), "a");
    SEQ(nodepath::safeRelDir("\\\\srv\\share\\f"), "srv\\share\\f");
    SEQ(nodepath::safeRelDir("a//b"), "a\\b");
    SEQ(nodepath::safeRelDir(""), "");
    SEQ(nodepath::safeRelDir("C:"), "");
    SEQ(nodepath::safeRelDir("D:\\"), "");
    SEQ(nodepath::safeRelDir("a\\D:\\b"), "a\\b");
    SEQ(nodepath::safeRelDir("..\\.\\.."), "");
    SEQ(nodepath::safeRelDir("/../../passwd"), "passwd");
    // agent.js regexes: only a leading "X:" is stripped; "X:" segments go, "X:y" segments and "..." stay.
    SEQ(nodepath::safeRelDir("C:C:\\x"), "x");
    SEQ(nodepath::safeRelDir("1:\\x"), "1:\\x");
    SEQ(nodepath::safeRelDir("a\\b:c\\..."), "a\\b:c\\...");
    SEQ(nodepath::safeRelDir("é/日本\\x.txt"), "é\\日本\\x.txt");
    const std::string hostile = nodepath::safeRelDir(std::string("\0\\..\\x\\\\y\\C:\\z", 14));
    QVERIFY(hostile.find("..") == std::string::npos);
    QVERIFY(hostile.find(':') == std::string::npos);
    SEQ(hostile, std::string("\0\\x\\y\\z", 7));
}

void TestAnfsCoreFs::conflictNameVectors()
{
    const std::string dir = sub("conflict");
    QVERIFY(fs::makeDirs(dir));
    const std::string a = dir + "\\a.txt";
    SEQ(nodepath::conflictName(a), a);   // free name unchanged
    QVERIFY(writeBytes(a, "x"));
    SEQ(nodepath::conflictName(a), dir + "\\a (1).txt");
    QVERIFY(writeBytes(dir + "\\a (1).txt", "x"));
    SEQ(nodepath::conflictName(a), dir + "\\a (2).txt");   // the header vector
    QVERIFY(writeBytes(dir + "\\.bashrc", "x"));
    SEQ(nodepath::conflictName(dir + "\\.bashrc"), dir + "\\.bashrc (1)");
    QVERIFY(writeBytes(dir + "\\noext", "x"));
    SEQ(nodepath::conflictName(dir + "\\noext"), dir + "\\noext (1)");
    QVERIFY(writeBytes(dir + "\\a.tar.gz", "x"));
    SEQ(nodepath::conflictName(dir + "\\a.tar.gz"), dir + "\\a.tar (1).gz");
    QVERIFY(fs::makeDirs(dir + "\\folder"));
    SEQ(nodepath::conflictName(dir + "\\folder"), dir + "\\folder (1)");
    QVERIFY(writeBytes(dir + "\\Case.TXT", "x"));
    SEQ(nodepath::conflictName(dir + "\\case.txt"), dir + "\\case (1).txt");   // NTFS names compare without case
    QVERIFY(writeBytes(dir + "\\é.mp4", "x"));
    SEQ(nodepath::conflictName(dir + "\\é.mp4"), dir + "\\é (1).mp4");
}

void TestAnfsCoreFs::nodePathDifferential()
{
    const QString node = findNode();
    if (node.isEmpty()) QSKIP("node.exe not found: the literal Node vectors above still ran");
    static const char* const kTokens[] = {"C:", "c:", "D:", "Q:", "\\", "/", "\\\\", "//", "..", ".", "...", "a", "B",
                                          "b.txt", ".x", "x.", "srv", "share", ":", "é", "日本", "😀", " ", "a b",
                                          "x.tar.gz", "\\\\?\\", "?", "~", "node_modules", ".bashrc", "C:\\", "\\\\.\\"};
    QRandomGenerator rng(20261026);
    std::vector<std::pair<std::string, std::string>> cases = {{"C:\\a\\..\\b\\", ".txt"}, {"c\\", "bc"}, {"", ""},
                                                              {"\\\\srv\\share", "x"}, {"a\\b:", "c"}};
    for (int i = 0; i < 3000; ++i) {
        std::string a, b;
        const int na = static_cast<int>(rng.bounded(8)), nb = static_cast<int>(rng.bounded(4));
        for (int k = 0; k < na; ++k) a += kTokens[rng.bounded(static_cast<quint32>(std::size(kTokens)))];
        for (int k = 0; k < nb; ++k) b += kTokens[rng.bounded(static_cast<quint32>(std::size(kTokens)))];
        cases.emplace_back(a, b);
    }
    QByteArray input;
    for (const auto& c : cases) input += qb(c.first).toHex() + ' ' + qb(c.second).toHex() + '\n';
    const std::string script = sub("path-oracle.js"), in = sub("path-in.txt"), out = sub("path-out.txt");
    QVERIFY(writeBytes(script, nodePathJs()));
    QVERIFY(writeBytes(in, input.toStdString()));
    if (!runTool(node, {qs(script), qs(in), qs(out)})) QFAIL("node oracle failed");
    const QList<QByteArray> lines = QByteArray::fromStdString(readBytes(out)).split('\n');
    QVERIFY(lines.size() >= static_cast<qsizetype>(cases.size()));
    static const char* const kNames[] = {"normalize", "join", "basename", "basename(p,ext)", "dirname", "extname",
                                         "isAbsolute", "resolve(p)", "resolve(a,b)"};
    int bad = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const std::string& a = cases[i].first;
        const std::string& b = cases[i].second;
        const std::string got[9] = {nodepath::normalize(a), nodepath::join(a, b), nodepath::basename(a),
                                    nodepath::basename(a, b), nodepath::dirname(a), nodepath::extname(a),
                                    nodepath::isAbsolute(a) ? "1" : "0", nodepath::resolve(a), nodepath::resolve(a, b)};
        const QList<QByteArray> cols = lines[static_cast<qsizetype>(i)].split(' ');
        if (cols.size() != 9) {
            ++bad;
            qWarning("line %zu: %d columns", i, static_cast<int>(cols.size()));
            continue;
        }
        for (int k = 0; k < 9; ++k) {
            const std::string want = QByteArray::fromHex(cols[k]).toStdString();
            if (got[k] != want && ++bad <= 15)
                qWarning("%s(\"%s\", \"%s\"): ours \"%s\", node \"%s\"", kNames[k], a.c_str(), b.c_str(), got[k].c_str(),
                         want.c_str());
        }
    }
    QCOMPARE(bad, 0);
    qInfo("node path.win32: %zu cases x 9 functions identical", cases.size());
}

void TestAnfsCoreFs::widenNarrowWtf8()
{
    QVERIFY(fs::widen("aé日😀") == std::wstring(L"a\u00e9\u65e5\xD83D\xDE00"));
    SEQ(fs::narrow(L"a\u00e9\u65e5\xD83D\xDE00"), "aé日😀");
    // A lone surrogate in a file name round-trips as WTF-8.
    const std::wstring lone = std::wstring(L"x") + wchar_t(0xD800) + L"y" + wchar_t(0xDFFF);
    SEQ(fs::narrow(lone), "x\xED\xA0\x80y\xED\xBF\xBF");
    QVERIFY(fs::widen(fs::narrow(lone)) == lone);
    // Invalid UTF-8: one U+FFFD per maximal subpart (WHATWG).
    QVERIFY(fs::widen("\xC0\x80") == std::wstring(L"\xFFFD\xFFFD"));
    QVERIFY(fs::widen("\xE2\x82") == std::wstring(L"\xFFFD"));
    QVERIFY(fs::widen("\xE2\x82x") == std::wstring(L"\xFFFDx"));
    QVERIFY(fs::widen("\xF4\x90\x80\x80") == std::wstring(L"\xFFFD\xFFFD\xFFFD\xFFFD"));
    QVERIFY(fs::widen("\xFF") == std::wstring(L"\xFFFD"));
    QVERIFY(fs::widen(std::string("a\0b", 3)) == std::wstring(L"a\0b", 3));
    QVERIFY(fs::widen("").empty());
    QVERIFY(fs::narrow(L"").empty());
}

// ---- listing ------------------------------------------------------------------------------------------------------

void TestAnfsCoreFs::listDirRawOrder()
{
    const std::string dir = sub("order");
    QVERIFY(fs::makeDirs(dir));
    // Names whose NTFS order (upcase-table collation) differs from a byte sort: '_' (0x5F) sorts after 'A'..'Z' in
    // bytes but NTFS compares the upcased "_A" with "A1" ... and lower-case letters fold onto upper case.
    const std::vector<std::string> files = {"B", "_a", "a1", "Zz", "ä", "[x]", "~t", "10", "9", "é", "日本.txt",
                                            "😀.bin", ".hidden", "UPPER.TXT", "lower.txt", "sys.dat"};
    for (size_t i = 0; i < files.size(); ++i) QVERIFY(writeBytes(dir + "\\" + files[i], std::string(i * 7, 'z')));
    QVERIFY(fs::makeDirs(dir + "\\sub"));
    QVERIFY(fs::makeDirs(dir + "\\node_modules"));
    SetFileAttributesW(fs::widen(dir + "\\.hidden").c_str(), FILE_ATTRIBUTE_HIDDEN);
    SetFileAttributesW(fs::widen(dir + "\\sys.dat").c_str(), FILE_ATTRIBUTE_SYSTEM);

    std::vector<fs::DirEntry> entries;
    uint32_t err = 99;
    QVERIFY(fs::listDir(dir, [&](const fs::DirEntry& e) { entries.push_back(e); return true; }, &err));
    QCOMPARE(err, 0u);
    std::vector<std::string> names;
    for (const auto& e : entries) names.push_back(e.name);
    const std::vector<std::string> raw = rawListing(dir);
    QCOMPARE(names.size(), files.size() + 2);
    QVERIFY2(names == raw, "listDir order differs from the raw FindFirstFileW order");
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    QVERIFY2(names != sorted, "the crafted names should not come back in byte order");

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& e : entries) {
        const std::string p = dir + "\\" + e.name;
        const fs::FileStat st = fs::stat(p);
        QVERIFY2(st.exists, p.c_str());
        QCOMPARE(e.isDir, st.isDir);
        QCOMPARE(e.isReparse, false);
        QCOMPARE(e.attributes, st.attributes);
        QCOMPARE(e.mtimeMs, st.mtimeMs);
        QCOMPARE(e.birthMs, st.birthMs);
        QVERIFY(std::llabs(e.mtimeMs - now) < 120000);
        if (!e.isDir) {
            const auto it = std::find(files.begin(), files.end(), e.name);
            QVERIFY(it != files.end());
            QCOMPARE(e.size, static_cast<int64_t>((it - files.begin()) * 7));
            QCOMPARE(e.size, st.size);
            QCOMPARE(e.mtimeMs, QFileInfo(qs(p)).lastModified().toMSecsSinceEpoch());
        } else {
            QCOMPARE(e.size, int64_t(0));
        }
        QCOMPARE(e.isHidden, e.name == ".hidden");
        QCOMPARE(e.isSystem, e.name == "sys.dat");
    }

    // Node's readdirSync (NtQueryDirectoryFile) returns the same order.
    const QString node = findNode();
    if (!node.isEmpty()) {
        const std::string script = sub("readdir.js"), out = sub("readdir-out.txt");
        QVERIFY(writeBytes(script, "const fs=require('fs');fs.writeFileSync(process.argv[3],"
                                   "fs.readdirSync(process.argv[2]).map(n=>Buffer.from(n,'utf8').toString('hex')).join('\\n'))"));
        QVERIFY(runTool(node, {qs(script), qs(dir), qs(out)}));
        std::vector<std::string> nodeNames;
        for (const QByteArray& h : QByteArray::fromStdString(readBytes(out)).split('\n'))
            if (!h.isEmpty()) nodeNames.push_back(QByteArray::fromHex(h).toStdString());
        QVERIFY2(names == nodeNames, "listDir order differs from Node's readdirSync");
    }
}

void TestAnfsCoreFs::listDirErrorsAndEarlyStop()
{
    const std::string dir = sub("errors");
    QVERIFY(fs::makeDirs(dir + "\\empty"));
    for (int i = 0; i < 10; ++i) QVERIFY(writeBytes(dir + "\\f" + std::to_string(i), "1"));
    int calls = 0;
    QVERIFY(fs::listDir(dir, [&](const fs::DirEntry&) { return ++calls < 3; }));
    QCOMPARE(calls, 3);

    calls = 0;
    QVERIFY(fs::listDir(dir + "\\empty", [&](const fs::DirEntry&) { ++calls; return true; }));
    QCOMPARE(calls, 0);
    QVERIFY(fs::listDir(dir + "\\empty\\", [&](const fs::DirEntry&) { ++calls; return true; }));   // trailing separator
    QCOMPARE(calls, 0);

    uint32_t err = 0;
    QVERIFY(!fs::listDir(dir + "\\missing", [](const fs::DirEntry&) { return true; }, &err));
    QVERIFY2(err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND, qPrintable(QString::number(err)));
    QVERIFY(!fs::listDir(dir + "\\f1", [](const fs::DirEntry&) { return true; }, &err));
    QCOMPARE(err, static_cast<uint32_t>(ERROR_DIRECTORY));   // 267: not a directory
    QVERIFY(!fs::listDir("", [](const fs::DirEntry&) { return true; }, &err));
    QCOMPARE(err, static_cast<uint32_t>(ERROR_PATH_NOT_FOUND));
    QVERIFY(!fs::listDir(dir + std::string("\0x", 2), [](const fs::DirEntry&) { return true; }, &err));
    QCOMPARE(err, static_cast<uint32_t>(ERROR_INVALID_NAME));

    // A callback that throws leaves no find handle behind (the directory can be removed at once).
    bool threw = false;
    try {
        fs::listDir(dir, [](const fs::DirEntry&) -> bool { throw std::runtime_error("stop"); });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    QVERIFY(threw);
    QVERIFY(fs::removeTree(dir));
    QVERIFY(!fs::exists(dir));
}

void TestAnfsCoreFs::reparsePointsFlaggedNotFollowed()
{
    const std::string base = sub("reparse");
    const std::string target = base + "\\target";
    const std::string tree = base + "\\tree";
    QVERIFY(fs::makeDirs(target + "\\inner"));
    QVERIFY(fs::makeDirs(tree));
    QVERIFY(writeBytes(target + "\\keep.txt", "keep"));
    QVERIFY(writeBytes(target + "\\inner\\deep.txt", "deep"));
    QVERIFY(writeBytes(tree + "\\f.txt", "f"));
    auto junction = [](const std::string& link, const std::string& to) {
        return runTool(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                                                   QDir::toNativeSeparators(qs(link)), QDir::toNativeSeparators(qs(to))});
    };
    if (!junction(tree + "\\link", target)) QSKIP("mklink /J failed");

    bool seen = false;
    QVERIFY(fs::listDir(tree, [&](const fs::DirEntry& e) {
        if (e.name == "link") {
            seen = true;
            if (!e.isDir || !e.isReparse) qWarning("link entry: isDir %d isReparse %d", e.isDir, e.isReparse);
            seen = e.isDir && e.isReparse;
        }
        return true;
    }));
    QVERIFY2(seen, "junction not flagged as a reparse directory");
    const fs::FileStat st = fs::stat(tree + "\\link");
    QVERIFY(st.exists && st.isDir && st.isReparse);
    QVERIFY(fs::isFile(tree + "\\link\\keep.txt"));   // reachable through the link

    // removeTree removes the link, never what it points at.
    QVERIFY(fs::removeTree(tree));
    QVERIFY(!fs::exists(tree));
    QCOMPARE(readBytes(target + "\\keep.txt"), std::string("keep"));
    QCOMPARE(readBytes(target + "\\inner\\deep.txt"), std::string("deep"));

    // removeFile on a junction removes the link only; a real directory is refused.
    QVERIFY(junction(base + "\\link2", target));
    QVERIFY(!fs::removeFile(target));
    QVERIFY(fs::removeFile(base + "\\link2"));
    QVERIFY(!fs::exists(base + "\\link2"));
    QVERIFY(fs::isFile(target + "\\keep.txt"));

    // removeTree on the link itself.
    QVERIFY(junction(base + "\\link3", target));
    QVERIFY(fs::removeTree(base + "\\link3"));
    QVERIFY(!fs::exists(base + "\\link3"));
    QVERIFY(fs::isFile(target + "\\inner\\deep.txt"));

    // A file symlink when this account may create one (elevated or developer mode).
    if (runTool(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"),
                                            QDir::toNativeSeparators(qs(base + "\\flink")),
                                            QDir::toNativeSeparators(qs(target + "\\keep.txt"))})) {
        const fs::FileStat ls = fs::stat(base + "\\flink");
        QVERIFY(ls.exists && !ls.isDir && ls.isReparse);
        QVERIFY(fs::removeFile(base + "\\flink"));
        QVERIFY(fs::isFile(target + "\\keep.txt"));
    } else {
        qInfo("file symlink not created (no privilege): skipped that part");
    }
}

void TestAnfsCoreFs::statExistsAndLockedFiles()
{
    const std::string f = sub("stat.bin");
    QVERIFY(writeBytes(f, std::string(12345, 'q')));
    const fs::FileStat st = fs::stat(f);
    QVERIFY(st.exists && !st.isDir && !st.isReparse);
    QCOMPARE(st.size, int64_t(12345));
    const QFileInfo fi(qs(f));
    QCOMPARE(st.mtimeMs, fi.lastModified().toMSecsSinceEpoch());
    QCOMPARE(st.birthMs, fi.birthTime().toMSecsSinceEpoch());
    QVERIFY(st.atimeMs > 0);
    QVERIFY(fs::exists(f) && fs::isFile(f) && !fs::isDir(f));
    QVERIFY(fs::isDir(root_) && !fs::isFile(root_) && fs::exists(root_ + "\\"));
    QVERIFY(!fs::exists(sub("nope")) && !fs::isFile(sub("nope")) && !fs::isDir(sub("nope")));
    QVERIFY(!fs::exists(""));
    QVERIFY(!fs::exists(f + std::string("\0.x", 3)));   // an embedded NUL is refused, not cut off
    const fs::FileStat d = fs::stat(root_);
    QVERIFY(d.exists && d.isDir && d.size == 0);

    // A file another process opened without any sharing still stats (directory metadata, not the data).
    HANDLE h = CreateFileW(fs::widen(f).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    QVERIFY(fs::exists(f));
    QCOMPARE(fs::stat(f).size, int64_t(12345));
    CloseHandle(h);

    // pagefile.sys answers ERROR_SHARING_VIOLATION to GetFileAttributesExW: the directory entry is read instead.
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const std::wstring pf = L"C:\\pagefile.sys";
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pf.c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        FindClose(fh);
        if (!GetFileAttributesExW(pf.c_str(), GetFileExInfoStandard, &a) && GetLastError() == ERROR_SHARING_VIOLATION) {
            const fs::FileStat p = fs::stat("C:\\pagefile.sys");
            QVERIFY(p.exists && !p.isDir);
            QVERIFY(p.size > 0);
            QVERIFY(fs::isFile("C:\\pagefile.sys"));
        } else {
            qInfo("pagefile.sys is not locked here");
        }
    } else {
        qInfo("no C:\\pagefile.sys: locked-file case skipped");
    }
}

void TestAnfsCoreFs::makeDirsRemoveRename()
{
    const std::string base = sub("ops");
    std::string err;
    QVERIFY(fs::makeDirs(base + "\\a\\b\\c", &err));
    QVERIFY(fs::isDir(base + "\\a\\b\\c"));
    QVERIFY(fs::makeDirs(base + "\\a\\b\\c", &err));   // existing: fine
    QVERIFY(fs::makeDirs(base + "/a/x/", &err));      // forward slashes and a trailing separator
    QVERIFY(fs::isDir(base + "\\a\\x"));
    QVERIFY(fs::makeDirs("C:\\", &err));              // a protected existing root
    QVERIFY(writeBytes(base + "\\file", "f"));
    QVERIFY(!fs::makeDirs(base + "\\file", &err));
    SEQ(err, "EEXIST: file already exists, mkdir '" + base + "\\file'");
    QVERIFY(!fs::makeDirs(base + "\\file\\x\\y", &err));
    SEQ(err, "ENOTDIR: not a directory, mkdir '" + base + "\\file\\x\\y'");
    const std::string q = freeDriveLetter();
    if (!q.empty()) {
        QVERIFY(!fs::makeDirs(q + "\\x\\y", &err));
        QVERIFY2(err.rfind("mkdir '" + q + "\\x\\y'") != std::string::npos, err.c_str());
        QVERIFY2(err.rfind("ENOENT", 0) == 0 || err.rfind("EPERM", 0) == 0, err.c_str());
    }

    // removeFile: read-only files go (libuv clears the attribute too), directories and missing paths do not.
    const std::string ro = base + "\\ro.txt";
    QVERIFY(writeBytes(ro, "r"));
    QVERIFY(SetFileAttributesW(fs::widen(ro).c_str(), FILE_ATTRIBUTE_READONLY));
    QVERIFY(fs::removeFile(ro));
    QVERIFY(!fs::exists(ro));
    QVERIFY(!fs::removeFile(base + "\\a"));
    QVERIFY(fs::isDir(base + "\\a"));
    QVERIFY(!fs::removeFile(base + "\\missing"));

    // POSIX delete: the name is free at once although another handle (sharing delete) is still open.
    const std::string held = base + "\\held.txt";
    QVERIFY(writeBytes(held, "old"));
    HANDLE h = CreateFileW(fs::widen(held).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    QVERIFY(fs::removeFile(held));
    QVERIFY(!fs::exists(held));
    QVERIFY(writeBytes(held, "new"));
    CloseHandle(h);
    QCOMPARE(readBytes(held), std::string("new"));

    // renameFile
    const std::string from = base + "\\from.txt", to = base + "\\to.txt";
    QVERIFY(writeBytes(from, "1"));
    QVERIFY(fs::renameFile(from, to, false, &err));
    QVERIFY(!fs::exists(from) && readBytes(to) == "1");
    QVERIFY(writeBytes(from, "2"));
    QVERIFY(!fs::renameFile(from, to, false, &err));
    SEQ(err, "EEXIST: file already exists, rename '" + from + "' -> '" + to + "'");
    QVERIFY(fs::renameFile(from, to, true, &err));
    QCOMPARE(readBytes(to), std::string("2"));
    QVERIFY(!fs::renameFile(base + "\\nope", to, true, &err));
    SEQ(err, "ENOENT: no such file or directory, rename '" + base + "\\nope' -> '" + to + "'");
    QVERIFY(fs::renameFile(base + "\\a\\x", base + "\\a\\y", false, &err));   // directories too
    QVERIFY(fs::isDir(base + "\\a\\y"));

    // removeTree: read-only and hidden entries inside, nested dirs; a missing tree is already gone; roots refused.
    QVERIFY(writeBytes(base + "\\a\\b\\c\\ro.txt", "r"));
    SetFileAttributesW(fs::widen(base + "\\a\\b\\c\\ro.txt").c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN);
    QVERIFY(fs::removeTree(base + "\\a"));
    QVERIFY(!fs::exists(base + "\\a"));
    QVERIFY(fs::removeTree(base + "\\never-was"));
    QVERIFY(fs::removeTree(base));
    QVERIFY(!fs::exists(base));

    // removeTree refuses a volume root. Never tried on a real drive: a free letter is mapped onto a sandbox folder
    // for this process's logon session (DefineDosDevice, removed at once), so a broken guard could only empty it.
    // Every spelling Win32 resolves to that root is tried (the review found "Q: ", "Q:..." and the \\?\ forms slipped
    // past a string-only check), plus device paths without a drive form, which are refused whole. The \\?\UNC\ share
    // form goes through the same rule but is not exercised: that would need a real share.
    const std::string sandbox = sub("rootguard");
    QVERIFY(fs::makeDirs(sandbox + "\\keep"));
    QVERIFY(writeBytes(sandbox + "\\keep\\canary.txt", "c"));
    const std::string letter = freeDriveLetter();
    if (letter.empty()) QSKIP("no free drive letter for the root guard check");
    if (!DefineDosDeviceW(0, fs::widen(letter).c_str(), fs::widen(sandbox).c_str()))
        QSKIP("DefineDosDevice failed: root guard not exercised");
    const std::vector<std::string> spellings = {
        letter + "\\", letter, letter + " ", letter + "...", letter + ". ", letter + "\\.", letter + "\\keep\\..",
        "\\\\?\\" + letter + "\\", "\\\\?\\" + letter, "//?/" + letter + "/", "\\\\.\\" + letter + "\\",
        "\\??\\" + letter + "\\", "\\\\?\\GLOBALROOT\\??\\" + letter + "\\", "\\\\?\\GLOBALROOT\\??\\" + letter + "\\keep"};
    struct Probe {
        std::string spelling;
        bool existed;
        bool removed;
    };
    std::vector<Probe> probes;
    for (const std::string& s : spellings) probes.push_back({s, fs::exists(s), fs::removeTree(s)});
    const bool canary = fs::isFile(letter + "\\keep\\canary.txt");
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, fs::widen(letter).c_str(),
                     fs::widen(sandbox).c_str());
    for (const Probe& p : probes) {
        if (p.existed) QVERIFY2(!p.removed, p.spelling.c_str());
        else qInfo("root spelling %s does not resolve here", p.spelling.c_str());
    }
    QVERIFY(canary);
    QVERIFY(fs::isFile(sandbox + "\\keep\\canary.txt"));
}

void TestAnfsCoreFs::makeDirsOddNamesNeverHang()
{
    // Each of these looped forever before the ANFS-F2a review fix: the child failed with PATH_NOT_FOUND right after
    // its parent was made or found, and the walk went up and down again. A middle name ending in a space or made of
    // dots is kept by Win32 but trimmed as the last name, so the parent made is not the one the child needs. Now
    // ENOENT, the error C#'s Directory.CreateDirectory gives (Node creates 'Photos ' and '...' literally via \\?\).
    const std::string base = sub("mkodd");
    QVERIFY(fs::makeDirs(base));
    auto expectEnoent = [](const std::string& p, std::string* why) {
        std::string err;
        bool finished = false;
        const bool ok = makeDirsWithin(p, &err, &finished);
        const std::string want = "ENOENT: no such file or directory, mkdir '" + p + "'";
        if (!finished) *why = "makeDirs did not return within 5 s: " + p;
        else if (ok) *why = "makeDirs succeeded: " + p;
        else if (err != want) *why = "got \"" + err + "\", want \"" + want + "\"";
        else return true;
        return false;
    };
    std::string why;
    for (const std::string& p : {base + "\\Photos \\2024", base + "\\Photos \\2024\\Jan", base + "\\x\\...\\y",
                                 base + "\\x\\...\\y\\z"})
        QVERIFY2(expectEnoent(p, &why), why.c_str());

    // A junction whose target is gone: ENOENT for the link and below it, as Node (its stat follows the link).
    const std::string gone = base + "\\gone", dangling = base + "\\dangling";
    QVERIFY(fs::makeDirs(gone));
    if (!makeJunction(dangling, gone)) QSKIP("mklink /J failed");
    QVERIFY(RemoveDirectoryW(fs::widen(gone).c_str()));
    QVERIFY(fs::stat(dangling).isReparse);
    for (const std::string& p : {dangling + "\\sub\\deeper", dangling + "\\sub", dangling})
        QVERIFY2(expectEnoent(p, &why), why.c_str());

    // With the target back the link is a directory again, and folders below it are made in the target.
    QVERIFY(fs::makeDirs(gone));
    std::string err;
    QVERIFY2(fs::makeDirs(dangling, &err), err.c_str());
    QVERIFY2(fs::makeDirs(dangling + "\\made\\below", &err), err.c_str());
    QVERIFY(fs::isDir(gone + "\\made\\below"));
    QVERIFY(fs::removeTree(base));
    QVERIFY(!rawExists(base));
}

void TestAnfsCoreFs::removeTreeExactNames()
{
    // Names Win32 would trim or map, made through \\?\ as Node makes them: removeTree deletes each one exactly and
    // leaves the sibling 'report' outside alone. Before the ANFS-F2a review fix it opened 'report' for 'report ',
    // and returned false with 'report ' left behind.
    const std::string base = sub("oddnames");
    const std::string tree = base + "\\tree";
    QVERIFY(fs::makeDirs(tree));
    QVERIFY(writeBytes(base + "\\report", "sibling"));
    QVERIFY(writeBytes(tree + "\\report", "inside"));
    QVERIFY(makeExactFile(tree + "\\report "));
    QVERIFY(makeExactFile(tree + "\\a."));
    QVERIFY(makeExactFile(tree + "\\nul"));
    QVERIFY(makeExactFile(tree + "\\con.txt"));
    QVERIFY(makeExactDir(tree + "\\..."));
    QVERIFY(makeExactFile(tree + "\\...\\in.txt"));
    QVERIFY(makeExactDir(tree + "\\sub "));
    QVERIFY(makeExactFile(tree + "\\sub \\deep. "));
    std::vector<std::string> names;
    QVERIFY(fs::listDir(tree, [&](const fs::DirEntry& e) { names.push_back(e.name); return true; }));
    QCOMPARE(int(names.size()), 7);
    QVERIFY(fs::removeTree(tree));
    QVERIFY(!rawExists(tree));
    QCOMPARE(readBytes(base + "\\report"), std::string("sibling"));

    // The tree's own name keeps Win32 semantics (C#'s GetFullPath): "tree2 " is "tree2".
    QVERIFY(fs::makeDirs(base + "\\tree2\\x"));
    QVERIFY(fs::removeTree(base + "\\tree2 "));
    QVERIFY(!rawExists(base + "\\tree2"));

    // A junction given with a trailing separator: the link goes, the target and its files stay.
    const std::string target = base + "\\target";
    QVERIFY(fs::makeDirs(target));
    QVERIFY(writeBytes(target + "\\canary.txt", "c"));
    if (makeJunction(base + "\\link", target)) {
        QVERIFY(fs::removeTree(base + "\\link\\"));
        QVERIFY(!rawExists(base + "\\link"));
        QCOMPARE(readBytes(target + "\\canary.txt"), std::string("c"));
    } else {
        qInfo("mklink /J failed: trailing-separator junction case skipped");
    }
    QVERIFY(fs::removeTree(base));
    QVERIFY(!rawExists(base));
}

void TestAnfsCoreFs::readerWriterAndReadFile()
{
    const std::string base = sub("io");
    QVERIFY(fs::makeDirs(base));
    std::string data;
    for (int i = 0; i < 200000; ++i) data.push_back(static_cast<char>('a' + i % 26));

    // FileWriter: create + commit.
    const std::string f = base + "\\w.bin";
    {
        fs::FileWriter w;
        std::string err;
        QVERIFY(w.create(f, &err));
        w.write(data.data(), 1000);
        w.write(std::string_view(data).substr(1000));
        w.flush();
        QCOMPARE(w.written(), static_cast<int64_t>(data.size()));
        SEQ(w.path(), f);
        QVERIFY(w.commit());
        bool threw = false;
        try { w.write("x", 1); } catch (const IoError&) { threw = true; }
        QVERIFY(threw);
    }
    QCOMPARE(readBytes(f), data);

    // discard and the destructor delete a created file; create replaces an existing one.
    {
        fs::FileWriter w;
        QVERIFY(w.create(base + "\\gone.bin"));
        w.write("partial", 7);
        w.discard();
    }
    QVERIFY(!fs::exists(base + "\\gone.bin"));
    {
        fs::FileWriter w;
        QVERIFY(w.create(base + "\\gone2.bin"));
        w.write("partial", 7);
    }   // destructor: neither commit() nor close()
    QVERIFY(!fs::exists(base + "\\gone2.bin"));
    {
        fs::FileWriter w;
        QVERIFY(w.create(f));
        w.write("short", 5);
        w.close();
    }
    QCOMPARE(readBytes(f), std::string("short"));
    // A hidden file is overwritten (CREATE_ALWAYS alone refuses it).
    QVERIFY(SetFileAttributesW(fs::widen(f).c_str(), FILE_ATTRIBUTE_HIDDEN));
    {
        fs::FileWriter w;
        std::string err;
        QVERIFY2(w.create(f, &err), err.c_str());
        w.write("hidden", 6);
        QVERIFY(w.commit());
    }
    QCOMPARE(readBytes(f), std::string("hidden"));
    SetFileAttributesW(fs::widen(f).c_str(), FILE_ATTRIBUTE_NORMAL);
    {
        fs::FileWriter w;
        std::string err;
        QVERIFY(!w.create(base + "\\no\\such\\dir\\x", &err));
        SEQ(err, "ENOENT: no such file or directory, open '" + base + "\\no\\such\\dir\\x'");
        QVERIFY(!w.create(base, &err));
        SEQ(err, "EISDIR: illegal operation on a directory, open '" + base + "'");
    }

    // openAppend: appends; discard cuts back to the size at open, or deletes a file it created.
    const std::string ap = base + "\\append.txt";
    QVERIFY(writeBytes(ap, "abc"));
    {
        fs::FileWriter w;
        QVERIFY(w.openAppend(ap));
        w.write("def", 3);
        QVERIFY(w.commit());
    }
    QCOMPARE(readBytes(ap), std::string("abcdef"));
    {
        fs::FileWriter w;
        QVERIFY(w.openAppend(ap));
        w.write("XYZ", 3);
        w.discard();
    }
    QCOMPARE(readBytes(ap), std::string("abcdef"));
    {
        fs::FileWriter w;
        QVERIFY(w.openAppend(ap));
        w.write("123", 3);
    }   // destructor discards as well
    QCOMPARE(readBytes(ap), std::string("abcdef"));
    {
        fs::FileWriter w;
        QVERIFY(w.openAppend(base + "\\fresh.txt"));
        w.write("x", 1);
        w.discard();
    }
    QVERIFY(!fs::exists(base + "\\fresh.txt"));

    // FileReader
    QVERIFY(writeBytes(f, data));
    {
        fs::FileReader r;
        std::string err;
        QVERIFY(r.open(f, &err));
        QVERIFY(r.isOpen());
        QCOMPARE(r.size(), static_cast<int64_t>(data.size()));
        QCOMPARE(r.length(), static_cast<int64_t>(data.size()));
        QVERIFY(r.nativeHandle() != nullptr);
        StringSink sink;
        QCOMPARE(copyStream(r, sink), static_cast<int64_t>(data.size()));
        QCOMPARE(sink.data, data);
        char buf[10];
        QCOMPARE(r.read(buf, sizeof buf), size_t(0));   // EOF
        QVERIFY(r.seek(5));
        QCOMPARE(r.read(buf, 3), size_t(3));
        QCOMPARE(std::string(buf, 3), data.substr(5, 3));
        QVERIFY(!r.seek(-1));
        QVERIFY(r.seek(static_cast<int64_t>(data.size()) + 100));
        QCOMPARE(r.read(buf, sizeof buf), size_t(0));
        // Shared for delete: the file can go while the reader is open.
        QVERIFY(fs::removeFile(f));
        QVERIFY(!fs::exists(f));
        r.close();
        QVERIFY(!r.isOpen());
        bool threw = false;
        try { r.read(buf, 1); } catch (const IoError&) { threw = true; }
        QVERIFY(threw);
        QVERIFY(!r.open(f, &err));
        SEQ(err, "ENOENT: no such file or directory, open '" + f + "'");
        QVERIFY(!r.open(base, &err));
        SEQ(err, "EISDIR: illegal operation on a directory, read");
    }

    // readFile
    QVERIFY(writeBytes(f, data));
    std::string got, err;
    QVERIFY(fs::readFile(f, got, 64LL << 20, &err));
    QCOMPARE(got, data);
    QVERIFY(!fs::readFile(f, got, 1000, &err));
    QVERIFY(got.empty());
    QVERIFY2(err.rfind("file too large", 0) == 0, err.c_str());
    QVERIFY(!fs::readFile(base + "\\none", got, 1000, &err));
    SEQ(err, "ENOENT: no such file or directory, open '" + base + "\\none'");
    QVERIFY(!fs::readFile(base, got, 1000, &err));
    SEQ(err, "EISDIR: illegal operation on a directory, read");
    QVERIFY(writeBytes(base + "\\empty", ""));
    QVERIFY(fs::readFile(base + "\\empty", got));
    QVERIFY(got.empty());
}

void TestAnfsCoreFs::longAndDeepPaths()
{
    // Over MAX_PATH without the long-path opt-in: fsutil adds \\?\ itself.
    std::string dir = sub("long");
    const std::string top = dir;
    for (int i = 0; i < 22; ++i) dir += "\\segment_0123456789";
    QVERIFY(dir.size() > 300);
    std::string err;
    QVERIFY2(fs::makeDirs(dir, &err), err.c_str());
    QVERIFY(fs::isDir(dir));
    QVERIFY(rawExists(dir));
    const std::string file = dir + "\\file.txt";
    {
        fs::FileWriter w;
        QVERIFY2(w.create(file, &err), err.c_str());
        w.write("long", 4);
        QVERIFY(w.commit());
    }
    QCOMPARE(fs::stat(file).size, int64_t(4));
    std::string got;
    QVERIFY(fs::readFile(file, got));
    QCOMPARE(got, std::string("long"));
    std::vector<std::string> names;
    QVERIFY(fs::listDir(dir, [&](const fs::DirEntry& e) { names.push_back(e.name); return true; }));
    QVERIFY(names == std::vector<std::string>{"file.txt"});
    QVERIFY(fs::renameFile(file, dir + "\\file2.txt", false, &err));
    QVERIFY(fs::exists(dir + "\\file2.txt"));
    SEQ(nodepath::conflictName(dir + "\\file2.txt"), dir + "\\file2 (1).txt");
    QVERIFY(fs::removeFile(dir + "\\file2.txt"));
    QVERIFY(fs::removeTree(top));
    QVERIFY(!rawExists(top));

    // 300 levels: makeDirs walks up without recursion, removeTree walks down on an explicit stack.
    std::string deep = sub("deep");
    const std::string deepTop = deep;
    for (int i = 0; i < 300; ++i) deep += "\\d";
    QVERIFY2(fs::makeDirs(deep, &err), err.c_str());
    {
        fs::FileWriter w;
        QVERIFY2(w.create(deep + "\\leaf.txt", &err), err.c_str());
        w.write("x", 1);
        QVERIFY(w.commit());
    }
    QVERIFY(rawExists(deep + "\\leaf.txt"));
    QVERIFY(fs::removeTree(deepTop));
    QVERIFY(!rawExists(deepTop));
}

void TestAnfsCoreFs::namesAndFolders()
{
    SEQ(fs::fullPath("C:\\a\\..\\b\\.\\c"), "C:\\b\\c");
    SEQ(fs::fullPath("C:/x//y"), "C:\\x\\y");
    SEQ(fs::fullPath(std::string("C:\\a\0b", 6)), "");
    SEQ(fs::fullPath(""), "");
    SEQ(fs::fullPath("rel"), fs::cwd() + (fs::cwd().back() == '\\' ? "" : "\\") + "rel");

    // 8.3 names: expanded, also below a folder that does not exist yet.
    const std::string longDir = sub("Long Folder Name 1234");
    QVERIFY(fs::makeDirs(longDir));
    wchar_t shortBuf[MAX_PATH];
    const DWORD n = GetShortPathNameW(fs::widen(longDir).c_str(), shortBuf, MAX_PATH);
    const std::string shortDir = n ? fs::narrow(std::wstring(shortBuf, n)) : std::string();
    if (!shortDir.empty() && shortDir != longDir) {
        SEQ(fs::longPath(shortDir), longDir);
        SEQ(fs::longPath(shortDir + "\\missing\\x.txt"), longDir + "\\missing\\x.txt");
    } else {
        qInfo("8.3 creation is off for the temp folder: using an existing short name instead");
    }
    // Folders made before 8.3 creation was switched off keep their short names (C:\PROGRA~1).
    const std::wstring pf = L"C:\\Program Files";
    const DWORD pn = GetShortPathNameW(pf.c_str(), shortBuf, MAX_PATH);
    const std::string pfShort = pn ? fs::narrow(std::wstring(shortBuf, pn)) : std::string();
    if (!pfShort.empty() && pfShort != "C:\\Program Files") {
        SEQ(fs::longPath(pfShort), "C:\\Program Files");
        SEQ(fs::longPath(pfShort + "\\anfs-missing\\deeper\\x.txt"), "C:\\Program Files\\anfs-missing\\deeper\\x.txt");
        // The expanded component comes back as stored; the drive letter as given.
        SEQ(fs::longPath(nodepath::toLowerInvariant(pfShort) + "\\x"), "c:\\Program Files\\x");
    } else if (shortDir.empty() || shortDir == longDir) {
        qInfo("no 8.3 names at all on this machine: longPath expansion not exercised");
    }
    SEQ(fs::longPath(longDir), longDir);
    SEQ(fs::longPath("Q:\\does\\not\\exist"), "Q:\\does\\not\\exist");

    QVERIFY(fs::isDevicePath("\\\\?\\C:\\x"));
    QVERIFY(fs::isDevicePath("\\\\.\\PhysicalDrive0"));
    QVERIFY(fs::isDevicePath("//?/C:/x"));
    QVERIFY(fs::isDevicePath("//./pipe/x"));
    QVERIFY(fs::isDevicePath("\\??\\C:\\x"));
    QVERIFY(!fs::isDevicePath("\\\\srv\\share"));
    QVERIFY(!fs::isDevicePath("C:\\x"));
    QVERIFY(!fs::isDevicePath("\\\\?"));
    QVERIFY(fs::hasAlternateStream("C:\\a.txt:hidden"));
    QVERIFY(fs::hasAlternateStream("C:\\a.txt::$DATA"));
    QVERIFY(fs::hasAlternateStream("a::$DATA"));
    QVERIFY(fs::hasAlternateStream("\\\\srv\\s\\f:x"));
    QVERIFY(!fs::hasAlternateStream("C:\\a.txt"));
    QVERIFY(!fs::hasAlternateStream("C:"));
    QVERIFY(!fs::hasAlternateStream("d:\\x\\y"));

    SEQ(fs::exeDir(), u8(QDir::toNativeSeparators(QCoreApplication::applicationDirPath())));
    const std::string t = fs::tempDir();
    QVERIFY(!t.empty() && t.back() != '\\');
    QVERIFY(fs::isDir(t));
    wchar_t tb[MAX_PATH + 2];
    const DWORD tn = GetTempPathW(MAX_PATH + 2, tb);
    std::wstring tw(tb, tn);
    while (tw.size() > 3 && tw.back() == L'\\') tw.pop_back();
    SEQ(t, fs::narrow(tw));
    QCOMPARE(qs(fs::knownFolder("PROFILE")).toLower(), qEnvironmentVariable("USERPROFILE").toLower());
    QCOMPARE(qs(fs::knownFolder("programdata")).toLower(), qEnvironmentVariable("ProgramData").toLower());
    QVERIFY(fs::isDir(fs::knownFolder("desktop")));
    QVERIFY(fs::isDir(fs::knownFolder("documents")));
    QVERIFY(fs::isDir(fs::knownFolder("downloads")));
    SEQ(fs::knownFolder("nope"), "");
}

void TestAnfsCoreFs::nodeErrorTexts()
{
    SEQ(fs::nodeError(ERROR_FILE_NOT_FOUND, "open", "C:\\x"), "ENOENT: no such file or directory, open 'C:\\x'");
    SEQ(fs::nodeError(ERROR_PATH_NOT_FOUND, "scandir", "C:\\y"), "ENOENT: no such file or directory, scandir 'C:\\y'");
    SEQ(fs::nodeError(ERROR_ACCESS_DENIED, "scandir", "C:\\System Volume Information"),
        "EPERM: operation not permitted, scandir 'C:\\System Volume Information'");
    SEQ(fs::nodeError(ERROR_SHARING_VIOLATION, "unlink", "p"), "EBUSY: resource busy or locked, unlink 'p'");
    SEQ(fs::nodeError(ERROR_DIR_NOT_EMPTY, "rmdir", "d"), "ENOTEMPTY: directory not empty, rmdir 'd'");
    SEQ(fs::nodeError(ERROR_DISK_FULL, "write"), "ENOSPC: no space left on device, write");
    SEQ(fs::nodeError(ERROR_NOT_SAME_DEVICE, "rename", "a"), "EXDEV: cross-device link not permitted, rename 'a'");
    SEQ(fs::nodeError(ERROR_BAD_NETPATH, "open", "\\\\srv\\x"), "UNKNOWN: unknown error, open '\\\\srv\\x'");
    SEQ(fs::nodeError(ERROR_DIRECTORY, "scandir", "C:\\f.txt"), "ENOTDIR: not a directory, scandir 'C:\\f.txt'");
    SEQ(fs::nodeError(ERROR_DIRECTORY, "open", "C:\\f.txt"), "ENOENT: no such file or directory, open 'C:\\f.txt'");
    QCOMPARE(QByteArray(fs::uvCode(ERROR_ALREADY_EXISTS)), QByteArray("EEXIST"));
    QCOMPARE(QByteArray(fs::uvCode(ERROR_INVALID_NAME)), QByteArray("ENOENT"));
    QCOMPARE(QByteArray(fs::uvCode(ERROR_FILENAME_EXCED_RANGE)), QByteArray("ENAMETOOLONG"));
    QCOMPARE(QByteArray(fs::uvCode(12345)), QByteArray("UNKNOWN"));

    // The same failures through Node's fs: the e.message texts agent.js answers with must match.
    const QString node = findNode();
    if (node.isEmpty()) QSKIP("node.exe not found: the literal texts above still ran");
    const std::string base = sub("errtext");
    QVERIFY(fs::makeDirs(base + "\\dir"));
    const std::string file = base + "\\file.txt", dir = base + "\\dir", missing = base + "\\missing";
    QVERIFY(writeBytes(file, "f"));
    struct Case {
        const char* op;
        std::string a, b, ours;
    };
    std::vector<Case> cases;
    auto scandir = [](const std::string& p) {
        uint32_t e = 0;
        fs::listDir(p, [](const fs::DirEntry&) { return true; }, &e);
        return fs::nodeError(e, "scandir", p);
    };
    std::string err, sink;
    cases.push_back({"readdir", file, "", scandir(file)});
    cases.push_back({"readdir", missing, "", scandir(missing)});
    fs::readFile(dir, sink, 1000, &err);
    cases.push_back({"readFile", dir, "", err});
    fs::readFile(missing, sink, 1000, &err);
    cases.push_back({"readFile", missing, "", err});
    fs::makeDirs(file + "\\x\\y", &err);
    cases.push_back({"mkdirp", file + "\\x\\y", "", err});
    fs::makeDirs(file, &err);
    cases.push_back({"mkdirp", file, "", err});
    fs::renameFile(missing, base + "\\to", true, &err);
    cases.push_back({"rename", missing, base + "\\to", err});
    {
        fs::FileWriter w;
        w.create(dir, &err);
    }
    cases.push_back({"openw", dir, "", err});
    {
        fs::FileReader r;
        r.open(missing, &err);
    }
    cases.push_back({"openr", missing, "", err});

    JVal in = JVal::array();
    for (const Case& c : cases) in.push(JVal::array({c.op, c.a, c.b}));
    const std::string script = sub("errtext.js"), inPath = sub("errtext-in.json"), outPath = sub("errtext-out.json");
    QVERIFY(writeBytes(script, "const fs=require('fs');const [,,i,o]=process.argv;"
                               "fs.writeFileSync(o,JSON.stringify(JSON.parse(fs.readFileSync(i,'utf8')).map(([op,a,b])=>{try{"
                               "if(op==='readdir')fs.readdirSync(a);else if(op==='readFile')fs.readFileSync(a);"
                               "else if(op==='mkdirp')fs.mkdirSync(a,{recursive:true});else if(op==='rename')fs.renameSync(a,b);"
                               "else if(op==='openw')fs.closeSync(fs.openSync(a,'w'));else if(op==='openr')fs.closeSync(fs.openSync(a,'r'));"
                               "return '';}catch(e){return e.message;}})))"));
    QVERIFY(writeBytes(inPath, json::stringify(in)));
    QVERIFY(runTool(node, {qs(script), qs(inPath), qs(outPath)}));
    const JVal want = json::parse(readBytes(outPath));
    QCOMPARE(static_cast<size_t>(want.size()), cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
        const std::string w = want.at(i).asString();
        if (w != cases[i].ours) qWarning("%s %s: ours \"%s\", node \"%s\"", cases[i].op, cases[i].a.c_str(),
                                         cases[i].ours.c_str(), w.c_str());
        SEQ(cases[i].ours, w);
    }
}

void TestAnfsCoreFs::drivesAndElevation()
{
    const std::vector<fs::DriveInfo> list = fs::drives();
    const DWORD mask = GetLogicalDrives();
    std::string letters, want;
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) want += static_cast<char>('A' + i);
    for (const auto& d : list) {
        QCOMPARE(d.letter.size(), size_t(2));
        QCOMPARE(d.letter[1], ':');
        letters += d.letter[0];
        if (d.ready) QVERIFY(d.size > 0 && d.free >= 0 && d.free <= d.size);
        else QVERIFY(d.size == 0 && d.free == 0);
    }
    SEQ(letters, want);   // GetLogicalDrives order, A..Z, not-ready drives included
    const std::string sys = u8(qEnvironmentVariable("SystemDrive", QStringLiteral("C:"))).substr(0, 2);
    const auto it = std::find_if(list.begin(), list.end(), [&](const fs::DriveInfo& d) {
        return nodepath::toLowerInvariant(d.letter) == nodepath::toLowerInvariant(sys);
    });
    QVERIFY(it != list.end());
    QVERIFY(it->ready);
    QCOMPARE(it->type, static_cast<int>(DRIVE_FIXED));
    QVERIFY(!it->fileSystem.empty());
    const QStorageInfo si(qs(sys + "\\"));
    QCOMPARE(it->size, static_cast<int64_t>(si.bytesTotal()));
    QVERIFY(std::llabs(it->free - static_cast<int64_t>(si.bytesAvailable())) < (1LL << 30));   // the disk is in use
    qInfo("drives: %s; %s %s label \"%s\"", letters.c_str(), it->letter.c_str(), it->fileSystem.c_str(), it->label.c_str());

    // Elevation: the C# rule through .NET Framework itself.
    const bool elevated = fs::isElevated();
    QCOMPARE(fs::isElevated(), elevated);   // cached, stable
    const QString ps = findPowerShell51();
    if (ps.isEmpty()) QSKIP("powershell.exe 5.1 not found");
    QByteArray out;
    if (!runTool(ps, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-Command"),
                      QStringLiteral("$i=[Security.Principal.WindowsIdentity]::GetCurrent();"
                                     "$i.IsSystem -or ([Security.Principal.WindowsPrincipal]$i).IsInRole("
                                     "[Security.Principal.WindowsBuiltInRole]::Administrator)")},
                 &out))
        QSKIP("powershell.exe did not run");
    QCOMPARE(elevated, out.trimmed() == "True");
    qInfo("isElevated: %s (powershell agrees)", elevated ? "true" : "false");
}

// ---- mime ---------------------------------------------------------------------------------------------------------

void TestAnfsCoreFs::mimeVectors()
{
    // Produced by mime-types 2.1.35 (lookup) and Express send / mime 1.6.0 (lookup + charsets).
    SEQ(mime::forExtension("mp4"), "video/mp4");
    SEQ(mime::forExtension(".MP4"), "video/mp4");
    SEQ(mime::forExtension("mpg"), "video/mpeg");
    SEQ(mime::forExtension("ts"), "video/mp2t");
    SEQ(mime::forExtension("wav"), "audio/wave");
    SEQ(mime::forExtension("mov"), "video/quicktime");
    SEQ(mime::forExtension("mkv"), "video/x-matroska");
    SEQ(mime::forExtension("mxf"), "application/mxf");
    SEQ(mime::forExtension("json"), "application/json");
    SEQ(mime::forExtension("js"), "application/javascript");
    SEQ(mime::forExtension("html"), "text/html");
    SEQ(mime::forExtension("bashrc"), "");
    SEQ(mime::forExtension(""), "");
    SEQ(mime::forExtension("tar.gz"), "application/gzip");
    SEQ(mime::forExtension("C:\\dir\\a.png"), "image/png");
    SEQ(mime::forExtension("x"), "");
    SEQ(mime::forExtension("m\xE2\x84\xAA""v"), "video/x-matroska");   // KELVIN SIGN lower-cases to 'k' in JS
    SEQ(mime::forExtension("mp3"), "audio/mpeg");
    SEQ(mime::forExtension("jpg"), "image/jpeg");
    SEQ(mime::forExtension("webp"), "image/webp");
    SEQ(mime::forExtension("avif"), "image/avif");
    SEQ(mime::forExtension("srt"), "application/x-subrip");
    SEQ(mime::forExtension("vtt"), "text/vtt");
    SEQ(mime::forExtension("m3u8"), "application/vnd.apple.mpegurl");
    SEQ(mime::forExtension("exe"), "application/x-msdos-program");
    SEQ(mime::forExtension("."), "");
    SEQ(mime::forExtension("..png"), "image/png");
    SEQ(mime::forPath("C:\\v\\clip.MOV"), "video/quicktime");
    SEQ(mime::forPath("C:\\u\\.bashrc"), "application/octet-stream");
    SEQ(mime::forPath("C:\\u\\json"), "application/octet-stream");
    SEQ(mime::forPath("noext"), "application/octet-stream");
    SEQ(mime::forPath("C:\\a.b\\c"), "application/octet-stream");
    SEQ(mime::forPath("D:\\media\\x.mpg"), "video/mpeg");
    SEQ(mime::forPath("noext", ""), "");
    SEQ(mime::contentTypeForFile("C:\\ui\\index.html"), "text/html; charset=UTF-8");
    SEQ(mime::contentTypeForFile("app.js"), "application/javascript; charset=UTF-8");
    SEQ(mime::contentTypeForFile("a.json"), "application/json; charset=UTF-8");
    SEQ(mime::contentTypeForFile("a.wav"), "audio/wav");
    SEQ(mime::contentTypeForFile("a.exe"), "application/octet-stream");
    SEQ(mime::contentTypeForFile("a.avif"), "application/octet-stream");
    SEQ(mime::contentTypeForFile("C:\\dir\\json"), "application/json; charset=UTF-8");   // mime 1.x: no dot needed
    SEQ(mime::contentTypeForFile("C:\\u\\.bashrc"), "application/octet-stream");
    SEQ(mime::contentTypeForFile("a.css"), "text/css; charset=UTF-8");
    SEQ(mime::contentTypeForFile("a.svg"), "image/svg+xml");
    SEQ(mime::contentTypeForFile("a.mp4"), "video/mp4");
    SEQ(mime::contentTypeForFile("a.txt"), "text/plain; charset=UTF-8");
    SEQ(mime::contentTypeForFile("x.map"), "application/json; charset=UTF-8");
    SEQ(mime::contentTypeForFile("C:\\a.b\\c"), "text/x-c; charset=UTF-8");
    SEQ(mime::contentTypeForFile("C:/x/y.ICO"), "image/x-icon");
    SEQ(mime::contentTypeForFile("README"), "application/octet-stream");
    SEQ(mime::contentTypeForFile("a.jsonld"), "application/ld+json");
    SEQ(mime::contentTypeForFile("a.mjs"), "application/javascript; charset=UTF-8");
    SEQ(mime::contentTypeForFile("a.m3u8"), "application/vnd.apple.mpegurl");
    SEQ(mime::contentTypeForFile("a.unknownext", "text/plain"), "text/plain; charset=UTF-8");
    SEQ(mime::contentTypeForFile("a.unknownext", ""), "");
    QCOMPARE(QByteArray(mime::kDefault), QByteArray("application/octet-stream"));
}

void TestAnfsCoreFs::mimeMatchesPackages()
{
    const QString node = findNode();
    if (node.isEmpty()) QSKIP("node.exe not found");
    QString modules = qEnvironmentVariable("ANFS_MIME_NODE_MODULES");
    if (modules.isEmpty()) modules = QStringLiteral("C:/libs/services/broadcast-playout/node_modules");
    if (!QFileInfo::exists(modules + QStringLiteral("/mime-types/package.json")) ||
        !QFileInfo::exists(modules + QStringLiteral("/mime/package.json")))
        QSKIP("no local mime-types/mime packages (set ANFS_MIME_NODE_MODULES)");

    const std::vector<std::string> extra = {"", ".", "x", "README", "C:\\dir\\json", ".bashrc", "a.TXT",
                                            "file.m\xE2\x84\xAAv", "a.tar.gz", "dir.d\\file", "x.JSON", "x.js", "mp4",
                                            ".mp4", "C:\\a.b\\c", "C:\\Media\\Clip.MXF", "line1.mp4\nline2",
                                            "a\xE2\x80\xA8z.mp4", "é.mp3", "\\\\srv\\share\\v.ts"};
    QByteArray input;
    for (const auto& s : extra) input += qb(s).toHex() + '\n';
    const std::string script = sub("mime-oracle.js"), in = sub("mime-in.txt"), out = sub("mime-out.txt");
    QVERIFY(writeBytes(script, nodeMimeJs()));
    QVERIFY(writeBytes(in, input.toStdString()));
    if (!runTool(node, {qs(script), modules, qs(in), qs(out)})) QFAIL("node mime oracle failed");
    QList<QByteArray> lines = QByteArray::fromStdString(readBytes(out)).split('\n');
    QVERIFY(!lines.isEmpty());
    const QByteArray versions = lines.takeFirst();
    if (versions != "2.1.35 1.52.0 1.6.0") QSKIP(qPrintable("package versions differ: " + QString::fromUtf8(versions)));
    int n = 0, bad = 0;
    for (const QByteArray& line : lines) {
        if (line.isEmpty()) continue;
        const QList<QByteArray> c = line.split(' ');
        QCOMPARE(c.size(), qsizetype(4));
        const std::string s = QByteArray::fromHex(c[0]).toStdString();
        const std::string got[3] = {mime::forExtension(s), mime::forPath(s, ""), mime::contentTypeForFile(s)};
        for (int k = 0; k < 3; ++k) {
            const std::string want = QByteArray::fromHex(c[k + 1]).toStdString();
            if (got[k] != want && ++bad <= 15)
                qWarning("%s(\"%s\"): ours \"%s\", node \"%s\"",
                         k == 0 ? "forExtension" : k == 1 ? "forPath" : "contentTypeForFile", s.c_str(), got[k].c_str(),
                         want.c_str());
        }
        ++n;
    }
    QCOMPARE(bad, 0);
    QVERIFY(n > 1500);
    qInfo("mime-types 2.1.35 / mime 1.6.0: %d inputs x 3 lookups identical", n);
}

// ---- zipstream (ANFS-F2b) ---------------------------------------------------------------------------------------------

void TestAnfsCoreFs::zipGoldenBytes()
{
    // A folder and a two-byte file, assembled field by field from APPNOTE 6.3.10, independently of the writer.
    const int64_t t = localMs(2024, 3, 9, 14, 30, 41, 250);
    const std::pair<uint16_t, uint16_t> dos = qtDos(t);
    const uint32_t ut = static_cast<uint32_t>(t / 1000);
    const uint32_t crc = refCrc(std::string("hi"));
    auto utField = [&](std::string& s) { put16(s, 0x5455); put16(s, 5); s.push_back(1); put32(s, ut); };
    auto local = [&](std::string& s, const std::string& name, uint32_t c, uint32_t size) {
        put32(s, 0x04034b50); put16(s, 20); put16(s, 0x0800); put16(s, 0); put16(s, dos.first); put16(s, dos.second);
        put32(s, c); put32(s, size); put32(s, size); put16(s, static_cast<uint32_t>(name.size())); put16(s, 9);
        s += name;
        utField(s);
    };
    auto central = [&](std::string& s, const std::string& name, uint32_t c, uint32_t size, uint32_t attr, uint32_t at) {
        put32(s, 0x02014b50); put16(s, 20); put16(s, 20); put16(s, 0x0800); put16(s, 0); put16(s, dos.first);
        put16(s, dos.second); put32(s, c); put32(s, size); put32(s, size); put16(s, static_cast<uint32_t>(name.size()));
        put16(s, 9); put16(s, 0); put16(s, 0); put16(s, 0); put32(s, attr); put32(s, at);
        s += name;
        utField(s);
    };
    std::string want;
    local(want, "d/", 0, 0);
    const uint32_t fileAt = static_cast<uint32_t>(want.size());
    local(want, "d/x.txt", crc, 2);
    want += "hi";
    const uint32_t cdAt = static_cast<uint32_t>(want.size());
    central(want, "d/", 0, 0, 0x10, 0);
    central(want, "d/x.txt", crc, 2, 0, fileAt);
    const uint32_t cdSize = static_cast<uint32_t>(want.size()) - cdAt;
    put32(want, 0x06054b50); put16(want, 0); put16(want, 0); put16(want, 2); put16(want, 2); put32(want, cdSize);
    put32(want, cdAt); put16(want, 0);

    // Store, and Deflate (2 bytes do not shrink, so they are stored): the same bytes.
    for (ZipWriter::Method m : {ZipWriter::Method::Store, ZipWriter::Method::Deflate}) {
        RecordingSink sink;
        ZipWriter z(sink);
        z.addDirectory("d", t);
        StringSource hi("hi");
        z.addFile("d/x.txt", hi, t, m);
        QCOMPARE(sink.data.size(), size_t(0));          // gathered until 64 KB or finish()
        QCOMPARE(z.bytesWritten(), int64_t(cdAt));
        z.finish();
        QCOMPARE(qb(sink.data).toHex(), qb(want).toHex());
        QCOMPARE(z.bytesWritten(), int64_t(want.size()));
        QCOMPARE(z.entries(), 2);
        QCOMPARE(sink.flushes, 1);
        QCOMPARE(sink.writes.size(), size_t(1));
    }
    // An empty archive is the end record alone.
    RecordingSink empty;
    ZipWriter z0(empty);
    z0.finish();
    std::string end;
    put32(end, 0x06054b50);
    end.append(18, '\0');
    QCOMPARE(qb(empty.data).toHex(), qb(end).toHex());
    QCOMPARE(z0.entries(), 0);
    QCOMPARE(ZipWriter::deflateAvailable(), ANFS_HAVE_ZLIB != 0);
}

void TestAnfsCoreFs::zipEntryKinds()
{
    using M = ZipWriter::Method;
    const int64_t t = localMs(2023, 7, 15, 13, 45, 27, 300);
    const bool zl = ZipWriter::deflateAvailable();
    const uint16_t D = zl ? 8 : 0;
    struct Case {
        std::string name, data;
        int64_t announce;
        M method;
        uint16_t wantMethod;
        bool descriptor, zip64Local;
    };
    const std::vector<Case> cases = {
        {"k/empty.bin", "", 0, M::Deflate, 0, false, false},
        {"k/small.txt", textBytes(5000), 5000, M::Deflate, D, false, false},
        {"k/random-small.bin", randomBytes(4000, 1), 4000, M::Deflate, 0, false, false},   // deflate would grow it
        {"k/whole-max.txt", textBytes(65535), 65535, M::Deflate, D, false, false},
        {"k/exactly-64k.txt", textBytes(65536), 65536, M::Deflate, D, true, false},
        {"k/stream-store.bin", randomBytes(200000, 2), 200000, M::Store, 0, true, false},
        {"k/stream-random.bin", randomBytes(300000, 3), 300000, M::Deflate, D, true, false},
        {"k/stream-text-unknown.txt", textBytes(1 << 20), -1, M::Deflate, D, true, false},
        {"k/hinted-big.bin", randomBytes(100000, 4), 5LL << 30, M::Deflate, D, true, true},
        {"k/hinted-big-but-whole.bin", "ten bytes!", 5LL << 30, M::Deflate, 0, false, false},
        {"k/hint-below.txt", textBytes(70000), 0xF0000000LL - 1, M::Deflate, D, true, false},
        {"k/hint-at.bin", textBytes(70000), 0xF0000000LL, M::Store, 0, true, true},
    };
    RecordingSink sink;
    ZipWriter z(sink);
    z.addDirectory("k", t);
    for (const Case& c : cases) {
        PieceSource src(c.data, c.announce);
        z.addFile(c.name, src, t, c.method);
    }
    z.addDirectory("k/sub/", t);
    z.finish();
    QCOMPARE(z.bytesWritten(), int64_t(sink.data.size()));
    QCOMPARE(z.entries(), int(cases.size()) + 2);
    // Output gathering: 64 KB pieces, only the last one shorter, one flush at the end.
    QVERIFY(sink.writes.size() > 10);
    for (size_t i = 0; i + 1 < sink.writes.size(); ++i) QCOMPARE(sink.writes[i], size_t(65536));
    QVERIFY(sink.writes.back() <= 65536);
    QCOMPARE(sink.flushes, 1);

    ZipInfo info;
    const std::string perr = parseZip(memReader(sink.data), sink.data.size(), info);
    QVERIFY2(perr.empty(), perr.c_str());
    QVERIFY(!info.zip64End);
    QCOMPARE(info.recs.size(), cases.size() + 2);
    QCOMPARE(qs(info.recs.front().name), QStringLiteral("k/"));
    QCOMPARE(qs(info.recs.back().name), QStringLiteral("k/sub/"));
    const std::pair<uint16_t, uint16_t> dos = qtDos(t);
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        const ZipRec& r = info.recs[i + 1];
        const char* what = c.name.c_str();
        QVERIFY2(r.name == c.name, what);
        QVERIFY2(r.method == c.wantMethod, what);
        QVERIFY2(r.descriptor == c.descriptor, what);
        QVERIFY2(r.lZip64 == c.zip64Local, what);
        QVERIFY2(r.descriptor64 == c.zip64Local, what);   // small sizes: a zip64 descriptor only after a zip64 header
        QVERIFY2(r.z64Sizes == c.zip64Local, what);
        QVERIFY2(!r.z64Offset, what);
        QVERIFY2(r.dosTime == dos.first && r.dosDate == dos.second, what);
        QVERIFY2(r.utCentral && r.utTime == static_cast<uint32_t>(t / 1000), what);
        std::string err;
        const std::string got = zipContent(sink.data, r, &err);
        QVERIFY2(err.empty(), err.c_str());
        QVERIFY2(got == c.data, what);
        if (r.method == 8 && c.name.find(".txt") != std::string::npos) QVERIFY2(r.csize * 2 < r.usize, what);
    }

    // Memory stays flat: 512 MB stored (announced) and 128 MB deflated (unknown length) through a sink that samples the
    // process's private bytes.
    {
        struct SamplingSink final : Sink {
            using Sink::write;
            void write(const char*, size_t n) override
            {
                bytes += static_cast<int64_t>(n);
                if (++calls % 64 == 0) sample();
            }
            void sample()
            {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof pmc;
                if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
                    peak = std::max<size_t>(peak, pmc.PrivateUsage);
            }
            int64_t bytes = 0;
            size_t peak = 0;
            int calls = 0;
        };
        SamplingSink s;
        s.sample();
        const size_t base = s.peak;
        ZipWriter w(s);
        ZeroMarkSource stored(512ull << 20, {{100, "x"}}, 512LL << 20);
        w.addFile("flat/stored.bin", stored, t, M::Store);
        ZeroMarkSource deflated(128ull << 20, {{7, "y"}}, -1);
        w.addFile("flat/deflated.bin", deflated, t, M::Deflate);
        w.finish();
        s.sample();
        QCOMPARE(w.bytesWritten(), s.bytes);
        QVERIFY2(s.peak - base < (16u << 20), qPrintable(QStringLiteral("private bytes grew by %1 KB").arg((s.peak - base) >> 10)));
        qInfo("640 MB zipped (%lld bytes out), private bytes +%zu KB", static_cast<long long>(s.bytes), (s.peak - base) >> 10);
    }

    // Level 0 stores everything; levels 1 and 9 inflate back.
    for (int level : {0, 1, 9}) {
        RecordingSink s;
        ZipWriter w(s, level);
        PieceSource big(textBytes(300000), -1), small(textBytes(3000), 3000);
        w.addFile("t/big.txt", big, t);
        w.addFile("t/small.txt", small, t);
        w.finish();
        ZipInfo zi;
        const std::string e = parseZip(memReader(s.data), s.data.size(), zi);
        QVERIFY2(e.empty(), e.c_str());
        QCOMPARE(zi.recs.size(), size_t(2));
        for (const ZipRec& r : zi.recs) {
            QCOMPARE(r.method, uint16_t(level == 0 || !zl ? 0 : 8));
            std::string err;
            zipContent(s.data, r, &err);
            QVERIFY2(err.empty(), err.c_str());
        }
    }
}

void TestAnfsCoreFs::zipNamesAndTimes()
{
    const int64_t t = localMs(2022, 11, 20, 9, 15, 0);
    struct N {
        std::string in, want;
    };
    const std::string smile = "\xF0\x9F\x98\x80", fffd = "\xEF\xBF\xBD";
    const std::vector<N> names = {
        {"a\\b\\c.txt", "a/b/c.txt"},
        {"/lead/x", "lead/x"},
        {"C:\\top\\x", "top/x"},
        {"c:/top2/x", "top2/x"},
        {"a/../b/./c", "a/b/c"},
        {"a//b///c", "a/b/c"},
        {"..\\..\\evil.txt", "evil.txt"},
        {"x/C:/y", "x/C:/y"},                                     // after a kept segment a drive is relative: stays
        {"x/d:y", "x/d:y"},
        {"d:x", "x"},                                             // a drive-relative prefix goes (archiver /^\w+:/)
        {"./C:/Windows/evil.dll", "Windows/evil.dll"},            // drives after dropped segments go too (review)
        {"../C:/x", "x"},
        {"..\\..\\C:\\Users\\x", "Users/x"},
        {"C:/D:/x", "x"},
        {"C:C:\\x", "x"},
        {"C:..\\y", "y"},
        {"...", "..."},
        {"\xC3\xBC/\xE5\x90\x8D\xE5\x89\x8D/" + smile + ".txt", "\xC3\xBC/\xE5\x90\x8D\xE5\x89\x8D/" + smile + ".txt"},
        {"bad\xFF\xFEname", "bad" + fffd + fffd + "name"},
        {"cut\xC3", "cut" + fffd},
        {"\xE2\x82x", fffd + "x"},                                // E2 82 is one maximal subpart
        {"a\xED\xA0\x80z", "a" + fffd + "z"},                     // a lone WTF-8 surrogate: one U+FFFD
        {"\xED\xA0\xBD\xED\xB8\x80", smile},                      // a WTF-8 pair D83D DE00: the character
        {"\xED\xB8\x80\xED\xA0\xBD", fffd + fffd},                // low before high: two lone halves
        {"\xED\x9F\xBF", "\xED\x9F\xBF"},                         // U+D7FF is valid UTF-8
        {"\xF4\x90\x80\x80", fffd + fffd + fffd + fffd},          // above U+10FFFF
        {"\xC0\xAF.txt", fffd + fffd + ".txt"},                   // an overlong '/' is no separator
        {std::string(65535, 'n'), std::string(65535, 'n')},
    };
    RecordingSink sink;
    ZipWriter z(sink);
    for (const N& n : names) {
        StringSource x("x");
        z.addFile(n.in, x, t);
    }
    z.addDirectory("dir\\sub\\", t);
    z.addDirectory("/dir2", t);
    z.addDirectory(std::string(65534, 'm'), t);
    const int64_t before = z.bytesWritten();
    for (const char* bad : {"", "/", "\\\\", ".", "..", "C:", "c:\\", "./..//.", "C:\\..\\.", "d:", "./C:/", "C:D:",
                            "../c:/./", "C:.."}) {
        StringSource x("x");
        bool threw = false;
        try { z.addFile(bad, x, t); } catch (const std::invalid_argument&) { threw = true; }
        QVERIFY2(threw, bad);
        threw = false;
        try { z.addDirectory(bad, t); } catch (const std::invalid_argument&) { threw = true; }
        QVERIFY2(threw, bad);
    }
    for (bool dir : {false, true}) {
        bool threw = false;
        StringSource x("x");
        try {
            if (dir) z.addDirectory(std::string(65535, 'm'), t);   // 65536 bytes with the '/'
            else z.addFile(std::string(65536, 'n'), x, t);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        QVERIFY(threw);
    }
    QCOMPARE(z.bytesWritten(), before);                           // refused names wrote nothing
    z.finish();
    ZipInfo info;
    const std::string perr = parseZip(memReader(sink.data), sink.data.size(), info);
    QVERIFY2(perr.empty(), perr.c_str());
    QCOMPARE(info.recs.size(), names.size() + 3);
    for (size_t i = 0; i < names.size(); ++i) {
        if (info.recs[i].name != names[i].want)
            QFAIL(qPrintable(QStringLiteral("name %1: %2 instead of %3").arg(i).arg(QString::fromLatin1(qb(info.recs[i].name).toHex()))
                                 .arg(QString::fromLatin1(qb(names[i].want).toHex()))));
    }
    SEQ(info.recs[names.size()].name, "dir/sub/");
    SEQ(info.recs[names.size() + 1].name, "dir2/");
    QCOMPARE(info.recs[names.size() + 2].name, std::string(65534, 'm') + "/");

    // Every mix of up to 4 drive / dot / empty segments and both separators (7,380 names): a stored name never starts
    // with '/' or "X:" and has no empty, '.' or '..' segment; a name throws exactly when it has no segment that is
    // always kept ("x", "...", "Q:y" -> "y"). Only "C:..", "C:" etc. after a kept segment may stay (relative).
    {
        static const char* const segs[] = {"C:", "d:", ".", "..", "", "x", "Q:y", "C:..", "..."};
        const size_t ns = sizeof(segs) / sizeof(segs[0]);
        RecordingSink hs;
        ZipWriter hz(hs);
        size_t added = 0;
        for (size_t len = 1, total = ns; len <= 4; ++len, total *= ns) {
            for (size_t code = 0; code < total; ++code) {
                std::string name;
                bool keeps = false;
                for (size_t k = 0, c = code; k < len; ++k, c /= ns) {
                    if (k) name += ((k + code) & 1) ? '\\' : '/';
                    const std::string seg = segs[c % ns];
                    name += seg;
                    keeps = keeps || seg == "x" || seg == "..." || seg == "Q:y";
                }
                StringSource x("x");
                bool threw = false;
                try { hz.addFile(name, x, t); } catch (const std::invalid_argument&) { threw = true; }
                QVERIFY2(threw != keeps, name.c_str());
                added += threw ? 0 : 1;
            }
        }
        hz.finish();
        ZipInfo hinfo;
        const std::string herr = parseZip(memReader(hs.data), hs.data.size(), hinfo);
        QVERIFY2(herr.empty(), herr.c_str());
        QCOMPARE(hinfo.recs.size(), added);
        for (const auto& r : hinfo.recs) {
            const std::string& n = r.name;
            QVERIFY2(!n.empty() && n[0] != '/' && !(n.size() >= 2 && n[1] == ':'), n.c_str());
            size_t b = 0;
            while (b <= n.size()) {
                size_t e = n.find('/', b);
                if (e == std::string::npos) e = n.size();
                const std::string seg = n.substr(b, e - b);
                QVERIFY2(!seg.empty() && seg != "." && seg != ".." && seg.find('\\') == std::string::npos, n.c_str());
                b = e + 1;
            }
        }
    }

    // Times: DOS date/time in local time (2 s steps), clamped to 1980..2107; the UT field holds UTC seconds while they
    // fit its signed 32 bits (1970..2038-01-19 03:14:07 UTC).
    const uint16_t minDate = (1 << 5) | 1, maxDate = (127 << 9) | (12 << 5) | 31, maxTime = (23 << 11) | (59 << 5) | 29;
    struct T {
        int64_t ms;
        bool ut;
        int clamp;   // 0 none, -1 to 1980-01-01 00:00:00, +1 to 2107-12-31 23:59:58
    };
    const std::vector<T> times = {
        {localMs(2023, 7, 15, 13, 45, 27, 300), true, 0},
        {localMs(2021, 1, 3, 8, 0, 1), true, 0},
        {localMs(1980, 1, 1, 0, 0, 0), true, 0},
        {localMs(1979, 12, 31, 23, 59, 59), true, -1},
        {localMs(1975, 6, 1, 12, 0, 0), true, -1},
        {0, true, -1},
        {-1, false, -1},
        {std::numeric_limits<int64_t>::min(), false, -1},
        {2147483647000LL, true, 0},
        {2147483648000LL, false, 0},
        {localMs(2040, 2, 29, 23, 59, 59), false, 0},
        {localMs(2107, 12, 31, 23, 59, 59), false, 0},
        {localMs(2108, 1, 1, 0, 0, 0), false, 1},
        {253402300800000LL, false, 1},
        {std::numeric_limits<int64_t>::max(), false, 1},
    };
    RecordingSink ts;
    ZipWriter tz(ts);
    for (size_t i = 0; i < times.size(); ++i) tz.addDirectory("t" + std::to_string(i), times[i].ms);
    tz.finish();
    ZipInfo ti;
    const std::string terr = parseZip(memReader(ts.data), ts.data.size(), ti);
    QVERIFY2(terr.empty(), terr.c_str());
    for (size_t i = 0; i < times.size(); ++i) {
        const ZipRec& r = ti.recs[i];
        const T& c = times[i];
        const std::pair<uint16_t, uint16_t> want = c.clamp < 0 ? std::make_pair(uint16_t(0), minDate)
                                                   : c.clamp > 0 ? std::make_pair(maxTime, maxDate)
                                                                 : qtDos(c.ms);
        const QByteArray what = QByteArray::number(qlonglong(c.ms));
        QVERIFY2(r.dosTime == want.first && r.dosDate == want.second, what.constData());
        QVERIFY2(r.utCentral == c.ut && r.utLocal == c.ut, what.constData());
        if (c.ut) QVERIFY2(r.utTime == static_cast<uint32_t>(c.ms / 1000), what.constData());
    }
    // The first two: DOS seconds are even (floor), so 13:45:27 is stored as 13:45:26.
    QCOMPARE(ti.recs[0].dosTime & 0x1F, (27 / 2));
}

void TestAnfsCoreFs::zipErrorsAndMisuse()
{
    using M = ZipWriter::Method;
    const int64_t t = localMs(2024, 1, 2, 3, 4, 6);
    // A source failing while its entry is still read ahead (its first 64 KB) writes nothing: the caller skips it.
    {
        RecordingSink sink;
        ZipWriter z(sink);
        StringSource a("first");
        z.addFile("e/a.txt", a, t);
        const int64_t before = z.bytesWritten();
        for (size_t failAt : {size_t(0), size_t(1), size_t(30000), size_t(65535)}) {
            FailingSource bad(randomBytes(200000, 5), failAt);
            bool threw = false;
            try { z.addFile("e/bad.bin", bad, t); } catch (const IoError& e) { threw = std::string(e.what()) == "disk gone"; }
            QVERIFY(threw);
            QCOMPARE(z.bytesWritten(), before);
            QCOMPARE(z.entries(), 1);
        }
        PieceSource b(textBytes(100000), 100000);
        z.addFile("e/b.txt", b, t);
        z.finish();
        ZipInfo info;
        const std::string perr = parseZip(memReader(sink.data), sink.data.size(), info);
        QVERIFY2(perr.empty(), perr.c_str());
        QCOMPARE(info.recs.size(), size_t(2));
        SEQ(info.recs[1].name, "e/b.txt");
    }
    auto brokenWith = [](const std::function<void()>& f, const char* why, bool peerGone) {
        try {
            f();
        } catch (const IoError& e) {
            return e.peerGone == peerGone && std::string(e.what()) == std::string("zip archive is broken: ") + why;
        } catch (...) {
            return false;
        }
        return false;
    };
    // A failure after the header went out (from byte 65536 on) breaks the archive: later calls throw IoError, peerGone
    // kept from the first error, and the archive can never be finished.
    for (bool peerGone : {false, true}) {
        for (size_t failAt : {size_t(65536), size_t(250000)}) {
            RecordingSink sink;
            ZipWriter z(sink);
            StringSource a("first");
            z.addFile("e/a.txt", a, t);
            const int64_t before = z.bytesWritten();
            FailingSource bad(randomBytes(300000, 6), failAt, peerGone);
            bool threw = false;
            try { z.addFile("e/bad.bin", bad, t, M::Store); } catch (const IoError& e) { threw = e.peerGone == peerGone; }
            QVERIFY(threw);
            QVERIFY(z.bytesWritten() > before);                   // how a caller tells "broken" from "skip this file"
            StringSource c("c");
            QVERIFY(brokenWith([&] { z.addFile("e/c.txt", c, t); }, "disk gone", peerGone));
            QVERIFY(brokenWith([&] { z.addDirectory("e/d", t); }, "disk gone", peerGone));
            QVERIFY(brokenWith([&] { z.finish(); }, "disk gone", peerGone));
            QVERIFY(brokenWith([&] { z.finish(); }, "disk gone", peerGone));
            QCOMPARE(z.entries(), 1);
            QCOMPARE(sink.flushes, 0);
        }
    }
    // A sink failure breaks it too, also when it first surfaces at finish() (output is gathered up to 64 KB).
    {
        RecordingSink sink(100);
        ZipWriter z(sink);
        StringSource a(randomBytes(500, 7));
        z.addFile("e/a.bin", a, t, M::Store);
        QCOMPARE(sink.data.size(), size_t(0));
        bool threw = false;
        try { z.finish(); } catch (const IoError& e) { threw = e.peerGone; }
        QVERIFY(threw);
        QVERIFY(brokenWith([&] { z.finish(); }, "connection reset", true));
    }
    {
        RecordingSink sink(200000);
        ZipWriter z(sink);
        PieceSource big(randomBytes(500000, 8), 500000);
        bool threw = false;
        try { z.addFile("e/big.bin", big, t, M::Store); } catch (const IoError& e) { threw = e.peerGone; }
        QVERIFY(threw);
        StringSource c("c");
        QVERIFY(brokenWith([&] { z.addFile("e/c.txt", c, t); }, "connection reset", true));
    }
    // srv-xfer's Stop: a CountingSource cancel mid-entry is a quiet (peerGone) abort.
    {
        RecordingSink sink;
        ZipWriter z(sink);
        PieceSource inner(textBytes(500000), 500000);
        bool stop = false;
        CountingSource cs(inner, [&](int64_t total) { if (total > 100000) stop = true; }, [&] { return stop; });
        bool threw = false;
        try { z.addFile("e/x.txt", cs, t); } catch (const IoError& e) { threw = e.peerGone && std::string(e.what()) == "cancelled"; }
        QVERIFY(threw);
        QVERIFY(brokenWith([&] { z.finish(); }, "cancelled", true));
    }
    // Misuse after finish() is a logic_error; the destructor does not finish.
    {
        RecordingSink sink;
        ZipWriter z(sink);
        z.finish();
        StringSource a("a");
        int logic = 0;
        try { z.addFile("a", a, t); } catch (const std::logic_error&) { ++logic; }
        try { z.addDirectory("d", t); } catch (const std::logic_error&) { ++logic; }
        try { z.finish(); } catch (const std::logic_error&) { ++logic; }
        QCOMPARE(logic, 3);
    }
    {
        RecordingSink sink;
        {
            ZipWriter z(sink);
            PieceSource big(textBytes(400000), -1);
            z.addFile("e/x.txt", big, t, M::Store);
        }
        QCOMPARE(sink.flushes, 0);
        QVERIFY(sink.data.size() <= size_t(400000 + 100));
        QVERIFY(sink.data.find(std::string("PK\x05\x06", 4)) == std::string::npos);
        QVERIFY(sink.data.find(std::string("PK\x01\x02", 4)) == std::string::npos);
    }
}

void TestAnfsCoreFs::zipManyEntriesZip64End()
{
    const int64_t t = localMs(2020, 5, 6, 7, 8, 10);
    std::string many;
    for (int n : {65534, 65535, 70000}) {
        RecordingSink sink;
        ZipWriter z(sink);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < n; ++i) {
            const std::string dir = "m/" + std::to_string(i / 10);
            if (i % 10 == 0) {
                z.addDirectory(dir, t + i * 1000LL);
            } else {
                const std::string v = std::to_string(i);
                StringSource s(v);
                z.addFile(dir + "/" + v + ".txt", s, t + i * 1000LL);
            }
        }
        z.finish();
        const qint64 ms = timer.elapsed();
        QCOMPARE(z.entries(), n);
        ZipInfo info;
        const std::string perr = parseZip(memReader(sink.data), sink.data.size(), info);
        QVERIFY2(perr.empty(), perr.c_str());
        QCOMPARE(info.recs.size(), size_t(n));
        QCOMPARE(info.zip64End, n >= 0xFFFF);
        for (int i : {1, 9, 12345, n - 1}) {
            const ZipRec& r = info.recs[static_cast<size_t>(i)];
            std::string err;
            SEQ(zipContent(sink.data, r, &err), std::to_string(i));
            QVERIFY2(err.empty(), err.c_str());
            QCOMPARE(r.dosTime, qtDos(t + i * 1000LL).first);
        }
        qInfo("%d entries: %lld ms, %zu bytes, zip64 end record: %s", n, ms, sink.data.size(), info.zip64End ? "yes" : "no");
        if (n == 70000) many.swap(sink.data);
    }
    // The real readers count 70,000 entries through the zip64 end record.
    const std::string path = sub("many.zip");
    QVERIFY(writeBytes(path, many));
    const QString script = QStringLiteral(
        "try { Add-Type -AssemblyName System.IO.Compression.FileSystem } catch { Add-Type -AssemblyName System.IO.Compression.ZipFile }; "
        "$z = [System.IO.Compression.ZipFile]::OpenRead(%1); try { [Console]::Out.Write($z.Entries.Count) } finally { $z.Dispose() }")
                               .arg(psQuote(path));
    int readers = 0;
    for (const QString& ps : {findPwsh(), findPowerShell51()}) {
        if (ps.isEmpty()) continue;
        QByteArray out, err;
        QCOMPARE(runPs(ps, script, &out, &err), 0);
        QCOMPARE(out.trimmed(), QByteArray("70000"));
        ++readers;
    }
    const QString tar = findBsdtar();
    if (!tar.isEmpty()) {
        QByteArray out, err;
        QCOMPARE(runCode(tar, {QStringLiteral("-tf"), qs(path)}, &out, &err), 0);
        QCOMPARE(out.count('\n'), qsizetype(70000));
        ++readers;
    }
    if (!readers) qWarning("no PowerShell and no bsdtar: the 70,000-entry archive was checked by the in-test parser only");
}

void TestAnfsCoreFs::zipRoundTripExpandArchive()
{
    using M = ZipWriter::Method;
    const QString pwsh = findPwsh(), ps51 = findPowerShell51(), tar = findBsdtar();
    if (pwsh.isEmpty() && ps51.isEmpty() && tar.isEmpty()) QSKIP("neither PowerShell (Expand-Archive) nor bsdtar here");
    const std::string base = sub("ziprt"), src = base + "\\src\\tree";
    const std::string uni = "\xC3\xBCn\xC3\xAF \xE5\x90\x8D\xE5\x89\x8D";        // "ünï 名前"
    QVERIFY(fs::makeDirs(src + "\\sub\\deep"));
    QVERIFY(fs::makeDirs(src + "\\empty-dir"));
    QVERIFY(fs::makeDirs(src + "\\" + uni));
    struct F {
        std::string rel, data;
        int64_t mtime;
    };
    const std::vector<F> files = {
        {"a.txt", "hello zip\r\n", localMs(2023, 7, 15, 13, 45, 27, 300)},
        {"empty.bin", "", localMs(2021, 1, 3, 8, 0, 1)},
        {"whole-max.txt", textBytes(65535), localMs(2022, 3, 27, 2, 30, 5)},
        {"exactly-64k.bin", randomBytes(65536, 9), localMs(2024, 10, 1, 23, 59, 59)},
        {"name with spaces.txt", "spaces\n", localMs(2019, 12, 31, 12, 0, 0)},
        {"sub\\random.bin", randomBytes(200000, 10), localMs(2020, 6, 30, 18, 1, 2)},
        {"sub\\text.log", textBytes(300000), localMs(2018, 2, 14, 7, 7, 7)},
        {"sub\\deep\\store-me.dat", randomBytes(100000, 11), localMs(2025, 5, 5, 5, 5, 5)},
        {uni + "\\\xC3\x9F" "\xF0\x9F\x98\x80.txt", "unicode name\n", localMs(2023, 1, 1, 0, 0, 0)},   // ß😀.txt
    };
    for (const F& f : files) {
        const std::string p = src + "\\" + f.rel;
        QVERIFY2(writeBytes(p, f.data), p.c_str());
        QVERIFY2(setMtime(p, f.mtime), p.c_str());
    }
    const int64_t dirTime = localMs(2024, 2, 2, 2, 2, 2);
    const int64_t unknownTime = localMs(2022, 8, 8, 8, 8, 8), hintedTime = localMs(2021, 9, 9, 9, 9, 9);
    const std::string unknownData = textBytes(150000), hintedData = randomBytes(100000, 12);

    // Zip it as download-dir does: folders as entries, raw listing order, reparse points skipped.
    const std::string zipPath = base + "\\tree.zip";
    {
        fs::FileWriter fw;
        std::string err;
        QVERIFY2(fw.create(zipPath, &err), err.c_str());
        ZipWriter z(fw);
        std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& dir, const std::string& prefix) {
            fs::listDir(dir, [&](const fs::DirEntry& e) {
                if (e.isReparse) return true;
                const std::string full = dir + "\\" + e.name, name = prefix + e.name;
                if (e.isDir) {
                    z.addDirectory(name, e.mtimeMs);
                    walk(full, name + "/");
                    return true;
                }
                fs::FileReader r;
                if (r.open(full)) z.addFile(name, r, e.mtimeMs, name.find("store") != std::string::npos ? M::Store : M::Deflate);
                return true;
            });
        };
        z.addDirectory("tree", dirTime);
        walk(src, "tree/");
        // Two entries from sources that are not files: unknown length, and a length hint past 3.75 GiB (zip64 header).
        PieceSource unknown(unknownData, -1), hinted(hintedData, 5LL << 30);
        z.addFile("tree/unknown-length.txt", unknown, unknownTime);
        z.addFile("tree/hinted-zip64.bin", hinted, hintedTime);
        z.finish();
        QCOMPARE(z.bytesWritten(), fw.written());
        QVERIFY(fw.commit());
    }
    struct Want {
        std::string name, data;
        int64_t mtime;
        bool dir;
    };
    std::vector<Want> want;
    for (const F& f : files) {
        std::string n = "tree/" + f.rel;
        std::replace(n.begin(), n.end(), '\\', '/');
        want.push_back({n, f.data, f.mtime, false});
    }
    want.push_back({"tree/unknown-length.txt", unknownData, unknownTime, false});
    want.push_back({"tree/hinted-zip64.bin", hintedData, hintedTime, false});
    for (const char* d : {"tree/sub/", "tree/sub/deep/", "tree/empty-dir/"}) want.push_back({d, "", 0, true});
    want.push_back({"tree/" + uni + "/", "", 0, true});

    // The in-test parser: layout rules, every entry's content.
    std::string zip;
    QVERIFY(fs::readFile(zipPath, zip));
    ZipInfo info;
    const std::string perr = parseZip(memReader(zip), zip.size(), info);
    QVERIFY2(perr.empty(), perr.c_str());
    QCOMPARE(info.recs.size(), want.size() + 1);
    QVERIFY(info.find("tree/"));
    for (const Want& w : want) {
        const ZipRec* r = info.find(w.name);
        QVERIFY2(r, w.name.c_str());
        if (w.dir) continue;
        std::string err;
        QVERIFY2(zipContent(zip, *r, &err) == w.data, w.name.c_str());
        QVERIFY2(err.empty(), err.c_str());
        QVERIFY2(r->utTime == static_cast<uint32_t>(w.mtime / 1000), w.name.c_str());
    }
    QVERIFY(info.find("tree/hinted-zip64.bin")->lZip64);
    QVERIFY(info.find("tree/sub/deep/store-me.dat")->method == 0 && info.find("tree/sub/deep/store-me.dat")->descriptor);
    QVERIFY(info.find("tree/unknown-length.txt")->descriptor && !info.find("tree/unknown-length.txt")->lZip64);

    // An extracted tree holds exactly the entries, with the bytes; mtimes to the 2 s DOS step (Expand-Archive) or to the
    // second (bsdtar reads the UT field).
    auto checkTree = [&](const std::string& out, const char* who, int64_t stepMs) -> std::string {
        for (const Want& w : want) {
            std::string p = out + "\\" + w.name;
            std::replace(p.begin(), p.end(), '/', '\\');
            if (w.dir) {
                if (!fs::isDir(p)) return std::string(who) + ": missing folder " + w.name;
                continue;
            }
            std::string got;
            if (!fs::readFile(p, got) || got != w.data) return std::string(who) + ": content of " + w.name;
            const int64_t m = QFileInfo(qs(p)).lastModified().toMSecsSinceEpoch();
            if (m != w.mtime / stepMs * stepMs)
                return std::string(who) + ": mtime of " + w.name + " is " + std::to_string(m) + ", expected " +
                       std::to_string(w.mtime / stepMs * stepMs);
        }
        int count = 0;
        QDirIterator it(qs(out + "\\tree"), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            ++count;
        }
        if (count != static_cast<int>(want.size())) return std::string(who) + ": " + std::to_string(count) + " items extracted";
        return {};
    };
    int readers = 0;
    struct Ps {
        QString exe;
        const char* who;
    };
    for (const Ps& ps : {Ps{pwsh, "Expand-Archive (PowerShell 7)"}, Ps{ps51, "Expand-Archive (Windows PowerShell 5.1)"}}) {
        if (ps.exe.isEmpty()) continue;
        const std::string out = base + "\\out-" + std::to_string(readers);
        QByteArray o, e;
        const int rc = runPs(ps.exe, QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force").arg(psQuote(zipPath), psQuote(out)), &o, &e);
        QVERIFY2(rc == 0, e.constData());
        const std::string bad = checkTree(out, ps.who, 2000);
        QVERIFY2(bad.empty(), bad.c_str());
        // .NET's listing agrees on names and sizes (and the CRC where .NET shows it).
        QByteArray list;
        QCOMPARE(runPs(ps.exe, dotnetListScript(zipPath), &list, &e), 0);
        const std::vector<DotnetEntry> entries = parseDotnetList(list);
        QCOMPARE(entries.size(), info.recs.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            QVERIFY2(entries[i].name == info.recs[i].name, entries[i].name.c_str());
            QCOMPARE(entries[i].length, info.recs[i].usize);
            QCOMPARE(entries[i].compressed, info.recs[i].csize);
            if (entries[i].crc != "-") QCOMPARE(entries[i].crc, QByteArray::number(info.recs[i].crc));
        }
        qInfo("%s: %zu entries extracted and listed", ps.who, entries.size());
        ++readers;
    }
    if (!tar.isEmpty()) {
        const std::string out = base + "\\out-tar";
        QVERIFY(fs::makeDirs(out));
        QByteArray o, e;
        QVERIFY2(runCode(tar, {QStringLiteral("-xf"), qs(zipPath), QStringLiteral("-C"), qs(out)}, &o, &e) == 0, e.constData());
        const std::string bad = checkTree(out, "bsdtar", 1000);
        QVERIFY2(bad.empty(), bad.c_str());
        // bsdtar checks CRCs: a flipped byte in a stored entry and in a deflated one is reported (and the oracle is real).
        for (const char* victim : {"tree/sub/deep/store-me.dat", "tree/sub/text.log"}) {
            const ZipRec* r = info.find(victim);
            QVERIFY(r && r->csize > 2000);
            std::string damaged = zip;
            damaged[static_cast<size_t>(r->dataAt + r->csize / 2)] ^= 0x55;
            const std::string dpath = base + "\\damaged.zip";
            QVERIFY(writeBytes(dpath, damaged));
            QByteArray err;
            const int rc = runCode(tar, {QStringLiteral("-xOf"), qs(dpath), qs(victim)}, nullptr, &err, 60000, true);
            QVERIFY2(rc != 0, victim);
            qInfo("bsdtar on a damaged %s: exit %d, %s", victim, rc, err.trimmed().constData());
        }
        QCOMPARE(runCode(tar, {QStringLiteral("-xOf"), qs(zipPath), QStringLiteral("tree/sub/deep/store-me.dat")}, nullptr, nullptr, 60000, true), 0);
        ++readers;
    }
    qInfo("round trip read back by %d tools", readers);
}

void TestAnfsCoreFs::zipZip64Over4GiB()
{
    if (qEnvironmentVariableIntValue("ANFS_HEAVY_TESTS") != 1)
        QSKIP("heavy test: set ANFS_HEAVY_TESTS=1 and run it under the machine-wide build/test lock");
    const QString dirRoot = qEnvironmentVariableIsSet("ANFS_HEAVY_DIR") ? qEnvironmentVariable("ANFS_HEAVY_DIR")
                                                                        : QCoreApplication::applicationDirPath();
    const uint64_t N = (1ull << 32) + (1ull << 20) + 7;          // 4 GiB + 1 MiB + 7
    const qint64 freeBytes = QStorageInfo(dirRoot).bytesAvailable();
    if (freeBytes < static_cast<qint64>(N + (4ull << 30)))
        QSKIP(qPrintable(QStringLiteral("needs about 8.3 GB free in %1 (has %2 MB)").arg(dirRoot).arg(freeBytes >> 20)));
    wchar_t volume[MAX_PATH] = {};
    DWORD volFlags = 0;
    if (!GetVolumePathNameW(fs::widen(u8(QDir::toNativeSeparators(dirRoot))).c_str(), volume, MAX_PATH) ||
        !GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &volFlags, nullptr, 0) || !(volFlags & FILE_SUPPORTS_SPARSE_FILES))
        QSKIP("the volume has no sparse files");
    QTemporaryDir tmp(dirRoot + QStringLiteral("/anfs-zip64-XXXXXX"));
    QVERIFY(tmp.isValid());
    const std::string dir = fs::longPath(u8(QDir::toNativeSeparators(tmp.path())));
    const QString pwsh = findPwsh(), ps51 = findPowerShell51(), tar = findBsdtar();
    QElapsedTimer timer;
    timer.start();

    // The source: a sparse file of N bytes, zeros except four marks (one across the 4 GiB boundary).
    const std::vector<std::pair<uint64_t, std::string>> marks = {
        {0, randomBytes(4096, 21)},
        {(3ull << 29) + 12345, randomBytes(100, 22)},
        {(1ull << 32) - 3000, randomBytes(8000, 23)},
        {N - 777, randomBytes(777, 24)},
    };
    const std::string sparse = dir + "\\sparse.bin";
    QVERIFY(makeSparseFile(sparse, N, marks));
    uint32_t wantCrc = 0;
    {
        fs::FileReader r;
        QVERIFY(r.open(sparse));
        QCOMPARE(uint64_t(r.size()), N);
        std::string buf(1 << 20, '\0');
        for (size_t k; (k = r.read(&buf[0], buf.size())) != 0;) wantCrc = refCrc(buf.data(), k, wantCrc);
    }
    qInfo("sparse source of %llu bytes, zlib CRC %08x: %lld ms", static_cast<unsigned long long>(N), wantCrc, timer.restart());
    const int64_t mt = localMs(2024, 8, 1, 10, 0, 0);
    const std::string head = textBytes(3000), after = "after the 4 GiB entry\n";

    // Archive A: a stored 4 GiB + 1 MiB entry from the file (zip64 local header: its length is announced), then two
    // entries that start past 4 GiB, so the end records are zip64 as well.
    const std::string zipA = dir + "\\a.zip";
    {
        SparseFileSink sink;
        QVERIFY(sink.open(zipA));
        ZipWriter z(sink);
        z.addDirectory("big", mt);
        StringSource h(head);
        z.addFile("big/head.txt", h, mt);
        fs::FileReader r;
        QVERIFY(r.open(sparse));
        z.addFile("big/sparse.bin", r, mt, ZipWriter::Method::Store);
        StringSource a(after);
        z.addFile("big/after.txt", a, mt);
        z.addDirectory("big/z", mt);
        z.finish();
        QCOMPARE(static_cast<uint64_t>(z.bytesWritten()), sink.size());
        QVERIFY(sink.close());
    }
    qInfo("archive A written (%llu bytes, %lu KB on disk): %lld ms",
          static_cast<unsigned long long>(FileAt(zipA).size()), GetCompressedFileSizeW(fs::widen(zipA).c_str(), nullptr) >> 10,
          timer.restart());
    {
        FileAt fa(zipA);
        QVERIFY(fa.ok());
        ZipInfo zi;
        const std::string e = parseZip(fa.reader(), fa.size(), zi);
        QVERIFY2(e.empty(), e.c_str());
        QVERIFY(zi.zip64End);
        QVERIFY(zi.cdStart > kM32);
        const ZipRec* big = zi.find("big/sparse.bin");
        QVERIFY(big);
        QVERIFY(big->lZip64 && big->descriptor64 && big->z64Sizes && !big->z64Offset);
        QCOMPARE(big->method, uint16_t(0));
        QCOMPARE(big->usize, N);
        QCOMPARE(big->csize, N);
        QCOMPARE(big->crc, wantCrc);
        const ZipRec* af = zi.find("big/after.txt");
        QVERIFY(af && af->z64Offset && !af->z64Sizes && af->offset > kM32);
        QCOMPARE(fa.read(af->dataAt, static_cast<size_t>(af->csize)), after);
        const ZipRec* zd = zi.find("big/z/");
        QVERIFY(zd && zd->z64Offset);
        const ZipRec* hd = zi.find("big/head.txt");
        QVERIFY(hd && !hd->z64Offset && !hd->z64Sizes);
    }
    if (!tar.isEmpty()) {   // bsdtar reads the entry to its end and checks the CRC; stdout goes to NUL (no disk writes)
        QByteArray err;
        const int rc = runCode(tar, {QStringLiteral("-xOf"), qs(zipA), QStringLiteral("big/sparse.bin")}, nullptr, &err, 600000, true);
        QVERIFY2(rc == 0, err.constData());
        qInfo("bsdtar read big/sparse.bin and its CRC: %lld ms", timer.restart());
    }
    const QString ps = !pwsh.isEmpty() ? pwsh : ps51;
    if (!ps.isEmpty()) {     // Expand-Archive extracts it; the 4 GiB file is compared byte for byte with the source
        const std::string out = dir + "\\outA";
        QByteArray o, e;
        const int rc = runPs(ps, QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force").arg(psQuote(zipA), psQuote(out)), &o, &e, 900000);
        QVERIFY2(rc == 0, e.constData());
        qInfo("Expand-Archive (%s): %lld ms", qPrintable(QFileInfo(ps).fileName()), timer.restart());
        std::string got;
        QVERIFY(fs::readFile(out + "\\big\\head.txt", got) && got == head);
        QVERIFY(fs::readFile(out + "\\big\\after.txt", got) && got == after);
        QVERIFY(fs::isDir(out + "\\big\\z"));
        const std::string diff = compareFiles(out + "\\big\\sparse.bin", sparse);
        QVERIFY2(diff.empty(), diff.c_str());
        QVERIFY(fs::removeFile(out + "\\big\\sparse.bin"));
        qInfo("extracted 4 GiB entry equals the source: %lld ms", timer.restart());
    } else {
        qWarning("no PowerShell: Expand-Archive not run on the zip64 archive");
    }

    // Archive B: the same bytes from a source of unknown length: no zip64 local field, so the descriptor and the
    // central sizes switch to zip64 after the fact; the entry after it starts past 4 GiB.
    const std::string zipB = dir + "\\b.zip";
    {
        SparseFileSink sink;
        QVERIFY(sink.open(zipB));
        ZipWriter z(sink);
        ZeroMarkSource gen(N, marks, -1);
        z.addFile("s/stream.bin", gen, mt, ZipWriter::Method::Store);
        StringSource a(after);
        z.addFile("s/after.txt", a, mt);
        z.finish();
        QVERIFY(sink.close());
    }
    qInfo("archive B written: %lld ms", timer.restart());
    {
        FileAt fb(zipB);
        ZipInfo zi;
        const std::string e = parseZip(fb.reader(), fb.size(), zi);
        QVERIFY2(e.empty(), e.c_str());
        QVERIFY(zi.zip64End);
        const ZipRec& s = zi.recs.at(0);
        QVERIFY(!s.lZip64 && s.descriptor && s.descriptor64 && s.z64Sizes);
        QCOMPARE(s.usize, N);
        QCOMPARE(s.crc, wantCrc);                                 // the generated stream equals the sparse file
        QVERIFY(zi.recs.at(1).z64Offset);
    }
    if (!tar.isEmpty()) {
        QByteArray err;
        const int rc = runCode(tar, {QStringLiteral("-xOf"), qs(zipB), QStringLiteral("s/stream.bin")}, nullptr, &err, 600000, true);
        QVERIFY2(rc == 0, err.constData());
        qInfo("bsdtar read s/stream.bin and its CRC: %lld ms", timer.restart());
    }
    // .NET 9 (PowerShell 7) and .NET Framework (Windows PowerShell 5.1) list both archives and read B's entry to its end.
    for (const QString& p : {pwsh, ps51}) {
        if (p.isEmpty()) continue;
        for (const std::string& zp : {zipA, zipB}) {
            QByteArray list, e;
            QCOMPARE(runPs(p, dotnetListScript(zp), &list, &e), 0);
            const std::vector<DotnetEntry> entries = parseDotnetList(list);
            const bool isA = zp == zipA;
            QCOMPARE(entries.size(), size_t(isA ? 5 : 2));
            const DotnetEntry& big = entries[isA ? 2 : 0];
            SEQ(big.name, isA ? "big/sparse.bin" : "s/stream.bin");
            QCOMPARE(big.length, N);
            QCOMPARE(big.compressed, N);
            if (big.crc != "-") QCOMPARE(big.crc, QByteArray::number(wantCrc));
            SEQ(entries.back().name, isA ? "big/z/" : "s/after.txt");
        }
        const QString readAll = QStringLiteral(
            "try { Add-Type -AssemblyName System.IO.Compression.FileSystem } catch { Add-Type -AssemblyName System.IO.Compression.ZipFile }; "
            "$z = [System.IO.Compression.ZipFile]::OpenRead(%1); try { $s = $z.GetEntry('s/stream.bin').Open(); "
            "$b = New-Object byte[] 4194304; $t = [long]0; while (($k = $s.Read($b, 0, $b.Length)) -gt 0) { $t += $k }; "
            "$s.Dispose(); [Console]::Out.Write($t) } finally { $z.Dispose() }").arg(psQuote(zipB));
        QByteArray o, e;
        QCOMPARE(runPs(p, readAll, &o, &e, 900000), 0);
        QCOMPARE(o.trimmed(), QByteArray::number(qulonglong(N)));
        qInfo("%s: .NET listed both archives and read the 4 GiB entry of B: %lld ms", qPrintable(QFileInfo(p).fileName()),
              timer.restart());
    }
}

namespace {

// node path-oracle.js <in> <out>: lines "hex(a) hex(b)" -> nine hex columns.
const char* nodePathJs()
{
    return R"JS(const fs = require('fs'), p = require('path').win32;
const [,, inFile, outFile] = process.argv;
const h = s => Buffer.from(s, 'utf8').toString('hex');
const u = x => Buffer.from(x, 'hex').toString('utf8');
const out = [];
for (const line of fs.readFileSync(inFile, 'utf8').split('\n')) {
  if (!line.length) continue;
  const [a, b] = line.split(' ').map(u);
  out.push([p.normalize(a), p.join(a, b), p.basename(a), p.basename(a, b), p.dirname(a), p.extname(a),
            p.isAbsolute(a) ? '1' : '0', p.resolve(a), p.resolve(a, b)].map(h).join(' '));
}
fs.writeFileSync(outFile, out.join('\n') + '\n');
)JS";
}

// node mime-oracle.js <node_modules> <in> <out>: the versions, then for every extension either package knows plus the
// extra inputs: hex(input) hex(mime-types lookup) hex(lookup(path.extname)) hex(send's Content-Type).
const char* nodeMimeJs()
{
    return R"JS(const fs = require('fs'), path = require('path');
const [,, root, inFile, outFile] = process.argv;
const mtDir = path.join(root, 'mime-types');
const mt = require(mtDir);
const dbDir = path.dirname(require.resolve('mime-db', { paths: [mtDir] }));
const db = require(path.join(dbDir, 'db.json'));
const m1 = require(path.join(root, 'mime'));
const ver = d => require(path.join(d, 'package.json')).version;
const h = s => Buffer.from(s, 'utf8').toString('hex');
const send = s => { const t = m1.lookup(s); const c = m1.charsets.lookup(t); return t + (c ? '; charset=' + c : ''); };
const inputs = new Set();
for (const t of Object.keys(db)) for (const e of (db[t].extensions || [])) { inputs.add(e); inputs.add('C:\\d\\f.' + e.toUpperCase()); }
for (const e of Object.keys(m1.types)) inputs.add('x.' + e);
for (const line of fs.readFileSync(inFile, 'utf8').split('\n')) if (line.length || inputs.size) inputs.add(Buffer.from(line, 'hex').toString('utf8'));
const out = [ver(mtDir) + ' ' + ver(dbDir) + ' ' + ver(path.join(root, 'mime'))];
for (const s of inputs) out.push([s, mt.lookup(s) || '', mt.lookup(path.win32.extname(s)) || '', send(s)].map(h).join(' '));
fs.writeFileSync(outFile, out.join('\n') + '\n');
)JS";
}

} // namespace

QTEST_GUILESS_MAIN(TestAnfsCoreFs)
#include "test_anfs_core_fs.moc"
