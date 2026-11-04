// libav plumbing for the Studio media utilities (ST-17). See avopen.h for the rules every open follows.
#include "media/avopen.h"

#include <QDir>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/exif.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace studio::media::av {
namespace {

int interruptCb(void* opaque) { return static_cast<Deadline*>(opaque)->expired() ? 1 : 0; }

// Our AVIOContext is the only input: a nested open (HLS segment, mov data reference, a playlist entry) is refused.
int refuseNestedOpen(AVFormatContext*, AVIOContext** pb, const char*, int, AVDictionary**) {
    *pb = nullptr;
    return AVERROR(EPERM);
}

QString failText(const QString& what, int r, Deadline* dl) {
    if (dl && dl->expired()) return what + QStringLiteral(": ") + dl->reason();
    return what + QStringLiteral(": ") + errText(r);
}

int memRead(void* opaque, uint8_t* buf, int size) {
    auto* m = static_cast<Input::Mem*>(opaque);
    const qint64 left = m->bytes.size() - m->pos;
    if (left <= 0) return AVERROR_EOF;
    const int n = int(qMin<qint64>(size, left));
    std::memcpy(buf, m->bytes.constData() + m->pos, size_t(n));
    m->pos += n;
    return n;
}

int64_t memSeek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<Input::Mem*>(opaque);
    const qint64 size = m->bytes.size();
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return size;
    const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? m->pos : whence == SEEK_END ? size : -1;
    if (base < 0) return AVERROR(EINVAL);
    const qint64 p = base + offset;
    if (p < 0 || p > size) return AVERROR(EINVAL);
    m->pos = p;
    return p;
}

// Demuxers that open other files or run other code (on top of every AVFMT_NOFILE one).
constexpr const char* kRefusedFormats[] = {
    "concat", "hls", "applehttp", "dash", "imf", "sdp", "rtsp", "rtp", "avisynth", "vapoursynth",
    "webm_dash_manifest", "image2", "lavfi", "dvdvideo", "bluray", "tee",
};

} // namespace

QString errText(int averror) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(averror, b, sizeof(b));
    return QString::fromUtf8(b);
}

Deadline::Deadline(int timeoutMs, const std::atomic<bool>* cancel) : m_timeoutMs(timeoutMs), m_cancel(cancel) {
    if (m_timeoutMs > 0) m_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_timeoutMs);
}

bool Deadline::expired() {
    if (m_timedOut || m_cancelled) return true;
    if (m_cancel && m_cancel->load(std::memory_order_relaxed)) { m_cancelled = true; return true; }
    if (m_timeoutMs > 0 && std::chrono::steady_clock::now() >= m_end) { m_timedOut = true; return true; }
    return false;
}

QString Deadline::reason() const {
    if (m_cancelled) return QStringLiteral("cancelled");
    if (m_timedOut) return QStringLiteral("timed out after %1 ms").arg(m_timeoutMs);
    return QString();
}

bool refusedFormat(const char* name, bool noFile) {
    if (noFile || !name || !*name) return true;
    const QList<QByteArray> tokens = QByteArray(name).split(',');
    for (const QByteArray& t : tokens) {
        const QByteArray tok = t.trimmed();
        for (const char* r : kRefusedFormats) if (tok == r) return true;
    }
    return false;
}

Input::~Input() {
    if (m_fc) avformat_close_input(&m_fc);   // custom IO: the demuxer never closes our AVIOContext
    if (m_pb) {
        if (m_memory) { av_freep(&m_pb->buffer); avio_context_free(&m_pb); }
        else avio_closep(&m_pb);
    }
}

QString Input::formatName() const {
    return m_fc && m_fc->iformat ? QString::fromLatin1(m_fc->iformat->name) : QString();
}

bool Input::finish(const QByteArray& url, Deadline* dl, QString* error, bool findStreamInfo) {
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    const AVInputFormat* fmt = nullptr;
    // The name is a hint for the probe, as in ffmpeg (an MP3 whose ID3 tag is larger than the probe limit is found by
    // its extension). But the probe always asks image2 too, which reads only the NAME: 50 for any .jpg / .jpeg, where
    // jpeg_pipe scores 26 until the image's end marker is inside the probe buffer (every JPEG over 2 KB); 52 for a
    // name with '{'; the top score for a "%d" name ("b%d.png"). image2 is refused below, so when the name lands on a
    // refused demuxer the bytes alone decide in a second probe (it starts from the first one's buffered data). A
    // refused format the BYTES pick (concat ...) is still refused, and when the bytes alone match nothing (hls wants
    // the .m3u8 name) the first refusal stands. ST-18a found it: the spot composer probes the stills it is handed,
    // and a JPEG still became a colour card.
    const auto refused = [](const AVInputFormat* f) { return refusedFormat(f->name, (f->flags & AVFMT_NOFILE) != 0); };
    int r = av_probe_input_buffer2(m_pb, &fmt, url.isEmpty() ? nullptr : url.constData(), nullptr, 0, 0);
    if (r >= 0 && fmt && !url.isEmpty() && refused(fmt)) {
        const AVInputFormat* named = fmt;
        fmt = nullptr;
        r = av_probe_input_buffer2(m_pb, &fmt, nullptr, nullptr, 0, 0);
        if (r == AVERROR_INVALIDDATA || (r >= 0 && !fmt)) { fmt = named; r = 0; }
    }
    if (r < 0 || !fmt) return fail(failText(QStringLiteral("not a media file"), r < 0 ? r : AVERROR_INVALIDDATA, dl));
    if (refused(fmt))
        return fail(QStringLiteral("refused format %1 (it can open other files or run code)").arg(QString::fromLatin1(fmt->name)));
    AVFormatContext* fc = avformat_alloc_context();
    if (!fc) return fail(QStringLiteral("out of memory"));
    fc->pb = m_pb;
    fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (dl) { fc->interrupt_callback.callback = interruptCb; fc->interrupt_callback.opaque = dl; }
    fc->io_open = refuseNestedOpen;
    AVDictionary* fo = nullptr;
    av_dict_set(&fo, "protocol_whitelist", "file", 0);
    r = avformat_open_input(&fc, url.isEmpty() ? "" : url.constData(), fmt, &fo);   // frees fc on failure, never m_pb
    av_dict_free(&fo);
    if (r < 0) return fail(failText(QStringLiteral("cannot read %1").arg(QString::fromLatin1(fmt->name)), r, dl));
    m_fc = fc;   // the destructor closes it from here on
    if (findStreamInfo && (r = avformat_find_stream_info(m_fc, nullptr)) < 0) return fail(failText(QStringLiteral("no stream info"), r, dl));
    if (dl && dl->expired()) return fail(dl->reason());
    return true;
}

std::unique_ptr<Input> openFile(const QString& path, Deadline* dl, QString* error, bool findStreamInfo) {
    auto fail = [&](const QString& m) { if (error) *error = m; return std::unique_ptr<Input>(); };
    if (path.isEmpty()) return fail(QStringLiteral("no path"));
    if (path.contains(QLatin1String("://"))) return fail(QStringLiteral("only local files can be opened"));
    const QByteArray url = QDir::toNativeSeparators(path).toUtf8();
    std::unique_ptr<Input> in(new Input);
    AVIOInterruptCB cb{interruptCb, dl};
    AVDictionary* o = nullptr;
    av_dict_set(&o, "protocol_whitelist", "file", 0);
    const int r = avio_open2(&in->m_pb, url.constData(), AVIO_FLAG_READ, dl ? &cb : nullptr, &o);
    av_dict_free(&o);
    if (r < 0) return fail(failText(QStringLiteral("cannot open %1").arg(path), r, dl));
    in->m_size = avio_size(in->m_pb);
    if (!in->finish(url, dl, error, findStreamInfo)) return nullptr;
    return in;
}

std::unique_ptr<Input> openMemory(const QByteArray& bytes, Deadline* dl, QString* error, bool findStreamInfo) {
    auto fail = [&](const QString& m) { if (error) *error = m; return std::unique_ptr<Input>(); };
    if (bytes.isEmpty()) return fail(QStringLiteral("no data"));
    std::unique_ptr<Input> in(new Input);
    in->m_memory = true;
    in->m_mem = std::make_unique<Input::Mem>();
    in->m_mem->bytes = bytes;
    in->m_size = bytes.size();
    constexpr int kBuf = 32 * 1024;
    auto* buf = static_cast<uint8_t*>(av_malloc(kBuf));
    if (!buf) return fail(QStringLiteral("out of memory"));
    in->m_pb = avio_alloc_context(buf, kBuf, 0, in->m_mem.get(), memRead, nullptr, memSeek);
    if (!in->m_pb) { av_free(buf); return fail(QStringLiteral("out of memory")); }
    if (!in->finish(QByteArray(), dl, error, findStreamInfo)) return nullptr;
    return in;
}

void CodecFree::operator()(AVCodecContext* c) const { if (c) avcodec_free_context(&c); }
void FrameFree::operator()(AVFrame* f) const { if (f) av_frame_free(&f); }
void PacketFree::operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
void SwsFree::operator()(SwsContext* s) const { if (s) sws_freeContext(s); }
void SwrFree::operator()(SwrContext* s) const { if (s) swr_free(&s); }

FramePtr newFrame() { return FramePtr(av_frame_alloc()); }
PacketPtr newPacket() { return PacketPtr(av_packet_alloc()); }

CodecPtr openDecoder(AVStream* st, int threads, QString* error, qint64 maxPixels) {
    auto fail = [&](const QString& m) { if (error) *error = m; return CodecPtr(); };
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return fail(QStringLiteral("no decoder for %1").arg(QString::fromLatin1(avcodec_get_name(st->codecpar->codec_id))));
    CodecPtr c(avcodec_alloc_context3(codec));
    if (!c) return fail(QStringLiteral("out of memory"));
    int r = avcodec_parameters_to_context(c.get(), st->codecpar);
    if (r < 0) return fail(QStringLiteral("decoder setup: ") + errText(r));
    c->pkt_timebase = st->time_base;
    c->thread_count = threads;
    if (maxPixels > 0) c->max_pixels = maxPixels;   // ff_set_dimensions and ff_get_buffer refuse a larger picture
    if ((r = avcodec_open2(c.get(), codec, nullptr)) < 0)
        return fail(QStringLiteral("cannot open the %1 decoder: %2").arg(QString::fromLatin1(codec->name), errText(r)));
    return c;
}

int displayOrientation(const unsigned char* matrix, size_t size) {
    if (!matrix || size < 9 * sizeof(int32_t)) return 1;
    int32_t m[9];
    std::memcpy(m, matrix, sizeof(m));   // side data has no alignment promise
    const int o = av_exif_matrix_to_orientation(m);
    return o >= 1 && o <= 8 ? o : 1;
}

} // namespace studio::media::av
