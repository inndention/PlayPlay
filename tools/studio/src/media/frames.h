#pragma once
// Frame access for the stills scrub, scene detection and key frames (ST-17): the libav replacement for the browser's
// Mediabunny frame reader (src/ads/compositor.ts openVideo / frames.frame(t) / frames.sampleAt(times, size)).
//
// FrameSampler decodes one video stream and hands out the frame *on screen* at a time t (the last frame whose pts is
// <= t, half a frame of tolerance), scaled:
//   - thumbAt(t, w, h): packed RGB24, stretched to w x h exactly as canvas drawImage(frame, 0, 0, w, h) does, the
//     input of stills.h (signatures, sharpness, exposure);
//   - imageAt(t, maxW, maxH): display aspect (sample aspect applied), fitted inside maxW x maxH, never upscaled.
// Both are turned upright by the display matrix (a phone's portrait clip stored landscape with a rotation flag), as
// the TS CanvasSink and every player do: the frame's own matrix when the decoder attaches one, else the stream's.
// Options::autoRotate = false gives the coded picture. displayWidth()/displayHeight() are the turned size.
// Ascending times are served by decoding forward, so a scan decodes each packet at most once (like the TS sparse
// reader). Any step back seeks to the keyframe before the target. A jump of more than seekAheadSec seeks too, unless
// the stream's index shows that keyframe is not past the frame already decoded (long GOPs: the seek would decode the
// same frames again). Times are seconds from the start of the stream (its first pts); duration() is the length on the
// same clock.
//
// A hard decode error (not a damaged frame the decoder conceals or skips by itself) fails that call; the decoder is
// flushed and the next call goes on from the next packet, and times up to the next good frame fail too rather than
// repeat the last good picture (scrub.ts restarts after a bad frame the same way). After 8 such errors every later call
// fails.
//
// Opening follows media/avopen.h (local files only, playlists refused, deadline and cancel). One sampler per worker
// thread: it is not thread-safe, and every call blocks while it decodes.
#include <QByteArray>
#include <QImage>
#include <QString>
#include <atomic>
#include <memory>
#include <vector>

namespace studio::media {

class FrameSampler {
public:
    struct Options {
        int timeoutMs = 10 * 60 * 1000;        // the sampler's whole life (open + every frame); 0 = none
        const std::atomic<bool>* cancel = nullptr;
        double seekAheadSec = 4.0;             // decode forward up to this far instead of seeking
        int threads = 2;                       // decoder threads (a pool worker should not take every core)
        bool autoRotate = true;                // turn pictures upright by the display matrix
    };

    explicit FrameSampler(const QString& path);
    FrameSampler(const QString& path, const Options& o);
    ~FrameSampler();
    FrameSampler(const FrameSampler&) = delete;
    FrameSampler& operator=(const FrameSampler&) = delete;

    bool ok() const;                           // opened, a video stream and a decoder
    /// Why the open or the last frame failed ("" when the last call worked).
    QString error() const;
    /// The deadline or the cancel flag ended the work; every later call fails at once.
    bool aborted() const;

    double duration() const;                   // seconds; 0 when the file does not say (a still image: 0)
    int width() const;                         // coded size
    int height() const;
    /// Display size: sample aspect applied to the coded width, then sides swapped when the stream's display matrix
    /// turns the picture by 90 / 270 degrees (and autoRotate is on).
    int displayWidth() const;
    int displayHeight() const;
    /// EXIF orientation (1..8) of the stream's display matrix that every frame is turned by (1 = none, or autoRotate
    /// off); probe()'s rotation 90 / 180 / 270 is 6 / 3 / 8 here.
    int orientation() const;
    double fps() const;                        // guessed frame rate (25 when unknown)

    /// Packed RGB24 (w*h*3 bytes) of the frame at t, turned upright, stretched to w x h; empty on failure (error()
    /// says why).
    QByteArray thumbAt(double t, int w, int h);
    /// The frame at t in display aspect, turned upright, fitted inside maxW x maxH (<= 0: no limit on that side),
    /// never upscaled, QImage::Format_RGB888. Null on failure.
    QImage imageAt(double t, int maxW, int maxH);
    /// Seconds (on the duration() clock) of the frame the last successful call used; -1 before the first.
    double lastFrameTime() const;
    /// Frames decoded so far (a measure of how much work a scan did).
    qint64 framesDecoded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// One thumbnail per time (ascending order is the fast path), `failed` counts the times no frame came for.
struct ThumbScan {
    bool ok = false;                           // the file opened and at least one frame came
    QString error;                             // open error, deadline / cancel, or the first decode error
    std::vector<double> times;                 // the times that produced a thumbnail
    std::vector<QByteArray> rgb;               // w*h*3 bytes each, parallel to `times`
    int failed = 0;
    double duration = 0;
    int width = 0, height = 0;                 // coded size of the video
};
ThumbScan sampleThumbs(const QString& path, const std::vector<double>& times, int w, int h,
                       const FrameSampler::Options& o = FrameSampler::Options());

} // namespace studio::media
