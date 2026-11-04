// PlayPlayMediaTool graph runner: the stream-copy half (DEP-05b). Two command lines of the render farm never encode:
//   the concat join    -f concat -safe 0 -i list.txt -c copy -movflags +faststart out.mp4      (RenderFarm::concatSegments)
//   the copy segment   -ss t -t d -i in -c copy -avoid_negative_ts make_zero seg.mp4           (preset "copy")
// Both are a packet remux through libavformat, as ffmpeg does them: the input's "best" video and audio stream (ffmpeg's
// automatic mapping: largest picture / most channels, the default disposition breaking ties) are copied with their
// parameters, timestamps rebased by the input's start time and -ss (the packets from the keyframe before -ss are kept,
// as ffmpeg keeps them for a copy; -avoid_negative_ts make_zero then shifts the first one to 0 in the muxer), the input
// stopped at the first packet at or past -t. The concat demuxer makes the entries' timestamps continuous itself.
// The MuxWriter below is what graphrunner.cpp writes through as well: one place opens the output (-y / -n, +faststart,
// avoid_negative_ts), adds streams, rescales and interleaves packets, keeps the -progress counters and closes the file.
#pragma once

#include "graphrunner.h"

#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

struct AVFormatContext;
struct AVStream;
struct AVPacket;
struct AVRational;
struct AVCodecParameters;

namespace mediatool {

namespace detail {
/// av_strerror as a QString.
QString avErr(int r);
/// Deadline + cancel hook, polled by libav's I/O (AVIOInterruptCB) and by the feed loops between packets.
struct Interrupt {
    QElapsedTimer clock;
    qint64 timeoutMs = 0;
    const std::function<bool()>* cancel = nullptr;
    bool timedOut = false, cancelled = false;
    bool stop();
    static int callback(void* p);
};
/// ffmpeg's automatic mapping (ffmpeg_mux_init.c map_auto_video / map_auto_audio): the largest picture / the most
/// channels, +1e8 for a stream that delivered packets, +5e6 for the default disposition; attached pictures last. -1 = none.
int bestVideoStream(const AVFormatContext* fc);
int bestAudioStream(const AVFormatContext* fc);
/// Opens a local input the way the runner does: scan_all_pmts, the protocol whitelist, the interrupt callback, the
/// concat demuxer with safe=0 for a concat list, the image2 demuxer with loop / framerate for -loop 1 and -framerate.
int openInput(const GraphInput& in, bool localOnly, Interrupt* irq, AVFormatContext** fc);
/// ffmpeg's input seek for -ss (the keyframe before t, 3/23 s earlier for streams with B-frame delay): returns the
/// seek target in AV_TIME_BASE units from the file start (start_time + ss), which packet timestamps are rebased to.
int64_t seekInput(AVFormatContext* fc, double seekSec, const QString& path);
}   // namespace detail

/// True when the command copies every stream it writes and has no graph: the remux handles it, nothing is decoded.
bool isCopyOnly(const GraphCommand& c);

/// The remux of the two copy forms above (also any other -c copy command line the parser accepts).
RunResult remuxCopy(const GraphCommand& c, const RunOptions& o);

/// The output side shared by the remux and the graph runner.
class MuxWriter {
public:
    MuxWriter();
    ~MuxWriter();
    MuxWriter(const MuxWriter&) = delete;
    MuxWriter& operator=(const MuxWriter&) = delete;

    /// Allocates the muxer for the output (mp4 / m4v -> "mp4", mov, wav, jpg -> image2) and opens the file. `err` gets
    /// ffmpeg's message. Nothing is written before writeHeader().
    bool open(const GraphOutput& out, bool overwrite, bool noOverwrite, QString* err, int* averror);
    AVFormatContext* context() const { return m_oc; }
    /// A new output stream; `timeBase` is what packets come in (the encoder's, or the input stream's for a copy).
    AVStream* addStream(const AVRational& timeBase);
    bool writeHeader(QString* err, int* averror);
    /// Rescales from `timeBase` to the stream's and interleaves. Tracks the output time for the progress counters.
    bool writePacket(AVPacket* pkt, int streamIndex, const AVRational& timeBase, QString* err, int* averror);
    /// Writes the trailer (the +faststart rewrite happens here) and closes the file.
    bool finish(QString* err, int* averror);

    qint64 bytesWritten() const;       // avio position, or the file size after finish()
    qint64 outTimeUs() const { return m_outTimeUs; }   // the largest packet end written, in microseconds
    double durationSec() const { return m_outTimeUs < 0 ? -1 : double(m_outTimeUs) / 1000000.0; }
    qint64 packets(int streamIndex) const { return streamIndex < m_packets.size() ? m_packets[streamIndex] : 0; }

private:
    AVFormatContext* m_oc = nullptr;
    bool m_headerWritten = false, m_finished = false;
    qint64 m_outTimeUs = -1;
    qint64 m_finalBytes = 0;
    QVector<qint64> m_packets;
    QVector<int64_t> m_lastDts;        // per stream, ffmpeg's last_mux_dts
};

} // namespace mediatool
