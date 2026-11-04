#ifndef DECODERPIPELINE_H
#define DECODERPIPELINE_H

#include <QObject>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <QThread>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <QJsonObject>
#include "mediaframe.h"
#include "../core/framerate.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
}

/**
 * DecoderPipeline — threaded FFmpeg decoder producing output-ready media.
 *
 * One instance per playlist item. open() seeks to the in-point, spins up a
 * decode thread, and returns once the first picture is ready, so the engine can
 * cut to it on the very next tick. The engine prepares the *next* item's
 * pipeline while the current one plays, then swaps: that is how transitions are
 * gapless.
 *
 * Output side (called from the frame-clock thread, never blocks on I/O):
 *   - videoFrameAt(posSec): the BGRA frame to show at item position posSec.
 *     Source and output rates may differ (24p into 59.94); we pick the newest
 *     decoded frame whose timestamp <= posSec and repeat it until the next is
 *     due. That is plain frame drop/repeat conversion.
 *   - pullAudio(dst, n): exactly n planar-float samples at the output rate.
 *     Audio is resampled on the decode thread into a FIFO aligned to the video
 *     timeline, so pulling per frame keeps lip-sync without a separate clock.
 *
 * Hardware decode uses d3d11va when the codec supports it (Intel Arc / any
 * modern GPU), with transparent fallback to software.
 *
 * Decode-thread cost (PE-12, each with its own switch; docs/plans/perf-engine.md):
 *   - the GPU -> CPU download goes into a pooled NV12 (sw_format) frame instead of a fresh 3 MB allocation per picture
 *     (Config::pooledDownload, env PLAYPLAY_DECODE_POOL=0 restores the per-picture allocation);
 *   - a source faster than the output (>= 1.5x, e.g. 59.94p into 29.97) converts only the pictures videoFrameAt() would
 *     show: each decoded picture is held until the next one arrives, and the output's own pick rule (newest picture whose
 *     position <= tick + a quarter frame, ticks on the output frame grid) decides whether the held one is converted or
 *     counted as dropped before any download / sws work (Config::skipUnshown, env PLAYPLAY_DECODE_SKIP=0);
 *   - hwDecode "auto" (Config::hwAuto, env PLAYPLAY_DECODE_HW=auto|on|off): the first item of a codec + resolution decodes
 *     in hardware and measures its CPU ms per picture over the first hwAutoFrames pictures (decode, download, sws and
 *     codec worker threads together), the second in software, and from then on the cheaper one is used (a mode whose
 *     decode thread cannot keep real time counts as infinitely expensive). hwAutoTable() shows the measurements.
 */
class DecoderPipeline : public QObject {
    Q_OBJECT

public:
    struct Config {
        int outWidth = 1920;
        int outHeight = 1080;
        FrameRate outRate = FrameRate::ntsc();
        int sampleRate = 48000;
        int channels = 2;
        bool hwAccel = true;
        bool hwAuto = false;            // hwAccel is a candidate, not a given: pick hw / sw by measured cost (PE-12)
        int hwAutoFrames = 60;          // pictures measured per item in auto mode
        bool pooledDownload = true;     // hw download into pooled frames (PE-12)
        bool skipUnshown = true;        // never convert a picture videoFrameAt() would drop (PE-12)
        int videoQueueDepth = 8;        // decoded frames buffered ahead
        double audioFifoMaxSec = 6.0;   // back-pressure threshold for audio-heavy files
        int openTimeoutMs = 4000;       // wait for first frame
        bool loop = false;              // restart at in-point on EOF
        QString inputFormat;            // libavdevice/format name ("dshow", "mpegts", ...) or empty
        QJsonObject inputOptions;       // avformat/device options (e.g. rtbufsize, video_size, framerate)
        bool realtime = false;          // live input: no duration, never "at end", free-running audio
    };

    struct Stats {
        qint64 decodedVideo = 0;
        qint64 decodedAudioSamples = 0;
        qint64 droppedVideo = 0;    // decoded but never shown (rate conversion / late)
        qint64 repeatedVideo = 0;   // shown more than once
        qint64 audioUnderruns = 0;  // pulls that had to pad with silence
        qint64 audioSilenceSamples = 0;
        int queueDepth = 0;
        bool hwAccelActive = false;
        QString hwDevice;
        // Per decoded picture, moving averages (ms): GPU->CPU surface download, and colour conversion + scale to the raster.
        double transferMs = 0.0;
        double convertMs = 0.0;
        // PE-12
        qint64 convertedVideo = 0;  // pictures that went through download / sws into the raster pool
        qint64 skippedConvert = 0;  // pictures dropped before conversion (also counted in droppedVideo)
        qint64 downloadAllocs = 0;  // hw download buffers allocated (pool growth, or one per picture without the pool)
        bool downloadPooled = false;
        bool skipActive = false;    // the never-shown skip is on for this item (source >= 1.5x output rate)
        QString hwChoice;           // "hw" / "sw": what this item decodes with
        QString autoReason;         // auto mode: "probe" (measuring this mode), "cheaper", "only" (no hw available); else empty
        double autoCostMs = -1.0;   // auto mode: measured CPU ms per picture of this item (-1 until the window closed)
        bool autoKeepsUp = true;    // auto mode: the decode thread kept real time over the window
    };

    explicit DecoderPipeline(const Config& cfg, QObject* parent = nullptr);
    ~DecoderPipeline() override;

    /// Open `path`, seek to inPointSec, start decoding. outPointSec <= 0 means
    /// play to end. Blocks up to Config::openTimeoutMs for the first picture.
    bool open(const QString& path, double inPointSec = 0.0, double outPointSec = 0.0);
    void close();
    bool isOpen() const { return m_opened.load(); }

    // --- output side (frame-clock thread) ---------------------------------
    FramePtr videoFrameAt(double posSec);
    bool pullAudio(float* const* dst, int nSamples);
    /// Fill an FLTP audio frame (nb_samples/channels already set). Returns false if padded.
    bool pullAudio(const FramePtr& audioFrame);

    /// True once the decoder hit EOF/out-point and every buffered frame was consumed.
    bool atEnd() const;
    /// True once the decoder thread has hit EOF/out-point (buffers may still hold frames).
    bool decoderFinished() const { return m_eof.load(); }

    // --- properties ---------------------------------------------------------
    QString path() const { return m_path; }
    double durationSec() const { return m_durationSec; }      // in->out (or stream) length
    double inPointSec() const { return m_inPointSec; }
    double outPointSec() const { return m_outPointSec; }
    int sourceWidth() const { return m_srcWidth; }
    int sourceHeight() const { return m_srcHeight; }
    FrameRate sourceRate() const { return m_srcRate; }
    bool hasAudio() const { return m_audioStreamIndex >= 0; }
    Stats stats() const;
    QString lastError() const { QMutexLocker l(&m_errMutex); return m_lastError; }

    /// Auto mode measurements per "codec WxH pixfmt": {hwMs, swMs, opens, choice}. Process-wide.
    static QJsonObject hwAutoTable();
    static void resetHwAutoTable();
    /// Would the output show a picture at `posSec` when the next one is at `nextPosSec`? (videoFrameAt's rule on the
    /// output frame grid; exposed for tests.)
    static bool wouldShow(const FrameRate& outRate, double posSec, double nextPosSec);

signals:
    void error(const QString& message);
    void endOfStream();

private:
    struct VideoEntry { FramePtr frame; double posSec; };

    // Decode thread body
    class Worker : public QThread {
    public:
        explicit Worker(DecoderPipeline* p) : m_p(p) {}
        void run() override { m_p->decodeLoop(); }
    private:
        DecoderPipeline* m_p;
    };

    Config m_cfg;
    FramePool m_pool;
    QString m_path;
    double m_inPointSec = 0.0;
    double m_outPointSec = 0.0;
    double m_durationSec = 0.0;
    double m_streamStartSec = 0.0;
    double m_loopOffsetSec = 0.0;     // accumulated item length across loop passes

    // FFmpeg state (decode thread only after open())
    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_vctx = nullptr;
    AVCodecContext* m_actx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;
    SwsContext* m_sws = nullptr;
    int m_cvtSrcW = 0, m_cvtSrcH = 0, m_cvtSrcFmt = -1, m_cvtDstW = 0, m_cvtDstH = 0;   // what m_sws was built for
    SwrContext* m_swr = nullptr;
    AVBufferRef* m_hwDevice = nullptr;
    AVPixelFormat m_hwPixFmt = AV_PIX_FMT_NONE;
    AVPixelFormat m_swsSrcFmt = AV_PIX_FMT_NONE;
    int m_swsSrcW = 0, m_swsSrcH = 0;
    int m_srcWidth = 0, m_srcHeight = 0;
    FrameRate m_srcRate;
    // Letterbox placement of the scaled picture inside the output frame
    int m_dstX = 0, m_dstY = 0, m_dstW = 0, m_dstH = 0;

    // PE-12: pooled hw download frames (the hw frames context's sw_format and allocated size)
    AVBufferPool* m_dlPool = nullptr;
    AVPixelFormat m_dlFmt = AV_PIX_FMT_NONE;
    int m_dlW = 0, m_dlH = 0;
    int m_dlLinesize[4] = {0, 0, 0, 0};
    size_t m_dlOffset[4] = {0, 0, 0, 0};
    bool m_pooledDownload = true;
    // PE-12: never-shown skip (decode thread only)
    bool m_skipActive = false;
    AVFrame* m_held = nullptr;        // the last decoded picture, waiting for the next one's timestamp
    double m_heldPos = 0.0;
    bool m_hasHeld = false;
    // PE-12: hw/sw auto measurement (decode thread after open)
    bool m_hwAuto = false;
    QString m_autoKey;
    bool m_autoMeasuring = false;
    std::unordered_map<unsigned long, double> m_autoBefore;   // thread CPU ms before the decoder existed (ids to exclude)
    std::unordered_map<unsigned long, double> m_autoBase;     // thread CPU ms at the first picture
    qint64 m_autoWallStartNs = 0;
    qint64 m_autoFirstDecoded = 0;    // decodedVideo at the window start
    qint64 m_autoWaitNs = 0;          // back-pressure waits inside the window (not decoder work)

    // Video queue
    mutable QMutex m_vmutex;
    QWaitCondition m_vspace;      // decode thread waits for room
    QWaitCondition m_vfirst;      // open() waits for first frame
    std::deque<VideoEntry> m_vqueue;
    FramePtr m_lastShown;
    double m_lastShownPos = -1.0;
    bool m_lastShownRepeated = false;

    // Audio FIFO (FLTP, m_cfg.channels, m_cfg.sampleRate), aligned to video timeline
    mutable QMutex m_amutex;
    QWaitCondition m_aspace;
    AVAudioFifo* m_afifo = nullptr;
    bool m_audioAligned = false;
    qint64 m_audioSamplesPushed = 0;     // after alignment, in output samples
    qint64 m_audioOutLimit = -1;         // sample count at out-point (-1 none)

    // Lifecycle
    Worker* m_worker = nullptr;
    std::atomic<bool> m_opened{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_eof{false};
    std::atomic<bool> m_firstFrameReady{false};

    // Stats
    mutable QMutex m_statMutex;
    Stats m_stats;
    mutable QMutex m_errMutex;
    QString m_lastError;

    bool openInput(const QString& path);
    bool openVideoDecoder();
    bool openAudioDecoder();
    bool seekTo(double absSec);
    void decodeLoop();
    bool handleVideoFrame(AVFrame* frame);
    bool handleAudioFrame(AVFrame* frame);
    bool pushVideo(FramePtr frame, double posSec);
    void pushAudioSamples(uint8_t** planes, int nSamples);
    void pushAudioSilence(qint64 nSamples);
    FramePtr convertToOutput(AVFrame* src);
    FramePtr downloadFrame(AVFrame* hw);
    bool flushHeld();
    void autoWindowStart();
    void autoWindowClose();
    void freeFfmpeg();
    void setError(const QString& msg);

    static AVPixelFormat getHwFormat(AVCodecContext* ctx, const AVPixelFormat* fmts);
};

#endif // DECODERPIPELINE_H
