#ifndef NDISINK_H
#define NDISINK_H

#include "sinks.h"
#include <QThread>
#include <QWaitCondition>
#include <deque>
#include <memory>
#include <vector>

#ifdef HAS_NDI
#include <Processing.NDI.Lib.h>
#endif

struct SwsContext;

/**
 * NdiUyvyConverter — BGRA program picture -> UYVY 4:2:2 for NDI (PE-11), with a threaded swscale.
 *
 * Matrix: BT.709 limited range from 720 lines up, BT.601 limited below, which is what NDI assumes for UYVY by
 * resolution (it carries no colour metadata for it). Output pictures come from a small ring; a ring picture is reused
 * only when nobody else holds it, so the one NDI's async send may still be reading (the sink's m_inFlight) is never
 * overwritten. Not thread-safe: one converter per sender thread.
 */
class NdiUyvyConverter {
public:
    NdiUyvyConverter() = default;
    ~NdiUyvyConverter();
    NdiUyvyConverter(const NdiUyvyConverter&) = delete;
    NdiUyvyConverter& operator=(const NdiUyvyConverter&) = delete;

    /// Converts one BGRA picture (even width). Null on failure (odd width, other format, sws error).
    FramePtr convert(const AVFrame* bgra);
    int threads() const { return m_threads; }
    double lastMs() const { return m_lastMs; }
    static int defaultThreads();      // PLAYPLAY_NDI_SWS_THREADS, else a quarter of the CPUs / 2, 1..4

private:
    SwsContext* m_sws = nullptr;
    int m_w = 0, m_h = 0, m_threads = 0;
    double m_lastMs = 0.0;
    std::vector<FramePtr> m_ring;     // at most 3 pictures
    bool reinit(int w, int h);
};

/**
 * NdiSink — NDI 6 sender for program video + audio.
 *
 * Video is sent with the asynchronous API (zero-copy: NDI reads our buffer
 * until the next send), so we hold a reference to the previous frame until the
 * following call returns. Audio uses planar float (FLTP) which is exactly what
 * our MediaFrame carries.
 *
 * Video FourCC (PE-11): BGRX by default — the program raster is opaque, and BGRA made NDI encode an alpha plane for
 * nothing. "uyvy" converts on this sink's own thread (NdiUyvyConverter) so NDI skips its internal RGB->YCbCr step;
 * "bgra" is the pre-PE-11 behaviour. Chosen per output by options.fourcc, else by PLAYPLAY_NDI_FOURCC, else bgrx.
 *
 * Runs on its own thread with a 3-deep queue; if NDI stalls the newest frames
 * win and drops are counted.
 */
class NdiSink : public BasebandSink {
    Q_OBJECT
public:
    enum class VideoFourcc { Bgra, Bgrx, Uyvy };

    explicit NdiSink(const QString& name, const QString& ndiSourceName = QString(),
                     const QString& groups = QString(), QObject* parent = nullptr);
    ~NdiSink() override;

    QString type() const override { return "ndi"; }
    static bool isAvailable();

    /// "bgra" | "bgrx" | "uyvy" (case-insensitive); anything else -> *ok = false and Bgrx.
    static VideoFourcc parseFourcc(const QString& s, bool* ok = nullptr);
    static QString fourccName(VideoFourcc f);
    /// The output's options.fourcc; empty keeps PLAYPLAY_NDI_FOURCC / the default. Takes effect at the next start().
    void setFourcc(const QString& requested);
    VideoFourcc fourcc() const { return m_fourcc; }

    bool start(const Format& fmt) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }
    void pushFrame(const MediaFrame& frame) override;

private:
    class Worker : public QThread {
    public:
        explicit Worker(NdiSink* s) : m_s(s) {}
        void run() override { m_s->sendLoop(); }
    private:
        NdiSink* m_s;
    };

    QString m_sourceName;
    QString m_groups;
    Format m_fmt;

    QMutex m_qmutex;
    QWaitCondition m_qcond;
    std::deque<MediaFrame> m_queue;
    static constexpr int kMaxQueue = 3;

    Worker* m_worker = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    VideoFourcc m_fourcc = VideoFourcc::Bgrx;

#ifdef HAS_NDI
    NDIlib_send_instance_t m_send = nullptr;
    FramePtr m_inFlight;                 // buffer NDI may still be reading (the program picture or a UYVY ring picture)
    std::vector<float> m_audioScratch;   // contiguous planar audio for NDI
    std::unique_ptr<NdiUyvyConverter> m_uyvy;   // sender thread only
    QString m_sentFourcc;                // what the last video frame went out as (a fallback can differ from m_fourcc)
    double m_convMsAvg = 0.0, m_convMsMax = 0.0;
    qint64 m_convFrames = 0;
#endif

    void sendLoop();
    void sendOne(const MediaFrame& f);
};

#endif // NDISINK_H
