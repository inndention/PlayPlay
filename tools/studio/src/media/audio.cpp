// Audio for transcription (ST-17). See audio.h.
#include "media/audio.h"

#include "media/avopen.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSaveFile>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace studio::media {
namespace {

quint32 u32le(const uchar* p) { return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
quint16 u16le(const uchar* p) { return quint16(p[0] | p[1] << 8); }
void put32(char* p, quint32 v) { for (int i = 0; i < 4; ++i) p[i] = char((v >> (8 * i)) & 0xFF); }
void put16(char* p, quint16 v) { p[0] = char(v & 0xFF); p[1] = char(v >> 8); }

constexpr double kMaxLeadSilence = 30.0;

// Sink: receives 16 kHz mono s16 samples; returns false to stop decoding (enough audio, or a write failed).
using Sink = std::function<bool(const qint16* samples, qint64 n)>;

struct DecodeInfo { QString codec; int rate = 0, channels = 0; double leadSilence = 0; qint64 samples = 0; };

// The one decode + resample path behind toWav16k and splitAudio16k (so their PCM is identical).
QString decodeTo16k(const QString& path, int timeoutMs, const std::atomic<bool>* cancel, double maxSeconds, bool alignToStart,
                    const Sink& sink, DecodeInfo* info) {
    av::Deadline dl(timeoutMs, cancel);
    QString err;
    std::unique_ptr<av::Input> in = av::openFile(path, &dl, &err);
    if (!in) return err;
    AVFormatContext* fc = in->fc();
    const int as = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (as < 0) return QStringLiteral("no audio stream in %1").arg(QDir::toNativeSeparators(path));
    AVStream* st = fc->streams[as];
    for (unsigned i = 0; i < fc->nb_streams; ++i) if (int(i) != as) fc->streams[i]->discard = AVDISCARD_ALL;
    av::CodecPtr cc = av::openDecoder(st, 1, &err);
    if (!cc) return err;
    info->codec = QString::fromLatin1(avcodec_get_name(st->codecpar->codec_id));
    info->rate = st->codecpar->sample_rate;
    info->channels = st->codecpar->ch_layout.nb_channels;

    const qint64 maxSamples = maxSeconds > 0 && std::isfinite(maxSeconds) ? qint64(std::llround(maxSeconds * kSttSampleRate))
                                                                          : std::numeric_limits<qint64>::max();
    bool stopped = false;
    auto push = [&](const qint16* s, qint64 n) {
        if (stopped || n <= 0) return;
        const qint64 take = std::min(n, maxSamples - info->samples);
        if (take > 0) {
            if (!sink(s, take)) { stopped = true; return; }
            info->samples += take;
        }
        if (info->samples >= maxSamples) stopped = true;
    };

    av::PacketPtr pkt = av::newPacket();
    av::FramePtr frame = av::newFrame();
    if (!pkt || !frame) return QStringLiteral("out of memory");
    av::SwrPtr swr;
    AVChannelLayout swrLayout{};
    int swrRate = 0, swrFmt = -1;
    std::vector<qint16> out;
    bool first = true;

    auto flushSwr = [&]() {
        if (!swr) return;
        for (int guard = 0; guard < 64 && !stopped; ++guard) {
            out.resize(4096);
            uint8_t* o[1] = {reinterpret_cast<uint8_t*>(out.data())};
            const int got = swr_convert(swr.get(), o, int(out.size()), nullptr, 0);
            if (got <= 0) break;
            push(out.data(), got);
        }
    };
    auto onFrame = [&](const AVFrame* f) -> QString {
        if (f->nb_samples <= 0) return {};
        if (first) {
            first = false;
            // sound that starts after the container start (late audio in a video): pad so transcript times match the picture
            const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            if (alignToStart && pts != AV_NOPTS_VALUE) {
                const double fileStart = fc->start_time != AV_NOPTS_VALUE ? double(fc->start_time) / AV_TIME_BASE : 0.0;
                const double lead = double(pts) * av_q2d(st->time_base) - fileStart;
                if (lead > 0.001) {
                    info->leadSilence = std::min(kMaxLeadSilence, lead);
                    const std::vector<qint16> zeros(size_t(std::llround(info->leadSilence * kSttSampleRate)), 0);
                    push(zeros.data(), qint64(zeros.size()));
                }
            }
        }
        AVChannelLayout layout{};
        if (f->ch_layout.nb_channels > 0 && f->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_copy(&layout, &f->ch_layout);
        else av_channel_layout_default(&layout, f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : qMax(1, info->channels));
        if (!swr || swrRate != f->sample_rate || swrFmt != f->format || av_channel_layout_compare(&swrLayout, &layout) != 0) {
            flushSwr();   // a format change mid-stream (rare): finish the old resampler first
            SwrContext* s = nullptr;
            const AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
            int r = swr_alloc_set_opts2(&s, &mono, AV_SAMPLE_FMT_S16, kSttSampleRate, &layout, AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
            if (r >= 0) r = swr_init(s);
            if (r < 0) { swr_free(&s); av_channel_layout_uninit(&layout); return QStringLiteral("resampler: ") + av::errText(r); }
            swr.reset(s);
            av_channel_layout_uninit(&swrLayout);
            av_channel_layout_copy(&swrLayout, &layout);
            swrRate = f->sample_rate; swrFmt = f->format;
        }
        av_channel_layout_uninit(&layout);
        const int cap = swr_get_out_samples(swr.get(), f->nb_samples);
        if (cap <= 0) return {};
        out.resize(size_t(cap));
        uint8_t* o[1] = {reinterpret_cast<uint8_t*>(out.data())};
        const int got = swr_convert(swr.get(), o, cap, f->extended_data, f->nb_samples);
        if (got < 0) return QStringLiteral("resample: ") + av::errText(got);
        push(out.data(), got);
        return {};
    };

    QString result;
    bool drained = false;
    for (qint64 guard = 0; !stopped; ++guard) {
        if (dl.expired()) { result = dl.reason(); break; }
        int r = avcodec_receive_frame(cc.get(), frame.get());
        if (r == 0) {
            const QString e = onFrame(frame.get());
            av_frame_unref(frame.get());
            if (!e.isEmpty()) { result = e; break; }
            continue;
        }
        if (r == AVERROR_EOF) break;
        if (r == AVERROR_INVALIDDATA) continue;
        if (r != AVERROR(EAGAIN)) { result = QStringLiteral("decode: ") + av::errText(r); break; }
        if (drained) break;
        r = av_read_frame(fc, pkt.get());
        if (r < 0) {
            if (dl.expired()) { result = dl.reason(); break; }
            avcodec_send_packet(cc.get(), nullptr);   // end of file (or a damaged tail): drain
            drained = true;
            continue;
        }
        if (pkt->stream_index == as) {
            const int s = avcodec_send_packet(cc.get(), pkt.get());
            if (s < 0 && s != AVERROR(EAGAIN) && s != AVERROR_INVALIDDATA) { av_packet_unref(pkt.get()); result = QStringLiteral("decode: ") + av::errText(s); break; }
        }
        av_packet_unref(pkt.get());
    }
    if (result.isEmpty() && !stopped) flushSwr();
    av_channel_layout_uninit(&swrLayout);
    if (result.isEmpty() && info->samples == 0 && !stopped) result = QStringLiteral("no audio could be decoded");
    return result;
}

QString safeBaseName(const QString& s) {
    QString out;
    for (const QChar c : s) out += (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.') ? c : QLatin1Char('_');
    while (out.startsWith(QLatin1Char('.'))) out.remove(0, 1);
    out.truncate(64);
    return out.isEmpty() ? QStringLiteral("audio") : out;
}

} // namespace

// ---------------------------------------------------------------- WAV bytes

std::optional<WavInfo> wavInfo(QByteArrayView b) {
    if (b.size() < 44) return std::nullopt;
    const uchar* p = reinterpret_cast<const uchar*>(b.data());
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return std::nullopt;
    qint64 o = 12;
    std::optional<WavInfo> fmt;
    while (o + 8 <= b.size()) {
        const quint32 len = u32le(p + o + 4);
        if (std::memcmp(p + o, "fmt ", 4) == 0 && o + 24 <= b.size()) {
            WavInfo w;
            w.channels = u16le(p + o + 10);
            w.sampleRate = int(u32le(p + o + 12));
            w.bitsPerSample = u16le(p + o + 22);
            fmt = w;
        }
        if (std::memcmp(p + o, "data", 4) == 0 && fmt) {
            fmt->dataOffset = o + 8;
            fmt->dataBytes = std::min<qint64>(len, b.size() - o - 8);
            return fmt;
        }
        o += 8 + qint64(len) + (len & 1);
    }
    return std::nullopt;
}

QByteArray wavHeader(qint64 dataBytes, int sampleRate, int channels, int bitsPerSample) {
    QByteArray h(44, '\0');
    char* p = h.data();
    const int blockAlign = (channels * bitsPerSample) / 8;
    std::memcpy(p, "RIFF", 4); put32(p + 4, quint32(36 + dataBytes)); std::memcpy(p + 8, "WAVE", 4); std::memcpy(p + 12, "fmt ", 4);
    put32(p + 16, 16); put16(p + 20, 1); put16(p + 22, quint16(channels)); put32(p + 24, quint32(sampleRate));
    put32(p + 28, quint32(sampleRate * blockAlign)); put16(p + 32, quint16(blockAlign)); put16(p + 34, quint16(bitsPerSample));
    std::memcpy(p + 36, "data", 4); put32(p + 40, quint32(dataBytes));
    return h;
}

std::vector<WavPiece> splitWav(const QByteArray& b, double seconds) {
    const std::optional<WavInfo> w = wavInfo(b);
    const qint64 blockAlign = w ? (qint64(w->channels) * w->bitsPerSample + 7) / 8 : 0;
    const qint64 bps = w ? qint64(w->sampleRate) * blockAlign : 0;
    if (!w || blockAlign <= 0 || bps <= 0 || !std::isfinite(seconds) || seconds <= 0) return {WavPiece{b, 0}};
    const qint64 piece = std::max<qint64>(blockAlign, qint64(std::floor(seconds * double(bps) / double(blockAlign))) * blockAlign);
    if (w->dataBytes <= piece) return {WavPiece{b, 0}};
    std::vector<WavPiece> out;
    for (qint64 at = 0; at < w->dataBytes; at += piece) {
        const qint64 n = std::min(piece, w->dataBytes - at);
        QByteArray part = wavHeader(n, w->sampleRate, w->channels, w->bitsPerSample);
        part.append(b.constData() + w->dataOffset + at, n);
        out.push_back({part, double(at) / double(bps)});
    }
    return out;
}

SttLimits sttFor(double maxAudioMinutes, double maxAudioMB) {
    const double inf = std::numeric_limits<double>::infinity();
    const double mb = std::isfinite(maxAudioMB) && maxAudioMB > 0 ? maxAudioMB : 0;
    const double minutes = std::isfinite(maxAudioMinutes) && maxAudioMinutes > 0 ? maxAudioMinutes : 0;
    // 16 kHz mono WAV = 32,000 bytes a second
    const double byMb = mb ? std::floor(mb * 1024 * 1024 * 0.9 / double(kSttBytesPerSecond)) : inf;
    const double byMin = minutes ? std::floor(minutes * 60 * 0.95) : inf;
    SttLimits l;
    l.pieceSeconds = std::max(30.0, std::min({kSttPieceSeconds, byMb, byMin}));
    l.maxSingleBytes = mb ? std::min<qint64>(kSttMaxSingle, qint64(std::floor(mb * 1024 * 1024 * 0.95))) : kSttMaxSingle;
    return l;
}

// ---------------------------------------------------------------- decode + resample

QJsonObject Wav16k::toJson() const {
    if (!ok) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("seconds"), seconds},
                       {QStringLiteral("bytes"), double(wav.size())}, {QStringLiteral("leadSilence"), leadSilence}};
}

QJsonObject SplitResult::toJson() const {
    if (!ok) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    QJsonArray a;
    for (const AudioPiece& p : pieces)
        a.append(QJsonObject{{QStringLiteral("index"), p.index}, {QStringLiteral("path"), p.path}, {QStringLiteral("offset"), p.offset},
                             {QStringLiteral("seconds"), p.seconds}, {QStringLiteral("bytes"), double(p.bytes)}});
    return QJsonObject{
        {QStringLiteral("ok"), true}, {QStringLiteral("pieces"), a}, {QStringLiteral("seconds"), seconds},
        {QStringLiteral("pieceSeconds"), pieceSeconds}, {QStringLiteral("leadSilence"), leadSilence},
        {QStringLiteral("source"), QJsonObject{{QStringLiteral("codec"), sourceCodec}, {QStringLiteral("sampleRate"), sourceRate},
                                               {QStringLiteral("channels"), sourceChannels}}},
    };
}

Wav16k toWav16k(const QString& path, const Wav16kOptions& o) {
    Wav16k r;
    QByteArray pcm;
    bool tooBig = false;
    const qint64 maxPcm = std::max<qint64>(0, o.maxBytes - 44);
    DecodeInfo info;
    const QString err = decodeTo16k(path, o.timeoutMs, o.cancel, o.maxSeconds, o.alignToStart,
        [&](const qint16* s, qint64 n) {
            if (pcm.size() + n * 2 > maxPcm) { tooBig = true; return false; }
            pcm.append(reinterpret_cast<const char*>(s), n * 2);
            return true;
        }, &info);
    if (tooBig) { r.error = QStringLiteral("the audio is longer than fits in memory here (%1 MB); split it into pieces instead").arg(o.maxBytes / (1024 * 1024)); return r; }
    if (!err.isEmpty()) { r.error = err; return r; }
    r.wav = wavHeader(pcm.size(), kSttSampleRate, 1, 16);
    r.wav.append(pcm);
    r.seconds = double(pcm.size() / 2) / kSttSampleRate;
    r.leadSilence = info.leadSilence;
    r.ok = true;
    return r;
}

QFuture<Wav16k> toWav16kAsync(const QString& path, const Wav16kOptions& o, QThreadPool* pool) {
    return QtConcurrent::run(pool ? pool : QThreadPool::globalInstance(), [path, o] { return toWav16k(path, o); });
}

SplitResult splitAudio16k(const QString& path, const QString& outDir, const SplitOptions& o) {
    SplitResult r;
    r.pieceSeconds = std::isfinite(o.pieceSeconds) ? std::clamp(o.pieceSeconds, 1.0, kSttPieceSeconds) : kSttPieceSeconds;
    const qint64 pieceSamples = std::max<qint64>(1, std::llround(r.pieceSeconds * kSttSampleRate));
    if (outDir.isEmpty() || !QDir().mkpath(outDir)) { r.error = QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(outDir)); return r; }
    const QString base = safeBaseName(o.baseName);
    QByteArray buf;
    buf.reserve(qsizetype(pieceSamples * 2));
    QString writeError;
    auto flushPiece = [&]() -> bool {
        if (buf.isEmpty()) return true;
        AudioPiece p;
        p.index = int(r.pieces.size());
        p.path = QDir(outDir).filePath(QStringLiteral("%1-%2.wav").arg(base).arg(p.index, 3, 10, QLatin1Char('0')));
        p.offset = double(qint64(p.index) * pieceSamples) / kSttSampleRate;
        p.seconds = double(buf.size() / 2) / kSttSampleRate;
        QSaveFile f(p.path);
        if (!f.open(QIODevice::WriteOnly) || f.write(wavHeader(buf.size(), kSttSampleRate, 1, 16)) != 44 || f.write(buf) != buf.size() || !f.commit()) {
            writeError = QStringLiteral("cannot write %1: %2").arg(QDir::toNativeSeparators(p.path), f.errorString());
            return false;
        }
        p.bytes = 44 + buf.size();
        r.pieces.push_back(p);
        buf.clear();
        if (o.onPiece) o.onPiece(p.index, p.offset, p.seconds);
        return true;
    };
    DecodeInfo info;
    const QString err = decodeTo16k(path, o.timeoutMs, o.cancel, o.maxSeconds, o.alignToStart,
        [&](const qint16* s, qint64 n) {
            while (n > 0) {
                const qint64 room = pieceSamples - buf.size() / 2;
                const qint64 take = std::min(room, n);
                buf.append(reinterpret_cast<const char*>(s), take * 2);
                s += take; n -= take;
                if (buf.size() / 2 >= pieceSamples && !flushPiece()) return false;
            }
            return true;
        }, &info);
    QString failure = !writeError.isEmpty() ? writeError : err;
    if (failure.isEmpty() && !flushPiece()) failure = writeError;
    if (!failure.isEmpty()) {
        for (const AudioPiece& p : r.pieces) QFile::remove(p.path);   // all or nothing
        r.pieces.clear();
        r.error = failure;
        return r;
    }
    r.seconds = double(info.samples) / kSttSampleRate;
    r.leadSilence = info.leadSilence;
    r.sourceCodec = info.codec;
    r.sourceRate = info.rate;
    r.sourceChannels = info.channels;
    r.ok = true;
    return r;
}

QFuture<SplitResult> splitAudio16kAsync(const QString& path, const QString& outDir, const SplitOptions& o, QThreadPool* pool) {
    return QtConcurrent::run(pool ? pool : QThreadPool::globalInstance(), [path, outDir, o] { return splitAudio16k(path, outDir, o); });
}

} // namespace studio::media
