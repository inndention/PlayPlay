// core/fsutil.h - Win32 file-system helpers on UTF-8 paths. FROZEN (ANFS-F0); implemented by ANFS-F2a
// (core/fsutil.cpp). No <windows.h> here: handles are opaque.
//
// Listing order is the raw FindFirstFileExW order (NTFS enumeration order, never sorted: CONVENTIONS.md section 2);
// size, times and attributes come from the same call (FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH). Deviation C2:
// entries whose stat would fail in Node (locked files) are listed.
#pragma once

#include "core/stream.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace anfs::fs {

// WTF-8 <-> UTF-16: a lone surrogate in a file name round-trips as ED A0..BF xx (what core/json prints as \udxxx);
// other invalid UTF-8 becomes U+FFFD per maximal subpart.
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// ANFS-F2a addition: libuv's code for a Win32 error ("ENOENT", "EPERM", "EBUSY"; "UNKNOWN" when unmapped) and
// Node's fs error text "<CODE>: <description>, <syscall> '<path>'", the e.message agent.js puts in 500 bodies.
// Every err out-parameter below carries this text.
const char* uvCode(uint32_t winError);
std::string nodeError(uint32_t winError, std::string_view syscall, std::string_view path = {});

struct DirEntry {
    std::string name;
    bool isDir = false;
    bool isReparse = false;        // junction or symlink (FILE_ATTRIBUTE_REPARSE_POINT): walkers skip descending
    bool isHidden = false;
    bool isSystem = false;
    int64_t size = 0;
    int64_t mtimeMs = 0;           // epoch ms
    int64_t birthMs = 0;
    uint32_t attributes = 0;
};

// Calls cb for each entry ("." and ".." skipped) until it returns false. Returns false when the directory cannot be
// opened; winError gets GetLastError() (2/3 missing, 5 denied, 267 not a directory).
bool listDir(const std::string& dir, const std::function<bool(const DirEntry&)>& cb, uint32_t* winError = nullptr);

struct FileStat {
    bool exists = false;
    bool isDir = false;
    bool isReparse = false;
    int64_t size = 0;
    int64_t mtimeMs = 0;
    int64_t birthMs = 0;
    int64_t atimeMs = 0;
    uint32_t attributes = 0;
};
FileStat stat(const std::string& path);           // GetFileAttributesExW; exists=false on any failure
bool exists(const std::string& path);
bool isDir(const std::string& path);
bool isFile(const std::string& path);

bool makeDirs(const std::string& path, std::string* err = nullptr);       // like fs.mkdirSync(p, {recursive:true})
bool removeFile(const std::string& path);
bool removeTree(const std::string& path);         // never follows reparse points
bool renameFile(const std::string& from, const std::string& to, bool replace, std::string* err = nullptr);
bool readFile(const std::string& path, std::string& out, int64_t maxBytes = 64LL << 20, std::string* err = nullptr);

std::string fullPath(const std::string& path);    // GetFullPathNameW
std::string longPath(const std::string& path);    // GetLongPathNameW (8.3 names expanded); input when it fails
bool isDevicePath(std::string_view path);         // \\?\ and \\.\ prefixes (refused by the guards)
bool hasAlternateStream(std::string_view path);   // a ':' after the drive letter (::$DATA, name:stream)
std::string cwd();
std::string exeDir();
std::string tempDir();                            // GetTempPathW without the trailing separator
// SHGetKnownFolderPath: "desktop", "downloads", "documents", "programdata", "profile"; "" when unknown.
std::string knownFolder(std::string_view which);

struct DriveInfo {
    std::string letter;            // "C:"
    std::string label;             // volume label ("" when none)
    std::string fileSystem;        // "NTFS"
    int type = 0;                  // GetDriveTypeW
    bool ready = false;
    int64_t size = 0;              // total bytes
    int64_t free = 0;              // bytes available to the caller
};
std::vector<DriveInfo> drives();                  // GetLogicalDrives order (A..Z); not-ready drives included
bool isElevated();                                // Administrators membership (CheckTokenMembership) or LocalSystem

// Sequential file reader (FILE_SHARE_READ|WRITE|DELETE, FILE_FLAG_SEQUENTIAL_SCAN).
class FileReader final : public Source {
public:
    FileReader() = default;
    ~FileReader() override;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    bool open(const std::string& path, std::string* err = nullptr);
    bool seek(int64_t offset);
    void close();
    bool isOpen() const { return h_ != nullptr; }
    int64_t size() const { return size_; }
    size_t read(char* buf, size_t n) override;    // throws IoError on a read failure
    int64_t length() const override { return size_; }
    void* nativeHandle() const { return h_; }     // HANDLE, for TransmitFile

private:
    void* h_ = nullptr;
    int64_t size_ = 0;
};

// File writer. create() opens a new or truncated file; discard() closes and deletes it (killed connection leaves no
// partial file); commit() flushes and closes. Writes are unbuffered WriteFile calls, so flush() has nothing to do and
// commit() does not force the disk cache (Node's write streams do not fsync either). An openAppend() writer's
// discard() cuts the file back to its size at open (deletes it only when openAppend created it).
class FileWriter final : public Sink {
public:
    FileWriter() = default;
    ~FileWriter() override;                       // discards when neither commit() nor close() ran
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
    bool create(const std::string& path, std::string* err = nullptr);
    bool openAppend(const std::string& path, std::string* err = nullptr);
    using Sink::write;
    void write(const char* p, size_t n) override; // throws IoError
    void flush() override;
    bool commit();
    void close();
    void discard();
    int64_t written() const { return written_; }
    const std::string& path() const { return path_; }

private:
    void* h_ = nullptr;
    std::string path_;
    int64_t written_ = 0;
    bool done_ = false;
    int64_t appendBase_ = -1;                     // openAppend(): size at open; -1 for create() (ANFS-F2a)
    bool createdNew_ = false;                     // openAppend() created the file (ANFS-F2a)
};

} // namespace anfs::fs
