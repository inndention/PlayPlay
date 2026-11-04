#pragma once
// libav plumbing shared by the Studio media utilities (ST-17): opening untrusted input, deadlines and cancel, RAII.
//
// The Studio is not on air, but it opens files that came from uploads, scrapes and model output, so every open follows
// the same rules:
//   - One input only. We open the file (or wrap a memory buffer) as an AVIOContext ourselves, with
//     protocol_whitelist=file, and hand it to the demuxer as custom IO. AVFormatContext::io_open refuses every nested
//     open, so an HLS/DASH playlist or a mov data reference can never make libav read another file or a URL.
//   - The format is probed on that AVIOContext, with the file name as a hint. Formats that open their own files or run
//     other code are refused by name before their read_header runs: every AVFMT_NOFILE demuxer, plus image2 (it reads
//     the name, not the bytes: sequence patterns), concat, hls, dash, imf, sdp, rtsp, rtp, avisynth, vapoursynth,
//     webm_dash_manifest (refusedFormat()). When only the NAME lands on a refused demuxer (image2 claims every .jpg
//     over a JPEG's own probe score) the bytes alone decide in a second probe; what the bytes pick is checked again.
//     The concat demuxer is the reason the io_open guard alone is not enough: it opens its entries through a nested
//     avformat_open_input of its own.
//   - A Deadline (timeout plus an optional cancel flag) stops blocking I/O through the interrupt callback, and the
//     decode loops check it too.
// Everything here is synchronous and thread-safe per call (each call owns its contexts): run it on a worker thread
// (QThreadPool), never on an event-loop thread.
#include <QByteArray>
#include <QString>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;
struct AVStream;
struct SwrContext;
struct SwsContext;

namespace studio::media::av {

/// av_strerror text.
QString errText(int averror);

/// Timeout plus optional cancel flag. expired() is cheap (one atomic load and one steady clock read) and sticky.
class Deadline {
public:
    Deadline() = default;
    Deadline(int timeoutMs, const std::atomic<bool>* cancel);
    bool expired();
    bool timedOut() const { return m_timedOut; }
    bool cancelled() const { return m_cancelled; }
    /// "cancelled" | "timed out after N ms" | "" while not expired.
    QString reason() const;

private:
    int m_timeoutMs = 0;
    const std::atomic<bool>* m_cancel = nullptr;
    std::chrono::steady_clock::time_point m_end{};
    bool m_timedOut = false, m_cancelled = false;
};

/// True for demuxers the media utilities never run (see the header comment). `name` may be the comma list of an
/// AVInputFormat ("mov,mp4,m4a,..."): any refused token refuses it. `noFile` = the format has AVFMT_NOFILE.
bool refusedFormat(const char* name, bool noFile = false);

/// An opened input: the demuxer plus the AVIOContext we own. Closing order (demuxer first, then our IO) is handled here.
class Input {
public:
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    AVFormatContext* fc() const { return m_fc; }
    /// iformat->name ("mov,mp4,m4a,3gp,3g2,mj2", "wav", "png_pipe" ...).
    QString formatName() const;
    /// Size of the input in bytes (-1 when unknown).
    qint64 size() const { return m_size; }

    /// Read position over a memory buffer (the AVIOContext callbacks of openMemory).
    struct Mem { QByteArray bytes; qint64 pos = 0; };

private:
    friend std::unique_ptr<Input> openFile(const QString&, Deadline*, QString*, bool);
    friend std::unique_ptr<Input> openMemory(const QByteArray&, Deadline*, QString*, bool);
    Input() = default;
    /// Probe on m_pb, refuse dangerous formats, open the demuxer with that format forced (m_fc owns it from then on).
    bool finish(const QByteArray& url, Deadline* dl, QString* error, bool findStreamInfo);
    AVFormatContext* m_fc = nullptr;
    AVIOContext* m_pb = nullptr;
    bool m_memory = false;
    std::unique_ptr<Mem> m_mem;
    qint64 m_size = -1;
};

/// Opens a local file under the rules above (`findStreamInfo`: also run avformat_find_stream_info). A path that looks
/// like a URL (has "://") is refused: local files only. nullptr and `error` on failure; `dl` may be nullptr.
std::unique_ptr<Input> openFile(const QString& path, Deadline* dl, QString* error, bool findStreamInfo = true);
/// The same for bytes in memory (uploaded images, downloaded results). The buffer is shared, not copied.
std::unique_ptr<Input> openMemory(const QByteArray& bytes, Deadline* dl, QString* error, bool findStreamInfo = true);

struct CodecFree { void operator()(AVCodecContext* c) const; };
struct FrameFree { void operator()(AVFrame* f) const; };
struct PacketFree { void operator()(AVPacket* p) const; };
struct SwsFree { void operator()(SwsContext* s) const; };
struct SwrFree { void operator()(SwrContext* s) const; };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFree>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using SwsPtr = std::unique_ptr<SwsContext, SwsFree>;
using SwrPtr = std::unique_ptr<SwrContext, SwrFree>;
FramePtr newFrame();
PacketPtr newPacket();

/// A decoder for `st` (pkt_timebase set; `threads` 0 = libavcodec's choice). `maxPixels` > 0 sets the decoder's
/// max_pixels, so a picture over it is refused from its header before any buffer is allocated. nullptr and `error`
/// on failure.
CodecPtr openDecoder(AVStream* st, int threads, QString* error, qint64 maxPixels = 0);

/// The EXIF orientation (1..8) of a display matrix (AV_PKT_DATA_DISPLAYMATRIX / AV_FRAME_DATA_DISPLAYMATRIX side
/// data, 9 int32): how the picture is turned / mirrored to show it upright, as ffmpeg's autorotate and browsers do.
/// 1 for no matrix, a short buffer or a singular matrix.
int displayOrientation(const unsigned char* matrix, size_t size);

} // namespace studio::media::av
