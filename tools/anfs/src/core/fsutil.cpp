// core/fsutil.cpp - Win32 file-system helpers on UTF-8 paths (ANFS-F2a).
//
// - Paths: Win32 semantics (what the C# port and every Windows tool see). A path of 240+ UTF-16 units goes through
//   GetFullPathNameW and gets the \\?\ (\\?\UNC\) prefix, so long paths work without the long-path opt-in, and Node
//   reaches them too (fs calls toNamespacedPath). A path with an embedded NUL is refused (Node throws
//   ERR_INVALID_ARG_VALUE); Win32 would silently cut it there.
// - listDir: FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH), entries in the raw order the file system
//   returns (never sorted), size/times/attributes from the same call. NTFS refreshes the copy in the directory index
//   lazily, so a file open for writing may show a stale size there; Node's per-entry stat would see the live value.
//   An empty drive root has no "." entry and answers ERROR_FILE_NOT_FOUND: listed as empty, not as an error.
// - stat: GetFileAttributesExW (does not follow reparse points). A locked system file (pagefile.sys) answers
//   ERROR_SHARING_VIOLATION; its directory entry is read instead (.NET does the same), so it exists (deviation C2).
// - Deletes open the entry with FILE_FLAG_OPEN_REPARSE_POINT (a junction or symlink is removed, never its target) and
//   use POSIX delete semantics with the read-only attribute ignored, as libuv's unlink does on Windows 10 1809+; the
//   name is gone at once even while another process holds it open. Older systems and FAT/SMB fall back to clearing
//   read-only and the classic delete-on-close. removeTree addresses what it found by the \\?\ form (names like
//   'report ' or 'nul' are deleted exactly); removeFile, like every call given a path, keeps Win32 semantics (C#'s).
// - makeDirs never loops: a child that still fails after its parent was made ('Photos ' / '...' as a middle name,
//   which Win32 trims only as the last one) is ENOENT, as in C#; Node would create those names literally.
// - Errors carry Node's text ("ENOENT: no such file or directory, open 'C:\x'") with libuv's Win32 mapping
//   (uv_translate_sys_error: ERROR_ACCESS_DENIED is EPERM, ERROR_SHARING_VIOLATION EBUSY).
#include "core/fsutil.h"

#include "core/nodepath.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace anfs::fs {

namespace {

constexpr size_t kPrefixAt = 240;          // UTF-16 units; leaves room for "\*" and MAX_PATH - 12 (CreateDirectory)
constexpr int kBusyRetries = 10;           // rename / rmdir retries on transient sharing errors (10 x 20 ms)

// FileDispositionInfoEx (Windows 10 1607+; IGNORE_READONLY 1809+). The SDK hides the enum value behind
// NTDDI_WIN10_RS1, above what _WIN32_WINNT=0x0A00 selects, so the values are spelled out here.
constexpr FILE_INFO_BY_HANDLE_CLASS kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr DWORD kDispositionDelete = 0x1;
constexpr DWORD kDispositionPosix = 0x2;
constexpr DWORD kDispositionIgnoreReadOnly = 0x10;
struct DispositionInfoEx {
    DWORD Flags;
};

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

bool isSepW(wchar_t c) { return c == L'\\' || c == L'/'; }

void setErr(std::string* err, std::string text)
{
    if (err) *err = std::move(text);
}

// UTF-8 path -> the wide path handed to Win32 (see the file comment). false for a NUL inside the path.
bool winPath(const std::string& path, std::wstring& out)
{
    if (path.find('\0') != std::string::npos) return false;
    out = widen(path);
    if (out.size() < kPrefixAt) return true;
    if (out.size() >= 4 && isSepW(out[0]) && isSepW(out[1]) && (out[2] == L'?' || out[2] == L'.') && isSepW(out[3]))
        return true;   // already a device path
    DWORD n = GetFullPathNameW(out.c_str(), 0, nullptr, nullptr);
    if (n == 0) return true;   // let the API report the problem
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(out.c_str(), n, &full[0], nullptr);
    full.resize(n);
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') out = L"\\\\?\\UNC\\" + full.substr(2);
    else if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') out = L"\\\\?\\" + full;
    else out = full;
    return true;
}

// GetFullPathNameW ("" on failure). A \\?\ path comes back as it is.
std::wstring getFullPathW(const std::wstring& w)
{
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, &out[0], nullptr);
    if (n == 0 || n >= out.size()) return {};   // a size >= the buffer means it did not fit
    out.resize(n);
    return out;
}

// The drive or UNC form of a resolved path: "\\?\C:\x" -> "C:\x", "\\?\UNC\srv\share" -> "\\srv\share", "\\.\C:" ->
// "C:\". A path without a device prefix comes back as it is; any other device path (\\?\Volume{...}\,
// \\?\GLOBALROOT\..., \\.\PhysicalDrive0) as "".
std::wstring plainPath(const std::wstring& p)
{
    if (!(p.size() >= 4 && isSepW(p[0]) && isSepW(p[1]) && (p[2] == L'?' || p[2] == L'.') && isSepW(p[3]))) return p;
    const std::wstring rest = p.substr(4);
    auto lower = [](wchar_t c) { return static_cast<wchar_t>(c | 0x20); };
    if (rest.size() >= 4 && lower(rest[0]) == L'u' && lower(rest[1]) == L'n' && lower(rest[2]) == L'c' && isSepW(rest[3]))
        return L"\\\\" + rest.substr(4);
    const bool drive = rest.size() >= 2 && lower(rest[0]) >= L'a' && lower(rest[0]) <= L'z' && rest[1] == L':';
    if (drive && rest.size() == 2) return rest + L"\\";
    if (drive && isSepW(rest[2])) return rest;
    return {};
}

// A drive or share root ("C:\", "\\srv\share\") in plain form: Node's dirname of it is itself.
bool isRootPlain(const std::wstring& plain)
{
    const std::string r = nodepath::resolve(narrow(plain));
    return nodepath::dirname(r) == r;
}

// The \\?\ form of a plain absolute path, which names an entry exactly: no trailing dot or space trimmed, no DOS device
// name ("nul") mapped, "..." kept.
std::wstring exactPath(const std::wstring& plain)
{
    if (plain.size() >= 2 && isSepW(plain[0]) && isSepW(plain[1])) return L"\\\\?\\UNC\\" + plain.substr(2);
    return L"\\\\?\\" + plain;
}

int64_t fileTimeMs(const FILETIME& ft)
{
    const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    const int64_t d = t - 116444736000000000LL;   // 1601 -> 1970 in 100 ns units
    return d >= 0 ? d / 10000 : -((-d + 9999) / 10000);
}

int64_t joinSize(DWORD high, DWORD low) { return static_cast<int64_t>((static_cast<uint64_t>(high) << 32) | low); }

struct FindCloser {
    HANDLE h;
    ~FindCloser() { if (h != INVALID_HANDLE_VALUE) FindClose(h); }
};

struct HandleCloser {
    HANDLE h;
    ~HandleCloser() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

// Node's uv_fs_stat follows a link: 0 when the entry behind w is a directory, ERROR_DIRECTORY when it is a file, else
// the Win32 error (a junction whose target is gone gives 2 or 3).
DWORD followedDirError(const std::wstring& w)
{
    HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    HandleCloser closer{h};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)) return GetLastError();
    return (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ERROR_DIRECTORY;
}

// Opens an entry for deletion, never following a reparse point. FILE_WRITE_ATTRIBUTES (for the read-only fallback)
// is asked first and dropped when refused.
HANDLE openForDelete(const std::wstring& w)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE h = CreateFileW(w.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, share, nullptr,
                           OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        h = CreateFileW(w.c_str(), DELETE | FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, flags, nullptr);
    return h;
}

// Deletes the entry (file, empty directory, or the link itself). allowDir=false refuses a real directory.
// Returns 0 or the Win32 error.
DWORD deleteEntry(const std::wstring& w, bool allowDir)
{
    HANDLE h = openForDelete(w);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    HandleCloser closer{h};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) return GetLastError();
    const bool dir = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool reparse = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (dir && !reparse && !allowDir) return ERROR_DIRECTORY_NOT_SUPPORTED;

    DispositionInfoEx ex{kDispositionDelete | kDispositionPosix | kDispositionIgnoreReadOnly};
    if (SetFileInformationByHandle(h, kFileDispositionInfoEx, &ex, sizeof ex)) return 0;
    const DWORD exErr = GetLastError();
    if (exErr == ERROR_DIR_NOT_EMPTY) return exErr;
    // Classic path (FAT, SMB, pre-1809): clear read-only, then delete on close.
    if (tag.FileAttributes & FILE_ATTRIBUTE_READONLY) {
        FILE_BASIC_INFO basic{};
        if (GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)) {
            basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
            if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof basic);
        }
    }
    FILE_DISPOSITION_INFO di{};
    di.DeleteFile = TRUE;
    if (SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di)) return 0;
    return GetLastError();
}

struct UvErr {
    DWORD win;
    const char* code;
};

// libuv src/win/error.c uv_translate_sys_error, the entries a file system can produce.
const UvErr kUvErrors[] = {
    {ERROR_NOACCESS, "EACCES"}, {ERROR_ELEVATION_REQUIRED, "EACCES"}, {ERROR_CANT_ACCESS_FILE, "EACCES"},
    {ERROR_INVALID_FLAGS, "EBADF"}, {ERROR_INVALID_HANDLE, "EBADF"},
    {ERROR_LOCK_VIOLATION, "EBUSY"}, {ERROR_PIPE_BUSY, "EBUSY"}, {ERROR_SHARING_VIOLATION, "EBUSY"},
    {ERROR_OPERATION_ABORTED, "ECANCELED"}, {ERROR_NO_UNICODE_TRANSLATION, "ECHARSET"},
    {ERROR_NETNAME_DELETED, "ECONNRESET"}, {ERROR_ALREADY_EXISTS, "EEXIST"}, {ERROR_FILE_EXISTS, "EEXIST"},
    {ERROR_BUFFER_OVERFLOW, "EFAULT"}, {ERROR_INSUFFICIENT_BUFFER, "EINVAL"}, {ERROR_INVALID_DATA, "EINVAL"},
    {ERROR_INVALID_NAME, "ENOENT"}, {ERROR_INVALID_PARAMETER, "EINVAL"}, {ERROR_SYMLINK_NOT_SUPPORTED, "EINVAL"},
    {ERROR_BEGINNING_OF_MEDIA, "EIO"}, {ERROR_BUS_RESET, "EIO"}, {ERROR_CRC, "EIO"}, {ERROR_DEVICE_DOOR_OPEN, "EIO"},
    {ERROR_DEVICE_REQUIRES_CLEANING, "EIO"}, {ERROR_DISK_CORRUPT, "EIO"}, {ERROR_EOM_OVERFLOW, "EIO"},
    {ERROR_FILEMARK_DETECTED, "EIO"}, {ERROR_GEN_FAILURE, "EIO"}, {ERROR_INVALID_BLOCK_LENGTH, "EIO"},
    {ERROR_IO_DEVICE, "EIO"}, {ERROR_NO_DATA_DETECTED, "EIO"}, {ERROR_NO_SIGNAL_SENT, "EIO"}, {ERROR_OPEN_FAILED, "EIO"},
    {ERROR_SETMARK_DETECTED, "EIO"}, {ERROR_SIGNAL_REFUSED, "EIO"},
    {ERROR_CANT_RESOLVE_FILENAME, "ELOOP"}, {ERROR_TOO_MANY_OPEN_FILES, "EMFILE"},
    {ERROR_FILENAME_EXCED_RANGE, "ENAMETOOLONG"}, {ERROR_BAD_PATHNAME, "ENOENT"}, {ERROR_DIRECTORY, "ENOENT"},
    {ERROR_ENVVAR_NOT_FOUND, "ENOENT"}, {ERROR_FILE_NOT_FOUND, "ENOENT"}, {ERROR_INVALID_DRIVE, "ENOENT"},
    {ERROR_INVALID_REPARSE_DATA, "ENOENT"}, {ERROR_MOD_NOT_FOUND, "ENOENT"}, {ERROR_PATH_NOT_FOUND, "ENOENT"},
    {ERROR_NOT_ENOUGH_MEMORY, "ENOMEM"}, {ERROR_OUTOFMEMORY, "ENOMEM"}, {ERROR_CANNOT_MAKE, "ENOSPC"},
    {ERROR_DISK_FULL, "ENOSPC"}, {ERROR_EA_TABLE_FULL, "ENOSPC"}, {ERROR_END_OF_MEDIA, "ENOSPC"},
    {ERROR_HANDLE_DISK_FULL, "ENOSPC"}, {ERROR_NOT_CONNECTED, "ENOTCONN"}, {ERROR_DIR_NOT_EMPTY, "ENOTEMPTY"},
    {ERROR_NOT_SUPPORTED, "ENOTSUP"}, {ERROR_BROKEN_PIPE, "EOF"}, {ERROR_ACCESS_DENIED, "EPERM"},
    {ERROR_PRIVILEGE_NOT_HELD, "EPERM"}, {ERROR_BAD_PIPE, "EPIPE"}, {ERROR_NO_DATA, "EPIPE"},
    {ERROR_PIPE_NOT_CONNECTED, "EPIPE"}, {ERROR_WRITE_PROTECT, "EROFS"}, {ERROR_SEM_TIMEOUT, "ETIMEDOUT"},
    {ERROR_NOT_SAME_DEVICE, "EXDEV"}, {ERROR_INVALID_FUNCTION, "EISDIR"}, {ERROR_META_EXPANSION_TOO_LONG, "E2BIG"},
};   // anything else (ERROR_BAD_NETPATH included) is "UNKNOWN: unknown error", as Node prints it

// uv_strerror texts (uv-common.h UV_ERRNO_MAP).
const char* uvText(std::string_view code)
{
    static const std::pair<const char*, const char*> kTexts[] = {
        {"E2BIG", "argument list too long"}, {"EACCES", "permission denied"}, {"EBADF", "bad file descriptor"},
        {"EBUSY", "resource busy or locked"}, {"ECANCELED", "operation canceled"},
        {"ECHARSET", "invalid Unicode character"}, {"ECONNRESET", "connection reset by peer"},
        {"EEXIST", "file already exists"}, {"EFAULT", "bad address in system call argument"},
        {"EINVAL", "invalid argument"}, {"EIO", "i/o error"}, {"EISDIR", "illegal operation on a directory"},
        {"ELOOP", "too many symbolic links encountered"}, {"EMFILE", "too many open files"},
        {"ENAMETOOLONG", "name too long"}, {"ENOENT", "no such file or directory"}, {"ENOMEM", "not enough memory"},
        {"ENOSPC", "no space left on device"}, {"ENOTCONN", "socket is not connected"},
        {"ENOTDIR", "not a directory"}, {"ENOTEMPTY", "directory not empty"},
        {"ENOTSUP", "operation not supported on socket"}, {"EOF", "end of file"}, {"EPERM", "operation not permitted"},
        {"EPIPE", "broken pipe"}, {"EROFS", "read-only file system"}, {"ETIMEDOUT", "connection timed out"},
        {"EXDEV", "cross-device link not permitted"},
    };
    for (const auto& t : kTexts)
        if (code == t.first) return t.second;
    return "unknown error";
}

std::string nodeErrorCode(const char* code, std::string_view syscall, std::string_view path)
{
    std::string s = std::string(code) + ": " + uvText(code) + ", " + std::string(syscall);
    if (!path.empty()) s += " '" + std::string(path) + "'";
    return s;
}

void sleepBriefly() { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }   // I/O retry, real time

} // namespace

// ---- strings ------------------------------------------------------------------------------------------------------

std::wstring widen(std::string_view s)
{
    std::wstring w;
    w.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    auto cont = [&](size_t k) { return k < n && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80; };
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            w.push_back(static_cast<wchar_t>(c));
            ++i;
            continue;
        }
        // Expected length and the allowed range of the second byte (WHATWG UTF-8 decoder, plus ED A0..BF for WTF-8).
        size_t need = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c == 0xE0) { need = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEF) need = 2;
        else if (c == 0xF0) { need = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4) { need = 3; hi = 0x8F; }
        if (need == 0) {
            w.push_back(0xFFFD);
            ++i;
            continue;
        }
        const unsigned char c1 = i + 1 < n ? static_cast<unsigned char>(s[i + 1]) : 0;
        if (i + 1 >= n || c1 < lo || c1 > hi) {
            w.push_back(0xFFFD);
            ++i;
            continue;
        }
        size_t got = 1;
        while (got < need && cont(i + 1 + got)) ++got;
        if (got < need) {   // cut-off sequence: one U+FFFD for the maximal subpart
            w.push_back(0xFFFD);
            i += 1 + got;
            continue;
        }
        uint32_t cp = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
        for (size_t k = 1; k <= need; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            w.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            w.push_back(static_cast<wchar_t>(cp));   // includes a lone surrogate stored as WTF-8
        }
        i += 1 + need;
    }
    return w;
}

std::string narrow(std::wstring_view w)
{
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
            ++i;
        }
        appendUtf8(out, c);   // a lone surrogate becomes ED A0..BF xx (WTF-8)
    }
    return out;
}

const char* uvCode(uint32_t winError)
{
    for (const UvErr& e : kUvErrors)
        if (e.win == winError) return e.code;
    return "UNKNOWN";
}

std::string nodeError(uint32_t winError, std::string_view syscall, std::string_view path)
{
    // libuv's scandir turns STATUS_NOT_A_DIRECTORY into ENOTDIR itself; the generic table says ENOENT for 267.
    if (winError == ERROR_DIRECTORY && syscall == "scandir") return nodeErrorCode("ENOTDIR", syscall, path);
    return nodeErrorCode(uvCode(winError), syscall, path);
}

// ---- listing and stat ---------------------------------------------------------------------------------------------

bool listDir(const std::string& dir, const std::function<bool(const DirEntry&)>& cb, uint32_t* winError)
{
    if (winError) *winError = 0;
    std::wstring w;
    if (dir.empty() || !winPath(dir, w)) {
        if (winError) *winError = dir.empty() ? ERROR_PATH_NOT_FOUND : ERROR_INVALID_NAME;
        return false;
    }
    std::wstring pattern = w;
    if (isSepW(pattern.back()) || (pattern.size() == 2 && pattern[1] == L':')) pattern += L"*";   // "X:" = drive cwd
    else pattern += L"\\*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND && isDir(dir)) return true;   // an empty root has no "." entry
        if (winError) *winError = e;
        return false;
    }
    FindCloser closer{h};
    for (;;) {
        const wchar_t* n = fd.cFileName;
        const bool dot = n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0));
        if (!dot) {
            DirEntry e;
            e.name = narrow(n);
            e.attributes = fd.dwFileAttributes;
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            e.isHidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
            e.isSystem = (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
            e.size = e.isDir ? 0 : joinSize(fd.nFileSizeHigh, fd.nFileSizeLow);
            e.mtimeMs = fileTimeMs(fd.ftLastWriteTime);
            e.birthMs = fileTimeMs(fd.ftCreationTime);
            if (!cb(e)) return true;
        }
        if (!FindNextFileW(h, &fd)) {
            const DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_FILES) return true;
            if (winError) *winError = e;   // the listing broke off (network drop): report it like a failed open
            return false;
        }
    }
}

FileStat stat(const std::string& path)
{
    FileStat st;
    std::wstring w;
    if (path.empty() || !winPath(path, w)) return st;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a)) {
        st.exists = true;
        st.attributes = a.dwFileAttributes;
        st.isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        st.isReparse = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        st.size = st.isDir ? 0 : joinSize(a.nFileSizeHigh, a.nFileSizeLow);
        st.mtimeMs = fileTimeMs(a.ftLastWriteTime);
        st.birthMs = fileTimeMs(a.ftCreationTime);
        st.atimeMs = fileTimeMs(a.ftLastAccessTime);
        return st;
    }
    if (GetLastError() != ERROR_SHARING_VIOLATION || w.find_first_of(L"*?") != std::wstring::npos) return st;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return st;
    FindClose(h);
    st.exists = true;
    st.attributes = fd.dwFileAttributes;
    st.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st.isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    st.size = st.isDir ? 0 : joinSize(fd.nFileSizeHigh, fd.nFileSizeLow);
    st.mtimeMs = fileTimeMs(fd.ftLastWriteTime);
    st.birthMs = fileTimeMs(fd.ftCreationTime);
    st.atimeMs = fileTimeMs(fd.ftLastAccessTime);
    return st;
}

bool exists(const std::string& path) { return stat(path).exists; }

bool isDir(const std::string& path)
{
    const FileStat st = stat(path);
    return st.exists && st.isDir;
}

bool isFile(const std::string& path)
{
    const FileStat st = stat(path);
    return st.exists && !st.isDir;
}

// ---- changes ------------------------------------------------------------------------------------------------------

bool makeDirs(const std::string& path, std::string* err)
{
    // fs.mkdirSync(p, {recursive:true}) (Node's MKDirpSync): an existing directory is fine; p itself being a file is
    // EEXIST, an ancestor being a file ENOTDIR, a link whose target is gone ENOENT (Node's stat follows it); the
    // message names p.
    const FileStat top = stat(path);
    if (top.exists && top.isDir && !top.isReparse) return true;
    std::vector<std::string> pending;   // deepest first
    std::string cur = path;
    // Set once the walk turns back down (a parent was made or found). Nothing is pushed after that, so the loop ends.
    bool climbing = false;
    for (;;) {
        std::wstring w;
        if (cur.empty() || !winPath(cur, w)) {
            setErr(err, nodeError(cur.empty() ? ERROR_PATH_NOT_FOUND : ERROR_INVALID_NAME, "mkdir", path));
            return false;
        }
        if (CreateDirectoryW(w.c_str(), nullptr)) {
            if (pending.empty()) return true;
            cur = std::move(pending.back());
            pending.pop_back();
            climbing = true;
            continue;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_ACCESS_DENIED) {
            // ACCESS_DENIED also comes back for an existing protected folder ("C:\"): an existing directory is fine.
            const FileStat st = stat(cur);
            const DWORD linkErr = st.exists && st.isDir && st.isReparse ? followedDirError(w) : 0;
            if (st.exists && st.isDir && linkErr == 0) {
                if (pending.empty()) return true;
                cur = std::move(pending.back());
                pending.pop_back();
                climbing = true;
                continue;
            }
            if (st.exists && (!st.isDir || linkErr == ERROR_DIRECTORY)) {
                setErr(err, nodeErrorCode(pending.empty() ? "EEXIST" : "ENOTDIR", "mkdir", path));
                return false;
            }
            setErr(err, nodeError(linkErr ? linkErr : e, "mkdir", path));
            return false;
        }
        if (e == ERROR_PATH_NOT_FOUND || e == ERROR_FILE_NOT_FOUND || e == ERROR_DIRECTORY) {
            // A child that still cannot be made right after its parent was made or found: Win32 reads that parent
            // under another name than the one it just made. A middle name ending in a space or dot ('Photos ') or
            // made of dots ('...') is kept as typed, the same name as the last component is trimmed ('Photos',
            // '' = the folder above). Retrying would loop forever. Node creates such names literally (its fs calls
            // use \\?\); Win32 semantics, as C#'s Directory.CreateDirectory, fail: ENOENT.
            if (climbing) {
                setErr(err, nodeError(e, "mkdir", path));
                return false;
            }
            // ENOENT in libuv terms: Node goes on with the parent.
            std::string parent = nodepath::dirname(cur);
            if (parent == cur || parent.empty() || parent == ".") {
                setErr(err, nodeError(e, "mkdir", path));
                return false;
            }
            pending.push_back(std::move(cur));
            cur = std::move(parent);
            continue;
        }
        setErr(err, nodeError(e, "mkdir", path));
        return false;
    }
}

bool removeFile(const std::string& path)
{
    std::wstring w;
    if (path.empty() || !winPath(path, w)) return false;
    return deleteEntry(w, false) == 0;
}

bool removeTree(const std::string& path)
{
    const FileStat top = stat(path);
    if (!top.exists) return true;
    std::wstring w;
    if (!winPath(path, w)) return false;
    // Judged on what Win32 resolves the path to (GetFullPathNameW; the NT form \??\x, which CreateFileW takes as it
    // is, read as \\?\x), so no spelling slips past the root guard ("C: ", "C:.", "\\?\C:", "//?/UNC/srv/share/").
    const bool ntForm = w.size() >= 4 && w[0] == L'\\' && w[1] == L'?' && w[2] == L'?' && w[3] == L'\\';
    const std::wstring full = ntForm ? L"\\\\?\\" + w.substr(4) : getFullPathW(w);
    const std::wstring plain = full.empty() ? std::wstring() : plainPath(full);
    // Never empty a whole volume or share: a drive or share root is refused before anything is touched, and so is any
    // device path without a drive or share form (\\?\GLOBALROOT\Device\HarddiskVolume3\, \\?\Volume{...}\): those
    // name a volume root with nothing to compare against. GetVolumePathNameW is no help: it fails on the root of a
    // SUBST / DefineDosDevice drive and reports that drive's first-level folders as mount points.
    if (plain.empty() || isRootPlain(plain)) return false;
    if (!top.isDir || top.isReparse) {
        deleteEntry(w, true);   // a file, or a junction / symlink: the link only
        return !stat(path).exists;
    }
    // Post-order walk on an explicit stack (a deep tree must not overflow a 256 KB worker stack). Children are
    // collected before deleting, so no find handle is open while entries go away. Every entry is addressed by the
    // \\?\ form, so the names listDir returns are opened exactly: 'report ', 'a.', '...' or 'nul' (made by Node or
    // another \\?\ caller) would otherwise be trimmed or mapped by Win32 onto a sibling ('report') or a device.
    struct Frame {
        std::wstring dir;
        bool listed = false;
    };
    std::vector<Frame> stack;
    std::wstring topDir = exactPath(plain);
    while (isSepW(topDir.back())) topDir.pop_back();   // "C:\x\" -> "\\?\C:\x" (never a root: refused above)
    stack.push_back({std::move(topDir), false});
    while (!stack.empty()) {
        if (!stack.back().listed) {
            stack.back().listed = true;
            const std::wstring dir = stack.back().dir;
            std::vector<std::pair<std::string, bool>> children;   // name, descend
            listDir(narrow(dir), [&](const DirEntry& e) {
                children.emplace_back(e.name, e.isDir && !e.isReparse);
                return true;
            });
            for (auto& c : children) {
                std::wstring child = dir;
                if (!isSepW(child.back())) child += L'\\';
                child += widen(c.first);
                if (c.second) stack.push_back({std::move(child), false});
                else deleteEntry(child, true);
            }
            continue;
        }
        for (int attempt = 0; attempt < kBusyRetries; ++attempt) {
            const DWORD e = deleteEntry(stack.back().dir, true);
            if (e != ERROR_DIR_NOT_EMPTY && e != ERROR_SHARING_VIOLATION) break;
            sleepBriefly();   // a child still pending deletion (classic semantics) or briefly held
        }
        stack.pop_back();
    }
    return !stat(path).exists;
}

bool renameFile(const std::string& from, const std::string& to, bool replace, std::string* err)
{
    // fs.renameSync is MoveFileExW(REPLACE_EXISTING). COPY_ALLOWED makes a move across volumes work like C#
    // File.Move (Node would answer EXDEV). Sharing violations from a scanner holding the file get a short retry.
    std::wstring wf, wt;
    if (from.empty() || to.empty() || !winPath(from, wf) || !winPath(to, wt)) {
        setErr(err, nodeError(ERROR_INVALID_NAME, "rename", from) + " -> '" + to + "'");
        return false;
    }
    const DWORD flags = (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_COPY_ALLOWED;
    DWORD e = 0;
    for (int attempt = 0; attempt < kBusyRetries; ++attempt) {
        if (MoveFileExW(wf.c_str(), wt.c_str(), flags)) return true;
        e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) break;
        sleepBriefly();
    }
    setErr(err, nodeError(e, "rename", from) + " -> '" + to + "'");
    return false;
}

bool readFile(const std::string& path, std::string& out, int64_t maxBytes, std::string* err)
{
    out.clear();
    FileReader r;
    if (!r.open(path, err)) return false;
    if (maxBytes >= 0 && r.size() > maxBytes) {
        setErr(err, "file too large: " + std::to_string(r.size()) + " bytes (limit " + std::to_string(maxBytes) +
                        "), read '" + path + "'");
        return false;
    }
    try {
        out.resize(static_cast<size_t>(r.size()));
        size_t got = 0;
        while (got < out.size()) {
            const size_t k = r.read(&out[got], out.size() - got);
            if (k == 0) break;   // the file shrank while reading
            got += k;
        }
        out.resize(got);
        // A file that grew while reading: take what fits under the limit, like a snapshot of the first size bytes.
    } catch (const IoError& e) {
        out.clear();
        setErr(err, e.what());
        return false;
    }
    return true;
}

// ---- names --------------------------------------------------------------------------------------------------------

std::string fullPath(const std::string& path)
{
    if (path.empty() || path.find('\0') != std::string::npos) return {};
    const std::wstring out = getFullPathW(widen(path));
    return out.empty() ? std::string() : narrow(out);
}

std::string longPath(const std::string& path)
{
    // GetLongPathNameW needs the path to exist. For a path whose tail does not exist yet (an upload target), the
    // nearest existing ancestor is expanded and the rest appended, so an 8.3 folder name never hides from the guard.
    if (path.empty() || path.find('\0') != std::string::npos) return path;
    auto expand = [](const std::string& p, std::string& out) {
        const std::wstring w = widen(p);
        DWORD n = GetLongPathNameW(w.c_str(), nullptr, 0);
        if (n == 0) return false;
        std::wstring buf(n, L'\0');
        n = GetLongPathNameW(w.c_str(), &buf[0], n);
        if (n == 0 || n >= buf.size()) return false;
        buf.resize(n);
        out = narrow(buf);
        return true;
    };
    std::string out;
    if (expand(path, out)) return out;
    std::string head = path;
    std::string tail;
    for (int depth = 0; depth < 4096; ++depth) {
        const std::string parent = nodepath::dirname(head);
        if (parent == head || parent.empty() || parent == ".") break;
        const std::string leaf = nodepath::basename(head);
        tail = tail.empty() ? leaf : leaf + "\\" + tail;
        head = parent;
        if (expand(head, out)) {
            if (!out.empty() && out.back() != '\\') out += '\\';
            return out + tail;
        }
    }
    return path;
}

bool isDevicePath(std::string_view p)
{
    // \\?\ and \\.\ (either separator: Win32 reads //./ and //?/ as device paths too) and the NT \??\ form, which
    // CreateFileW passes through untouched.
    auto sep = [](char c) { return c == '\\' || c == '/'; };
    if (p.size() >= 4 && sep(p[0]) && sep(p[1]) && (p[2] == '?' || p[2] == '.') && sep(p[3])) return true;
    return p.size() >= 4 && p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\';
}

bool hasAlternateStream(std::string_view p)
{
    const bool drive = p.size() >= 2 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
    return p.find(':', drive ? 2 : 0) != std::string_view::npos;
}

std::string cwd()
{
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    n = GetCurrentDirectoryW(n, &buf[0]);
    buf.resize(n);
    return narrow(buf);
}

std::string exeDir()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const size_t cut = buf.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return {};
    return narrow(buf.substr(0, cut == 2 && buf[1] == L':' ? 3 : cut));
}

std::string tempDir()
{
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 2, buf);
    if (n == 0 || n > MAX_PATH + 1) return {};
    std::wstring w(buf, n);
    while (w.size() > 3 && isSepW(w.back())) w.pop_back();   // keep "C:\"
    return narrow(w);
}

std::string knownFolder(std::string_view which)
{
    const std::string k = nodepath::toLowerInvariant(which);
    const KNOWNFOLDERID* id = nullptr;
    if (k == "desktop") id = &FOLDERID_Desktop;
    else if (k == "downloads") id = &FOLDERID_Downloads;
    else if (k == "documents") id = &FOLDERID_Documents;
    else if (k == "programdata") id = &FOLDERID_ProgramData;
    else if (k == "profile") id = &FOLDERID_Profile;
    if (!id) return {};
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = narrow(p);
    if (p) CoTaskMemFree(p);
    return out;
}

// ---- drives and elevation -----------------------------------------------------------------------------------------

std::vector<DriveInfo> drives()
{
    // A..Z from GetLogicalDrives. ready = the volume answers both GetVolumeInformationW and GetDiskFreeSpaceExW (what
    // Node's existsSync + statfsSync and C#'s DriveInfo.IsReady + TotalSize need). SEM_FAILCRITICALERRORS keeps an
    // empty card reader from raising a "No disk" dialog on this thread. A disconnected network drive can still take
    // the SMB timeout to answer: callers cache the list (60 s).
    std::vector<DriveInfo> out;
    const DWORD mask = GetLogicalDrives();
    DWORD oldMode = 0;
    const BOOL modeSet = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        DriveInfo d;
        d.letter = std::string(1, static_cast<char>('A' + i)) + ":";
        const wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        d.type = static_cast<int>(GetDriveTypeW(root));
        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fsName[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(root, label, MAX_PATH + 1, nullptr, nullptr, nullptr, fsName, MAX_PATH + 1)) {
            d.label = narrow(label);
            d.fileSystem = narrow(fsName);
            ULARGE_INTEGER avail{}, total{}, totalFree{};
            if (GetDiskFreeSpaceExW(root, &avail, &total, &totalFree)) {
                d.ready = true;
                d.size = static_cast<int64_t>(total.QuadPart);
                d.free = static_cast<int64_t>(avail.QuadPart);
            }
        }
        out.push_back(std::move(d));
    }
    if (modeSet) SetThreadErrorMode(oldMode, nullptr);
    return out;
}

bool isElevated()
{
    // C# PeerAnnouncer.IsElevated: WindowsIdentity.IsSystem || IsInRole(Administrator). CheckTokenMembership with a
    // null token checks the effective token, so a UAC-filtered admin (deny-only Administrators SID) is not elevated.
    static const bool value = [] {
        BOOL member = FALSE;
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        PSID admins = nullptr;
        if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                     &admins)) {
            if (!CheckTokenMembership(nullptr, admins, &member)) member = FALSE;
            FreeSid(admins);
        }
        if (member) return true;
        HANDLE token = nullptr;
        bool system = false;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            DWORD n = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &n);
            std::vector<unsigned char> buf(n ? n : 1);
            if (n && GetTokenInformation(token, TokenUser, buf.data(), n, &n))
                system = IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, WinLocalSystemSid) != FALSE;
            CloseHandle(token);
        }
        return system;
    }();
    return value;
}

// ---- FileReader ---------------------------------------------------------------------------------------------------

FileReader::~FileReader() { close(); }

bool FileReader::open(const std::string& path, std::string* err)
{
    close();
    std::wstring w;
    if (path.empty() || !winPath(path, w)) {
        setErr(err, nodeError(path.empty() ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_NAME, "open", path));
        return false;
    }
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        // A directory cannot be opened without BACKUP_SEMANTICS; Node opens it and fails the read with EISDIR.
        if (e == ERROR_ACCESS_DENIED && isDir(path)) setErr(err, nodeErrorCode("EISDIR", "read", {}));
        else setErr(err, nodeError(e, "open", path));
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        setErr(err, nodeError(e, "fstat", path));
        return false;
    }
    h_ = h;
    size_ = size.QuadPart;
    return true;
}

bool FileReader::seek(int64_t offset)
{
    if (!h_ || offset < 0) return false;
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    return SetFilePointerEx(static_cast<HANDLE>(h_), li, nullptr, FILE_BEGIN) != FALSE;
}

void FileReader::close()
{
    if (h_) CloseHandle(static_cast<HANDLE>(h_));
    h_ = nullptr;
    size_ = 0;
}

size_t FileReader::read(char* buf, size_t n)
{
    if (!h_) throw IoError("read on a closed file");
    if (n == 0) return 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(h_), buf, want, &got, nullptr)) {
        const DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;
        throw IoError(nodeError(e, "read"));
    }
    return got;
}

// ---- FileWriter ---------------------------------------------------------------------------------------------------

FileWriter::~FileWriter()
{
    if (h_ && !done_) discard();
}

bool FileWriter::create(const std::string& path, std::string* err)
{
    if (h_ && !done_) discard();
    h_ = nullptr;
    done_ = false;
    written_ = 0;
    appendBase_ = -1;
    createdNew_ = false;
    path_ = path;
    std::wstring w;
    if (path.empty() || !winPath(path, w)) {
        setErr(err, nodeError(path.empty() ? ERROR_PATH_NOT_FOUND : ERROR_INVALID_NAME, "open", path));
        return false;
    }
    // DELETE access lets discard() remove the file through the handle, even after someone renamed it. A reader that
    // did not share delete makes that open fail: then without DELETE (discard() deletes by name).
    auto openW = [&](DWORD disposition) {
        HANDLE x = CreateFileW(w.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (x == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
            x = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, disposition,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        return x;
    };
    HANDLE h = openW(CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
        // CREATE_ALWAYS refuses an existing hidden or system file unless the attributes are repeated; truncate it.
        const FileStat st = stat(path);
        if (st.exists && !st.isDir && !(st.attributes & FILE_ATTRIBUTE_READONLY) &&
            (st.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            h = openW(TRUNCATE_EXISTING);
        else
            SetLastError(ERROR_ACCESS_DENIED);
    }
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED && isDir(path)) setErr(err, nodeErrorCode("EISDIR", "open", path));
        else setErr(err, nodeError(e, "open", path));
        return false;
    }
    h_ = h;
    return true;
}

bool FileWriter::openAppend(const std::string& path, std::string* err)
{
    if (h_ && !done_) discard();
    h_ = nullptr;
    done_ = false;
    written_ = 0;
    appendBase_ = -1;
    createdNew_ = false;
    path_ = path;
    std::wstring w;
    if (path.empty() || !winPath(path, w)) {
        setErr(err, nodeError(path.empty() ? ERROR_PATH_NOT_FOUND : ERROR_INVALID_NAME, "open", path));
        return false;
    }
    // FILE_APPEND_DATA (without FILE_WRITE_DATA) makes every write land at the end, even with another appender.
    // FILE_WRITE_DATA is not asked, so truncating back on discard() goes through a second, short-lived handle.
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE | DELETE, share, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
        h = CreateFileW(w.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, share, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED && isDir(path)) setErr(err, nodeErrorCode("EISDIR", "open", path));
        else setErr(err, nodeError(e, "open", path));
        return false;
    }
    createdNew_ = GetLastError() != ERROR_ALREADY_EXISTS;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    appendBase_ = size.QuadPart;
    h_ = h;
    return true;
}

void FileWriter::write(const char* p, size_t n)
{
    if (!h_ || done_) throw IoError("write on a closed file");
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
        DWORD put = 0;
        if (!WriteFile(static_cast<HANDLE>(h_), p, chunk, &put, nullptr)) throw IoError(nodeError(GetLastError(), "write"));
        if (put == 0) throw IoError(nodeError(ERROR_WRITE_FAULT, "write"));
        p += put;
        n -= put;
        written_ += put;
    }
}

void FileWriter::flush() {}

bool FileWriter::commit()
{
    if (!h_ || done_) return false;
    const bool ok = CloseHandle(static_cast<HANDLE>(h_)) != FALSE;
    h_ = nullptr;
    done_ = true;
    return ok;
}

void FileWriter::close() { commit(); }

void FileWriter::discard()
{
    if (!h_ || done_) return;
    HANDLE h = static_cast<HANDLE>(h_);
    if (appendBase_ < 0 || createdNew_) {
        // create(), or a file openAppend() made: delete it through the handle (the name may have changed).
        DispositionInfoEx ex{kDispositionDelete | kDispositionPosix};
        if (!SetFileInformationByHandle(h, kFileDispositionInfoEx, &ex, sizeof ex)) {
            FILE_DISPOSITION_INFO di{};
            di.DeleteFile = TRUE;
            if (!SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di)) {
                CloseHandle(h);
                h_ = nullptr;
                done_ = true;
                removeFile(path_);
                return;
            }
        }
        CloseHandle(h);
    } else {
        // An existing file opened for append: cut the appended bytes off again.
        CloseHandle(h);
        std::wstring w;
        if (winPath(path_, w)) {
            HANDLE t = CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (t != INVALID_HANDLE_VALUE) {
                FILE_END_OF_FILE_INFO eof{};
                eof.EndOfFile.QuadPart = appendBase_;
                SetFileInformationByHandle(t, FileEndOfFileInfo, &eof, sizeof eof);
                CloseHandle(t);
            }
        }
    }
    h_ = nullptr;
    done_ = true;
}

} // namespace anfs::fs
