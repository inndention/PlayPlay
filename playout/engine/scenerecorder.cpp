#include "scenerecorder.h"
#include "encoderstage.h"
#include "sinks/ffmpegmuxsink.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QtEndian>
#include <map>
#include <cmath>

namespace {
constexpr qint64 maxBytes = 256LL * 1024 * 1024;
QJsonArray rect(const QRectF& r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRectF rect(const QJsonValue& v) { const auto a = v.toArray(); return a.size() == 4 ? QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : QRectF(); }
bool saveJson(const QString& path, const QJsonObject& o) {
    QSaveFile f(path); if (!f.open(QIODevice::WriteOnly)) return false;
    const auto b = QJsonDocument(o).toJson(); return f.write(b) == b.size() && f.commit();
}
qint64 estimate(const SceneCapture& c) {
    qint64 n = 0; for (const auto& d : c.draws) if (d.video) n += qint64(d.video->width) * d.video->height * 4;
    for (const auto& a : c.tracks) if (a.second) n += qint64(a.second->nb_samples) * a.second->ch_layout.nb_channels * 4;
    if (c.audio) n += qint64(c.audio->nb_samples) * c.audio->ch_layout.nb_channels * 4;
    return n;
}
QJsonObject blob(QFile& f, const QByteArray& raw) {
    const auto b = qCompress(raw, 1); const qint64 at = f.pos();
    if (f.write(b) != b.size()) return {};
    return {{"offset", double(at)}, {"bytes", b.size()}};
}
QByteArray readBlob(QFile& f, const QJsonObject& b, qint64 expected) {
    const qint64 at = qint64(b.value("offset").toDouble(-1)), len = qint64(b.value("bytes").toDouble());
    if (expected < 0 || expected > maxBytes || at < 0 || len < 4 || len > maxBytes || at > f.size() - len || !f.seek(at)) return {};
    const auto packed = f.read(len);
    if (packed.size() != len || qFromBigEndian<quint32>(packed.constData()) != quint64(expected)) return {};
    const auto raw = qUncompress(packed); return raw.size() == expected ? raw : QByteArray();
}
QJsonObject writeAudio(QFile& f, const FramePtr& a) {
    if (!a || a->format != AV_SAMPLE_FMT_FLTP || a->ch_layout.nb_channels < 1) return {};
    QByteArray raw; const int bytes = a->nb_samples * 4;
    for (int c = 0; c < a->ch_layout.nb_channels; ++c) raw.append(reinterpret_cast<const char*>(a->extended_data[c]), bytes);
    auto b = blob(f, raw); if (b.isEmpty()) return b;
    b["samples"] = a->nb_samples; b["channels"] = a->ch_layout.nb_channels; b["rate"] = a->sample_rate; return b;
}
FramePtr readAudio(QFile& f, const QJsonObject& b) {
    const int n = b.value("samples").toInt(), ch = b.value("channels").toInt(), rate = b.value("rate").toInt();
    if (n < 1 || n > 192000 || ch < 1 || ch > 32 || rate < 1) return {};
    const auto raw = readBlob(f, b, qint64(n) * ch * 4); if (raw.isEmpty()) return {};
    auto a = allocAudioFrame(n, ch, rate); if (!a) return {};
    for (int c = 0; c < ch; ++c) memcpy(a->extended_data[c], raw.constData() + c * n * 4, size_t(n) * 4);
    return a;
}
EncoderProfile recordingProfile(const OutputFormat& f) {
    auto p = EncoderProfile::preset("stream_h264_aac_1080p"); p.width = f.width; p.height = f.height;
    p.globalHeader = true; p.threads = 2; p.bFrames = 0; p.x264Preset = "ultrafast"; p.audioChannels = f.channels; p.audioSampleRate = f.sampleRate; return p;
}
}

SceneRecorder::SceneRecorder(const OutputFormat& f) : m_format(f) {}
SceneRecorder::~SceneRecorder() { finish(); }
bool SceneRecorder::begin(const QString& directory, QString* error) {
    if (!QDir::isAbsolutePath(directory) || QFileInfo::exists(directory)) {
        if (error) *error = "Recording needs a new absolute session directory"; return false;
    }
    if (!QDir().mkpath(directory)) { if (error) *error = "Cannot create recording directory"; return false; }
    m_directory = directory; m_active = true; start(QThread::LowPriority); return true;
}
void SceneRecorder::fail(const QString& message) {
    QMutexLocker l(&m_mutex); if (m_error.isEmpty()) m_error = message;
    m_stopping = true; m_ready.wakeAll();
}
void SceneRecorder::submit(SceneCapture c) {
    QMutexLocker l(&m_mutex); if (m_stopping || !m_active) return;
    const auto bytes = estimate(c);
    if (m_queue.size() >= 60 || m_queuedBytes + bytes > maxBytes) {
        m_error = "Recording stopped: storage or encoder could not keep up (capture queue full)";
        m_stopping = true; m_ready.wakeAll(); return;
    }
    m_queuedBytes += bytes; m_queue.push_back(std::move(c)); m_ready.wakeOne();
}
void SceneRecorder::finish() {
    { QMutexLocker l(&m_mutex); m_stopping = true; m_ready.wakeAll(); }
    if (QThread::currentThread() != this) wait();
}
QJsonObject SceneRecorder::status() const {
    QMutexLocker l(&m_mutex);
    return {{"active", m_active && !m_stopping}, {"finalizing", m_active && m_stopping}, {"complete", m_complete},
            {"directory", m_directory}, {"frames", double(m_frames)}, {"queueFrames", int(m_queue.size())}, {"error", m_error}};
}
FramePtr SceneRecorder::compose(const std::vector<SceneDraw>& draws, int w, int h, const QJsonObject& edits) {
    auto v = allocVideoFrame(w, h, AV_PIX_FMT_BGRA); if (!v) return {};
    QImage canvas(v->data[0], w, h, v->linesize[0], QImage::Format_ARGB32_Premultiplied); canvas.fill(Qt::black);
    QPainter p(&canvas); p.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto omit = edits.value("omitLayers").toArray(); const auto layers = edits.value("layers").toObject();
    for (const auto& d : draws) {
        if (!d.video || omit.contains(d.layer)) continue;
        const auto change = layers.value(QString::number(d.layer)).toObject(); if (change.value("hidden").toBool()) continue;
        QImage image(d.video->data[0], d.video->width, d.video->height, d.video->linesize[0], d.premultiplied ? QImage::Format_ARGB32_Premultiplied : QImage::Format_ARGB32);
        p.save(); p.setOpacity(qBound(0.0, change.value("opacity").toDouble(d.opacity), 1.0));
        if (!d.clip.isNull()) p.setClipRect(d.clip);
        p.drawImage(change.contains("rect") ? rect(change.value("rect")) : d.dst, image, d.src.isNull() ? QRectF(image.rect()) : d.src); p.restore();
    }
    p.end(); return v;
}
void SceneRecorder::run() {
    QDir dir(m_directory); QFile data(dir.filePath("media.bin")), timeline(dir.filePath("timeline.jsonl"));
    QJsonObject manifest{{"version", 1}, {"width", m_format.width}, {"height", m_format.height}, {"fpsNum", m_format.rate.num},
                         {"fpsDen", m_format.rate.den}, {"sampleRate", m_format.sampleRate}, {"channels", m_format.channels},
                         {"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"complete", false},
                         {"scope", "Compositor program bus and visible compositor layers. Embedded libobs scene items are one compositor layer."}};
    auto profile = recordingProfile(m_format);
    EncoderStage encoder(profile, m_format.width, m_format.height, m_format.rate);
    FfmpegMuxSink sink("recording", dir.filePath("program.mkv"));
    sink.setDrainOnStop(true);
    if (!saveJson(dir.filePath("session.json"), manifest) || !data.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !timeline.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("Cannot open recording files");
    else if (!encoder.initialize() || !encoder.addSink(&sink)) fail("Cannot start program recording: " + encoder.lastError() + " " + sink.stats().lastError);
    struct Last { FramePtr frame; QJsonObject ref; };
    std::map<QString, Last> cache;
    qint64 first = -1, last = -1, count = 0;
    while (true) {
        SceneCapture c;
        { QMutexLocker l(&m_mutex);
          while (m_queue.empty() && !m_stopping) m_ready.wait(&m_mutex);
          if (m_queue.empty() || !m_error.isEmpty()) break;
          c = std::move(m_queue.front()); m_queue.pop_front(); m_queuedBytes -= estimate(c); }
        if (first < 0) { first = c.frame; manifest["firstFrame"] = double(first); }
        if (last >= 0 && c.frame != last + 1) { fail("Noncontiguous frame timeline; recording incomplete"); break; }
        last = c.frame;
        QJsonArray draws; std::map<int, int> indices; std::map<QString, Last> next;
        bool ok = true;
        for (const auto& d : c.draws) {
            if (!d.video) continue;
            if (d.video->format != AV_PIX_FMT_BGRA) { ok = false; break; }
            const QString key = QString::number(d.layer) + ":" + QString::number(indices[d.layer]++);
            auto& previous = cache[key]; QJsonObject ref;
            if (previous.frame && previous.frame->data[0] == d.video->data[0] && previous.frame->width == d.video->width && previous.frame->height == d.video->height) ref = previous.ref;
            else {
                QByteArray raw; raw.resize(qsizetype(d.video->width) * d.video->height * 4);
                for (int y = 0; y < d.video->height; ++y) memcpy(raw.data() + qsizetype(y) * d.video->width * 4, d.video->data[0] + y * d.video->linesize[0], size_t(d.video->width) * 4);
                ref = blob(data, raw); ref["width"] = d.video->width; ref["height"] = d.video->height;
            }
            if (!ref.contains("bytes")) { ok = false; break; }
            next[key] = {d.video, ref};
            QJsonObject o{{"image", ref}, {"layer", d.layer}, {"rect", rect(d.dst)}, {"sourceRect", rect(d.src)}, {"clip", rect(d.clip)},
                          {"opacity", d.opacity}, {"premultiplied", d.premultiplied}};
            draws.append(o);
        }
        cache = std::move(next);
        QJsonObject tracks;
        for (const auto& a : c.tracks) { const auto b = writeAudio(data, a.second); if (b.isEmpty()) ok = false; else tracks[a.first] = b; }
        const auto audio = writeAudio(data, c.audio); if (audio.isEmpty()) ok = false;
        QJsonObject event{{"frame", double(c.frame)}, {"index", double(count)}, {"draws", draws}, {"audio", audio}, {"tracks", tracks}, {"controls", c.controls}};
        const auto line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
        if (!ok || timeline.write(line) != line.size()) { fail("Disk write or unsupported frame format; recording incomplete"); break; }
        // Flush complete records regularly so an interrupted session can be recovered.
        if (count % 30 == 0 && (!data.flush() || !timeline.flush())) { fail("Recording disk flush failed"); break; }
        MediaFrame mf; mf.video = compose(c.draws, m_format.width, m_format.height); mf.audio = c.audio; mf.frameNumber = count++;
        if (!mf.video) { fail("Cannot allocate program frame"); break; }
        encoder.pushFrame(mf);
        if (encoder.stats().framesDropped || sink.stats().dropped || !sink.stats().lastError.isEmpty()) { fail("Program recording encoder or file output failed"); break; }
        { QMutexLocker l(&m_mutex); m_frames = count; }
    }
    encoder.shutdown(true);
    if (encoder.stats().framesDropped || sink.stats().dropped || !sink.stats().lastError.isEmpty()) fail("Program recording output incomplete");
    if (!data.flush() || !timeline.flush()) fail("Recording final flush failed");
    data.close(); timeline.close();
    { QMutexLocker l(&m_mutex); m_complete = m_error.isEmpty() && count > 0; manifest["complete"] = m_complete; manifest["frames"] = double(count); manifest["error"] = m_error; m_queue.clear(); m_queuedBytes = 0; }
    if (!saveJson(dir.filePath("session.json"), manifest)) fail("Cannot finalize recording manifest");
    { QMutexLocker l(&m_mutex); m_active = false; if (!m_error.isEmpty()) m_complete = false; }
}

bool SceneRecorder::recompose(const QString& session, const QString& output, const QJsonObject& edits, QString* error) {
    auto bad = [&](const QString& s) { if (error) *error = s; return false; };
    if (QFileInfo::exists(output)) return bad("Output exists; choose a new file");
    QDir dir(session); QFile meta(dir.filePath("session.json")), data(dir.filePath("media.bin")), timeline(dir.filePath("timeline.jsonl"));
    if (!meta.open(QIODevice::ReadOnly) || !data.open(QIODevice::ReadOnly) || !timeline.open(QIODevice::ReadOnly)) return bad("Cannot open recording session");
    const auto m = QJsonDocument::fromJson(meta.readAll()).object(); OutputFormat fmt;
    if (!m.value("complete").toBool() && !edits.value("recover").toBool()) return bad("Session is incomplete or still recording; stop capture first (explicit recover:true can export intact records)");
    fmt.width = m.value("width").toInt(); fmt.height = m.value("height").toInt(); fmt.rate = FrameRate(m.value("fpsNum").toInt(), m.value("fpsDen").toInt());
    fmt.channels = m.value("channels").toInt(); fmt.sampleRate = m.value("sampleRate").toInt();
    if (m.value("version").toInt() != 1 || fmt.width < 1 || fmt.height < 1 || qint64(fmt.width) * fmt.height * 4 > maxBytes || !fmt.rate.isValid() || fmt.channels < 1 || fmt.channels > 32 || fmt.sampleRate < 1) return bad("Invalid session format");
    EncoderStage encoder(recordingProfile(fmt), fmt.width, fmt.height, fmt.rate); FfmpegMuxSink sink("recompose", output);
    sink.setDrainOnStop(true);
    if (!encoder.initialize() || !encoder.addSink(&sink)) { encoder.shutdown(); return bad("Cannot open export encoder or output"); }
    qint64 count = 0;
    while (!timeline.atEnd()) {
        const auto line = timeline.readLine(4 * 1024 * 1024); QJsonParseError pe; const auto ev = QJsonDocument::fromJson(line, &pe).object();
        if (pe.error != QJsonParseError::NoError || !ev.contains("draws") || qint64(ev.value("index").toDouble(-1)) != count) { encoder.shutdown(); return bad("Corrupt or interrupted timeline"); }
        std::vector<SceneDraw> draws;
        for (const auto& dv : ev.value("draws").toArray()) {
            const auto o = dv.toObject(), b = o.value("image").toObject(); const int w = b.value("width").toInt(), h = b.value("height").toInt();
            const auto raw = readBlob(data, b, qint64(w) * h * 4);
            if (w < 1 || h < 1 || raw.isEmpty()) { encoder.shutdown(); return bad("Missing or corrupt source picture"); }
            auto v = allocVideoFrame(w, h, AV_PIX_FMT_BGRA); if (!v) { encoder.shutdown(); return bad("Source allocation failed"); }
            for (int y = 0; y < h; ++y) memcpy(v->data[0] + y * v->linesize[0], raw.constData() + qsizetype(y) * w * 4, size_t(w) * 4);
            draws.push_back({v, rect(o.value("rect")), rect(o.value("sourceRect")), rect(o.value("clip")), o.value("opacity").toDouble(1), o.value("layer").toInt(), o.value("premultiplied").toBool()});
        }
        MediaFrame mf; mf.video = compose(draws, fmt.width, fmt.height, edits); mf.audio = readAudio(data, ev.value("audio").toObject()); mf.frameNumber = count++;
        if (!mf.video || !mf.audio) { encoder.shutdown(); return bad("Missing program audio or video"); }
        if (edits.contains("audioGainDb") || edits.contains("audioMute") || edits.contains("omitLayers")) {
            const auto tracks = ev.value("tracks").toObject(), ctl = ev.value("controls").toObject();
            const auto gains = ctl.value("strips").toObject(), adjustments = edits.value("audioGainDb").toObject();
            const auto muted = edits.value("audioMute").toArray(), omitted = edits.value("omitLayers").toArray();
            const double master = ctl.value("masterGain").toDouble(1.0);
            auto mix = allocAudioFrame(mf.audio->nb_samples, fmt.channels, fmt.sampleRate); if (!mix) { encoder.shutdown(); return bad("Audio allocation failed"); }
            silenceAudioFrame(mix);
            for (auto it = tracks.begin(); it != tracks.end(); ++it) {
                const int layer = it.key() == "program" ? 0 : it.key().mid(1).toInt();
                if (muted.contains(it.key()) || omitted.contains(layer)) continue;
                auto a = readAudio(data, it.value().toObject());
                if (!a || a->nb_samples != mix->nb_samples || a->ch_layout.nb_channels != fmt.channels) { encoder.shutdown(); return bad("Invalid isolated audio track"); }
                const double gain = gains.value(it.key()).toDouble() * std::pow(10.0, qBound(-96.0, adjustments.value(it.key()).toDouble(), 24.0) / 20.0);
                for (int c = 0; c < fmt.channels; ++c) for (int i = 0; i < mix->nb_samples; ++i) reinterpret_cast<float*>(mix->extended_data[c])[i] += float(reinterpret_cast<float*>(a->extended_data[c])[i] * gain);
            }
            for (int c = 0; c < fmt.channels; ++c) for (int i = 0; i < mix->nb_samples; ++i) { auto& v = reinterpret_cast<float*>(mix->extended_data[c])[i]; v = float(qBound(-1.0, double(v) * master, 1.0)); }
            mf.audio = mix;
        }
        // Offline export may wait. Keep the encoder queue below its drop threshold.
        while (encoder.stats().queueDepth >= 4 && encoder.isActive()) QThread::msleep(2);
        encoder.pushFrame(mf);
    }
    encoder.shutdown(true);
    if (!count || encoder.stats().framesDropped || sink.stats().dropped || !sink.stats().lastError.isEmpty()) return bad("Export incomplete: encoder or output error");
    return true;
}
