// core/zipstream.h - streaming ZIP writer with zip64 (replaces archiver / ZipArchive). FROZEN (ANFS-F0);
// implemented by ANFS-F2b (core/zipstream.cpp).
//
// The sink is never seeked: each entry is a local header with the data-descriptor flag, the data, then a zip64
// data descriptor when needed; finish() writes the central directory, plus the zip64 end records when any size,
// offset or the entry count overflows. Names are UTF-8 with '/' separators (general-purpose flag bit 11).
// Deflate uses zlib when the build has it (ANFS_HAVE_ZLIB); otherwise every entry is stored.
//
// ANFS-F2b notes (comment-only; the layout rules are in zipstream.cpp):
// - An entry whose data ends within 64 KB is written whole (real CRC and sizes in the local header, no descriptor);
//   deflate that would not shrink it is replaced by store. Longer entries stream with a descriptor; the local header
//   carries a zip64 field when Source::length() announces >= 0xF0000000 bytes; unknown or outgrown lengths switch the
//   descriptor and central sizes to zip64 afterwards.
// - Names: '\' -> '/', empty / '.' / '..' segments dropped, every "X:" prefix dropped until a segment is kept
//   ("./C:/x", "C:/D:/x", "d:x" -> "x"), invalid UTF-8 -> U+FFFD; a name that ends up empty or over 65535 bytes
//   throws std::invalid_argument and writes nothing.
// - addFile reads up to 64 KB ahead before it writes anything: a source failure there (an unreadable file) throws with
//   bytesWritten() unchanged and leaves the archive usable, so the caller may skip that file - but only a plain
//   IoError: one with peerGone or timeout (a cancel, a stalled network source) must abort the archive. A failure
//   after that, or any sink failure, has moved bytesWritten() and breaks the writer: every later call throws IoError
//   (peerGone/timeout of the first error kept) and finish() is impossible.
//   Calls after finish() throw std::logic_error.
// - Output is gathered and handed to the sink in 64 KB pieces; finish() writes the rest and calls Sink::flush().
//   bytesWritten() counts archive bytes produced (up to 64 KB may still be buffered before finish()).
// - mtimeMs: DOS date/time in local time (1980..2107, clamped) plus an Info-ZIP UT field with the UTC seconds.
#pragma once

#include "core/stream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace anfs {

class ZipWriter {
public:
    enum class Method { Store, Deflate };

    explicit ZipWriter(Sink& out, int deflateLevel = 5);
    ~ZipWriter();                                 // does not finish: an unfinished archive is the caller's abort
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    void addDirectory(std::string_view nameInZip, int64_t mtimeMs);
    // Streams data into one entry. Throws IoError when the source or the sink fails.
    void addFile(std::string_view nameInZip, Source& data, int64_t mtimeMs, Method method = Method::Deflate);
    void finish();

    int64_t bytesWritten() const;
    int entries() const;
    static bool deflateAvailable();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace anfs
