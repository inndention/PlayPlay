#pragma once
// Media probe on libav (ST-17): what a spot, film or ingest step needs to know about a file before it uses it:
// container, duration, the main video stream (size, display width, fps, rotation) and the main audio stream (rate,
// channels). In-process, under the open rules of media/avopen.h (local files only, playlists and script formats
// refused, nested opens refused, deadline and cancel). It replaces spawning ffprobe for the Studio's own decisions; the
// ffprobe-compatible JSON for the Supervisor stays in tools/avkit (PlayPlayAvKit).
//
// Synchronous and thread-safe: call it on a worker (probeAsync), never on the event loop.
#include <QFuture>
#include <QJsonObject>
#include <QString>
#include <atomic>

class QThreadPool;

namespace studio::media {

struct ProbeOptions {
    int timeoutMs = 15000;                     // whole probe (open + stream info + optional scan); 0 = none
    const std::atomic<bool>* cancel = nullptr;
    /// Read every packet to measure the duration (last pts + duration of the longest stream) instead of trusting the
    /// header. For raw streams, broken headers and files still being written; costs a full demux (no decode).
    bool exactDuration = false;
};

struct VideoInfo {
    bool present = false;
    int index = -1;                            // stream index
    QString codec;                             // "h264", "mjpeg", "png" ...
    int width = 0, height = 0;                 // coded size
    int displayWidth = 0;                      // width with the sample aspect ratio applied (even, >= 2), before rotation
    double fps = 0;                            // av_guess_frame_rate; 0 when unknown (still images)
    QString pixFmt;
    /// Clockwise degrees a player turns the picture (0, 90, 180, 270), from the stream's display matrix (a JPEG's EXIF
    /// orientation is per frame and not seen here). FrameSampler applies it to every frame it hands out.
    int rotation = 0;
    qint64 frames = 0;                         // from the header; 0 when not stored
    qint64 bitRate = 0;
};

struct AudioInfo {
    bool present = false;
    int index = -1;
    QString codec;                             // "aac", "pcm_s16le" ...
    int sampleRate = 0, channels = 0;
    QString layout;                            // "stereo", "mono", "5.1" ... ("" when unspecified)
    QString sampleFmt;
    qint64 bitRate = 0;
};

struct MediaInfo {
    bool ok = false;
    QString error;
    QString format;                            // iformat name ("mov,mp4,m4a,3gp,3g2,mj2", "wav", "png_pipe" ...)
    QString formatLongName;
    double durationSec = 0;                    // 0 when unknown (a still image)
    bool durationExact = false;                // measured by exactDuration rather than taken from the header
    qint64 sizeBytes = -1;
    qint64 bitRate = 0;
    int streams = 0;
    bool isImage = false;                      // a single picture (png/jpeg/webp/bmp... pipe demuxers)
    VideoInfo video;
    AudioInfo audio;

    bool hasVideo() const { return video.present; }
    bool hasAudio() const { return audio.present; }
    /// {ok, format, formatLongName, durationSec, durationExact, sizeBytes, bitRate, streams, isImage,
    ///  video{...} | null, audio{...} | null} or {ok:false, error}.
    QJsonObject toJson() const;
};

MediaInfo probe(const QString& path, const ProbeOptions& opts = {});
/// probe() on `pool` (nullptr = QThreadPool::globalInstance()).
QFuture<MediaInfo> probeAsync(const QString& path, const ProbeOptions& opts = {}, QThreadPool* pool = nullptr);

} // namespace studio::media
