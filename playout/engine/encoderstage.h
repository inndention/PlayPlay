#ifndef ENCODERSTAGE_H
#define ENCODERSTAGE_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QList>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <atomic>
#include <deque>
#include "mediaframe.h"
#include "sinks/sinks.h"
#include "../core/framerate.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/audio_fifo.h>
}

/**
 * EncoderProfile — one encode configuration. Every PacketSink attached to the
 * same EncoderStage receives identical packets, so ASI + UDP + file all cost
 * one encode.
 *
 * Presets (EncoderProfile::preset / `preset` in JSON):
 *   "mpeg2_ac3" / "mpeg2_ac3_atsc"  ATSC 1.0 main service: mpeg2video MP@HL CBR + AC-3 (legacy name)
 *   "h264_aac" / "h264_aac_low"     libx264 + AAC-LC (cable/IPTV/stream, legacy names)
 *   "atsc_mpeg2_ac3_hd"             mpeg2video 1080i MP@HL 12 Mb/s CBR, AC-3 384 k, dialnorm -27
 *   "atsc_mpeg2_ac3_sd"             mpeg2video 480i MP@ML 4 Mb/s CBR, AC-3 192 k
 *   "atsc_h264_ac3"                 libx264 high@4.1 6 Mb/s CBR, closed GOP 1 s, AC-3 384 k
 *   "stream_h264_aac_1080p"         libx264 high 6 Mb/s VBR (max 7, buf 12), AAC-LC 160 k
 *   "stream_h264_aac_720p"          libx264 main 3.5 Mb/s VBR, AAC-LC 128 k
 *   "stream_hevc_aac"               libx265 main 4 Mb/s VBR, AAC-LC 128 k
 *   "qsv_h264_aac"                  h264_qsv 6 Mb/s CBR (falls back to libx264 when the FFmpeg build lacks QSV)
 *
 * Rate control: `rateControl` cbr (minrate = maxrate = bitrate, HRD signalled), vbr (bitrate with
 * maxrate/bufsize) or cqp (`qp`). Broadcast GOP: `keyintMin` (0 = gop), `scenecut` off, `closedGop`.
 * Hardware encoders (h264_qsv/hevc_qsv/h264_nvenc/hevc_nvenc) get their option names mapped from the
 * same fields; when the codec is missing at runtime the stage falls back to libx264/libx265 with a
 * warning (`actualVideoCodec()` tells the UI which one is really running).
 *
 * `width`/`height` (0 = canvas) scale the program picture before encoding, so a mux service can run
 * at 1280x720 while the channel canvas is 1080p. `interlaced` is only honoured on >= 1080-line output.
 * `loudnessTargetLkfs` is informational (ATSC A/85: -24 LKFS; the compositor's loudness meter shows the
 * program level); `dialnorm` is what the AC-3 bitstream signals.
 */
struct EncoderProfile {
    QString name = "h264_aac";
    QString videoCodec = "libx264";      // libx264 | libx265 | mpeg2video | h264_qsv | hevc_qsv | h264_nvenc | hevc_nvenc
    int width = 0, height = 0;           // encoded picture size; 0 = channel canvas
    int videoBitrateKbps = 8000;
    int maxrateKbps = 0;                 // 0 = CBR at videoBitrate
    int bufsizeKbps = 0;                 // 0 = 2x bitrate (mpeg2: MP@HL VBV 1835 kbit)
    QString rateControl = "cbr";         // cbr | vbr | cqp
    int qp = 23;                         // cqp quantiser
    int gopFrames = 0;                   // 0 = 1 second
    int keyintMin = 0;                   // 0 = gopFrames (fixed cadence)
    bool scenecut = false;               // broadcast: off (fixed GOP for splicers and the statmux)
    bool closedGop = true;
    int bFrames = 2;
    QString x264Preset = "veryfast";     // speed preset (x264/x265 names; mapped to p1..p7 for NVENC)
    QString profile = "high";            // h264: high|main|baseline ; hevc: main ; mpeg2: main (level from height)
    QString level;                       // h264/hevc level, e.g. "4.1" (empty = encoder default)
    bool interlaced = false;             // 1080i: field coding + alternate scan (mpeg2) / tff (x264)
    bool ccPassthrough = true;           // a53cc: keep CEA-708 captions when the source carries them
    int afd = -1;                        // Active Format Description code (stored; signalled on the SDI/VANC path)
    QString pixelFormat = "yuv420p";
    int threads = 0;                     // 0 = auto (libx264/libx265: min(12, logical CPUs / 2), see EncoderStage::videoThreadCount)
    QString audioCodec = "aac";          // aac | ac3 | mp2 | eac3
    int audioBitrateKbps = 192;
    int audioChannels = 2;
    int audioSampleRate = 48000;
    int dialnorm = -27;                  // ac3/eac3 dialogue level (dB)
    int mixingLevel = -1;                // ac3 mixing_level (-1 = not signalled)
    QString aacProfile = "aac_low";      // aac_low | aac_he | aac_he_v2 | aac_ld | aac_eld
    double loudnessTargetLkfs = -24.0;   // informational (ATSC A/85)
    QJsonObject extra;                   // passed to avcodec as options (video encoder)

    bool globalHeader = false;          // file containers such as MKV / MP4
    static EncoderProfile fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    static EncoderProfile preset(const QString& name);
    static QStringList presetNames();
    /// Catalogue for UIs: [{id, label, videoCodec, videoBitrateKbps, audioCodec, audioBitrateKbps, ...}]
    static QJsonArray presetCatalogue();
    bool isCbr() const { return rateControl == "cbr" || (rateControl.isEmpty() && (maxrateKbps == 0 || maxrateKbps == videoBitrateKbps)); }
};

/**
 * EncoderStage — encodes program output on its own thread and fans packets out
 * to any number of PacketSinks.
 *
 * Input: MediaFrame (BGRA + FLTP audio) pushed from the frame-clock thread into a
 * bounded queue. If the encoder falls behind, the oldest frames are dropped and
 * counted; the engine never blocks on an output.
 *
 * Colour conversion BGRA -> YUV420P (and the optional down-scale) happens once here
 * per profile. Audio is accumulated into encoder-sized frames (AAC 1024, AC-3 1536,
 * MP2 1152) with a sample-accurate PTS so A/V stay locked.
 *
 * Hot path (perf-engine PE-09; switches are read once in initialize()):
 *  - every converted picture gets a fresh buffer from an AVBufferPool and a repeat re-sends a reference to the last
 *    good picture, so a codec that still holds the previous picture (B-frame reordering, lookahead) never costs an
 *    allocation + copy (av_frame_make_writable). PLAYPLAY_ENC_LEGACY_FRAMES=1 restores the old single-frame path.
 *  - libx264/libx265 with `threads` 0 get min(12, logical CPUs / 2) threads instead of the codec's own count
 *    (1.5 x 32 logical CPUs here: ~50 threads, ~1.1 GB and ~1.5 s of latency); for libx265 that is the worker pool
 *    (pools=N) and x265 derives its frame threads from it. A profile's `threads` always wins;
 *    PLAYPLAY_ENC_THREADS=auto restores the codec's own count, PLAYPLAY_ENC_THREADS=<n> sets n.
 *  - PLAYPLAY_ENC_FAST_RND=1 drops SWS_ACCURATE_RND from the RGB -> YUV conversion (default off: bit-exact as before).
 */
class EncoderStage : public QThread {
    Q_OBJECT

public:
    struct Stats {
        qint64 framesIn = 0;
        qint64 framesEncoded = 0;
        qint64 framesDropped = 0;
        qint64 videoPackets = 0;
        qint64 audioPackets = 0;
        qint64 videoBytes = 0;
        qint64 audioBytes = 0;
        double lastEncodeMs = 0.0;
        double avgEncodeMs = 0.0;
        int queueDepth = 0;
        qint64 videoRescaled = 0;    // pictures not at the canvas size / format (scaled by the fallback scaler)
        qint64 videoRepeated = 0;    // pictures that could not be used: the previous one was encoded again to keep A/V in step
        qint64 audioPaddedSamples = 0;   // silence inserted because audio fell more than two frames behind the video clock
        qint64 audioDroppedSamples = 0;  // samples trimmed because audio ran more than four frames ahead of it
        qint64 yuvAllocs = 0;        // picture buffers allocated: pooled = pool growth only; legacy = the first + every copy below
        qint64 yuvCopies = 0;        // legacy path: pictures av_frame_make_writable reallocated + copied (0 with the pool)
        qint64 yuvCodecHeld = 0;     // frames that found the previous picture still referenced by the codec (legacy: a copy each)
    };

    EncoderStage(const EncoderProfile& profile, int width, int height, const FrameRate& rate,
                 QObject* parent = nullptr);
    ~EncoderStage() override;

    bool initialize();
    void shutdown(bool drain = false);
    bool isActive() const { return m_running.load(); }

    const EncoderProfile& profile() const { return m_profile; }
    /// The encoder really opened (differs from profile().videoCodec after a hardware fallback).
    QString actualVideoCodec() const { return m_actualVideoCodec; }
    QString actualAudioCodec() const { return m_actualAudioCodec; }
    int outputWidth() const { return m_outW; }
    int outputHeight() const { return m_outH; }
    /// PE-09 settings in effect after initialize(): the video codec's thread count (0 = the codec's own count; libx264:
    /// thread_count; libx265 with profile threads 0: its worker pool, pools=N), pooled pictures (false with
    /// PLAYPLAY_ENC_LEGACY_FRAMES=1 or a format the pool cannot lay out), fast rounding.
    int videoThreads() const { return m_videoThreads; }
    bool pooledPictures() const { return m_pooled; }
    bool fastRounding() const { return m_fastRnd; }
    /// The thread_count PE-09 gives a video codec: a non-zero profile `threads` as is; for libx264/libx265 with 0,
    /// `env` (PLAYPLAY_ENC_THREADS) "auto"/"0" = 0 (the codec's own count), "<n>" = n, else min(12, logicalCpus / 2)
    /// (at least 1); other codecs keep 0 (their own default).
    static int videoThreadCount(const QString& codecName, int profileThreads, int logicalCpus, const QString& env);

    /// Attach a sink; starts it with the encoder parameters (so call after initialize()).
    bool addSink(PacketSink* sink);
    void removeSink(PacketSink* sink);
    QList<PacketSink*> sinks() const { QMutexLocker l(&m_sinkMutex); return m_sinks; }
    int sinkCount() const { QMutexLocker l(&m_sinkMutex); return m_sinks.size(); }

    /// Frame-clock thread: enqueue one output frame. Never blocks.
    void pushFrame(const MediaFrame& frame);

    Stats stats() const;
    QString lastError() const { QMutexLocker l(&m_errMutex); return m_lastError; }

    const AVCodecParameters* videoParams() const { return m_vpar; }
    const AVCodecParameters* audioParams() const { return m_apar; }
    AVRational videoTimeBase() const { return m_vctx ? m_vctx->time_base : AVRational{1, 1}; }
    AVRational audioTimeBase() const { return m_actx ? m_actx->time_base : AVRational{1, 1}; }

signals:
    void encoderError(const QString& message);

protected:
    void run() override;

private:
    std::atomic<bool> m_drainOnStop{false};
    EncoderProfile m_profile;
    int m_width, m_height;               // canvas (input) size
    int m_outW, m_outH;                  // encoded size
    FrameRate m_rate;
    QString m_actualVideoCodec, m_actualAudioCodec;

    AVCodecContext* m_vctx = nullptr;
    AVCodecContext* m_actx = nullptr;
    AVCodecParameters* m_vpar = nullptr;
    AVCodecParameters* m_apar = nullptr;
    SwsContext* m_sws = nullptr;
    SwsContext* m_swsAlt = nullptr;      // fallback for pictures of another size / format than the canvas
    int m_altW = 0, m_altH = 0, m_altFmt = -1;
    AVFrame* m_yuv = nullptr;            // the picture sent next (pooled: a fresh pool buffer, or a reference to m_lastYuv)
    AVFrame* m_lastYuv = nullptr;        // pooled: the last good picture, re-sent by reference for repeats
    AVBufferPool* m_yuvPool = nullptr;   // pooled: one buffer per picture, laid out by m_poolLinesize / m_poolOffset
    int m_poolLinesize[4] = {0, 0, 0, 0};
    size_t m_poolOffset[4] = {0, 0, 0, 0};
    std::atomic<qint64> m_poolAllocs{0};
    bool m_pooled = true;
    bool m_fastRnd = false;
    int m_videoThreads = 0;
    AVFrame* m_aframe = nullptr;
    AVAudioFifo* m_afifo = nullptr;
    AVPacket* m_pkt = nullptr;
    qint64 m_videoPts = 0;
    std::atomic<int> m_skipped{0};       // frames dropped from the queue since the last encode: their time is skipped, not lost
    qint64 m_audioPts = 0;         // in samples

    mutable QMutex m_qmutex;
    QWaitCondition m_qcond;
    std::deque<MediaFrame> m_queue;
    static constexpr int kMaxQueue = 6;

    mutable QMutex m_sinkMutex;
    QList<PacketSink*> m_sinks;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    mutable QMutex m_statMutex;
    Stats m_stats;
    mutable QMutex m_errMutex;
    QString m_lastError;

    bool openVideoEncoder();
    bool openAudioEncoder();
    const AVCodec* resolveVideoCodec();
    void applyVideoOptions(const QString& cname, AVDictionary** opts, bool interlaced);
    void encodeFrame(const MediaFrame& frame);
    void encodeVideo(const FramePtr& video);
    bool convertPicture(const FramePtr& video);  // into m_yuv: the canvas scaler, or the fallback scaler for other sizes
    bool setupYuvPool();
    bool takePooledPicture();                    // m_yuv := a fresh pool buffer nobody else references
    static AVBufferRef* poolAlloc(void* opaque, size_t size);
    void encodeAudio(const FramePtr& audio);    // audio may be null: the audio clock is still kept on the video clock
    void writeAudio(const FramePtr& audio);     // samples into the FIFO (converted when the codec wants another format)
    void drainPackets(AVCodecContext* ctx, int streamKind);
    void flush();
    void freeAll();
    void setError(const QString& msg);
};

#endif // ENCODERSTAGE_H
