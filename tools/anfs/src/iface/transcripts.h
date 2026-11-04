// iface/transcripts.h - transcript lookup and storage (mod-transcripts.md, mod-media-lib.md), and the optional
// OpenRouter fallback of /api/transcribe (w3-ai, off by default because it fabricates). FROZEN (ANFS-F0).
//   ITranscriptSource  defined for media-lib (detail "transcript"); published by transcripts
//   ITranscripts       transcripts (svcSet<ITranscripts> and svcSet<ITranscriptSource>)
//   ITranscribeFallback w3-ai
#pragma once

#include "core/json.h"

#include <stdexcept>
#include <string>

namespace anfs {

// runWhisper(): the Whisper script is missing. Callers treat the item as skipped.
class TranscribeNotAvailable : public std::runtime_error {
public:
    explicit TranscribeNotAvailable(const std::string& what) : std::runtime_error(what) {}
};

class ITranscriptSource {
public:
    virtual ~ITranscriptSource() = default;
    virtual JVal forHash(const std::string& hash) const = 0;      // the stored object, or null
};

class ITranscripts : public ITranscriptSource {
public:
    virtual std::string forFile(const std::string& nodeId, const std::string& path) const = 0;   // "" when none
    // Upserts file_transcripts; when IIndexStore::hashOf(nodeId, path) is known also media_transcripts and fts_files.
    virtual void save(const std::string& nodeId, const std::string& path, const std::string& text) = 0;
    // Local Whisper (python "<script>" "<path>", hidden, 300 s, stdout max 10 MB). Returns the transcript text or a
    // {text, segments} JSON document. Throws TranscribeNotAvailable (no script) or std::runtime_error (failure).
    virtual std::string runWhisper(const std::string& localPath) = 0;
};

class ITranscribeFallback {
public:
    virtual ~ITranscribeFallback() = default;
    // Called by POST /api/transcribe after a local failure; returns the route body
    // {success:true, transcript, local:false} or throws std::runtime_error with the error text.
    virtual JVal transcribe(const std::string& nodeId, const std::string& filePath, const std::string& localError) = 0;
};

} // namespace anfs
