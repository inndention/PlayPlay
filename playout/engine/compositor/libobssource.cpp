#include "libobssource.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// libobs public headers (C). Only this translation unit sees them: obs.h pulls in util/base.h (blog, LOG_*),
// graphics/vec*.h and media-io; none of those names collide with Qt, but keeping them out of the header keeps the
// rest of the engine libobs-free.
#include <obs.h>
#include <util/base.h>
#include <util/platform.h>

// ===========================================================================
// Process-wide libobs runtime (obs_startup / obs_reset_video / modules), started once, shut down at app exit.
// ===========================================================================
namespace {

struct Runtime {
    QMutex mutex;                     // guards start / shutdown
    bool started = false;
    bool downForGood = false;         // obs_shutdown ran: nothing restarts it in this process
    QString root;
    OutputFormat fmt;
    QStringList modules, failedModules;
    QString version;
    QString graphics;
    std::atomic<LibObsSource*> active{nullptr};   // the one open source
};
Runtime& rt() { static Runtime r; return r; }

void obsLogHandler(int lvl, const char* msg, va_list args, void*) {
    if (lvl > LOG_INFO) return;                       // LOG_DEBUG is 400: too chatty for the engine log
    char buf[2048];
    vsnprintf(buf, sizeof buf, msg, args);
    if (lvl <= LOG_ERROR) qCritical().noquote() << "[libobs]" << buf;
    else if (lvl <= LOG_WARNING) qWarning().noquote() << "[libobs]" << buf;
    else qInfo().noquote() << "[libobs]" << buf;
}

struct FoundModule { QString bin, data, name; };
void collectModule(void* param, const struct obs_module_info2* info) {
    auto* v = static_cast<std::vector<FoundModule>*>(param);
    v->push_back({QString::fromUtf8(info->bin_path), QString::fromUtf8(info->data_path ? info->data_path : ""), QString::fromUtf8(info->name)});
}

/// Start libobs headless for `fmt`. Returns false with `err` set. Holds rt().mutex.
bool startRuntime(const QString& root, const OutputFormat& fmt, QString* err) {
    Runtime& r = rt();
    QMutexLocker l(&r.mutex);
    if (r.started) {
        if (r.fmt.width != fmt.width || r.fmt.height != fmt.height || r.fmt.rate.num != fmt.rate.num || r.fmt.rate.den != fmt.rate.den)
            qWarning() << "[libobs] runtime already up at" << r.fmt.width << "x" << r.fmt.height << r.fmt.rate.toString()
                       << "- a source asked for" << fmt.width << "x" << fmt.height << fmt.rate.toString() << "(raw callback scales)";
        return true;
    }
    if (r.downForGood) { if (err) *err = "libobs was shut down in this process"; return false; }
    const QString bin = root + "/bin/64bit";
    if (!QFileInfo::exists(bin + "/obs.dll")) { if (err) *err = "libobs install not found: " + bin + "/obs.dll"; return false; }
    if (!QFileInfo::exists(root + "/data/libobs/default.effect")) { if (err) *err = "libobs data missing: " + root + "/data/libobs/default.effect"; return false; }

    base_set_log_handler(obsLogHandler, nullptr);
    if (!obs_startup("en-US", nullptr, nullptr)) { if (err) *err = "obs_startup failed"; return false; }
    r.version = QString::fromUtf8(obs_get_version_string());
    // Core effects (default.effect, ...). obs_add_data_path is deprecated in 33 but still the only way to tell a
    // host that is not laid out like OBS Studio (bin/64bit/obs64.exe + ../../data) where data/libobs lives.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const QByteArray dataPath = (root + "/data/libobs/").toUtf8();
    obs_add_data_path(dataPath.constData());
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    struct obs_video_info ovi;
    memset(&ovi, 0, sizeof ovi);
    ovi.graphics_module = "libobs-d3d11";
    ovi.fps_num = uint32_t(fmt.rate.num);
    ovi.fps_den = uint32_t(fmt.rate.den);
    ovi.base_width = ovi.output_width = uint32_t(fmt.width);
    ovi.base_height = ovi.output_height = uint32_t(fmt.height);
    ovi.output_format = VIDEO_FORMAT_BGRA;       // the compositor's own format: no colour conversion, straight readback
    ovi.adapter = uint32_t(qEnvironmentVariableIntValue("PLAYPLAY_LIBOBS_ADAPTER"));
    ovi.gpu_conversion = true;
    ovi.colorspace = VIDEO_CS_SRGB;
    ovi.range = VIDEO_RANGE_FULL;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    const int vr = obs_reset_video(&ovi);
    if (vr != OBS_VIDEO_SUCCESS) {
        if (err) *err = QString("obs_reset_video failed (%1)%2").arg(vr).arg(vr == OBS_VIDEO_MODULE_NOT_FOUND ? ": libobs-d3d11.dll not next to the executable" : "");
        obs_shutdown();
        return false;
    }
    r.graphics = "libobs-d3d11";
    struct obs_audio_info oai;
    oai.samples_per_sec = uint32_t(fmt.sampleRate);
    oai.speakers = fmt.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
    if (!obs_reset_audio(&oai)) { if (err) *err = "obs_reset_audio failed"; obs_shutdown(); return false; }

    // Modules: <root>/core/<name>/<name>.dll with data in <root>/core/<name>/data (the OBS 33 layout).
    const QByteArray modBin = (root + "/core/%module%").toUtf8(), modData = (root + "/core/%module%/data").toUtf8();
    obs_add_module_path(modBin.constData(), modData.constData());
    std::vector<FoundModule> found;
    obs_find_modules2(collectModule, &found);
    for (const FoundModule& m : found) {
        obs_module_t* mod = nullptr;
        const QByteArray b = m.bin.toUtf8(), d = m.data.toUtf8();
        const int rc = obs_open_module(&mod, b.constData(), d.isEmpty() ? nullptr : d.constData());
        if (rc != MODULE_SUCCESS) { r.failedModules << QString("%1 (open %2)").arg(m.name).arg(rc); continue; }
        if (!obs_init_module(mod)) { r.failedModules << QString("%1 (init)").arg(m.name); continue; }
        r.modules << m.name;
    }
    obs_post_load_modules();
    r.root = root; r.fmt = fmt; r.started = true;
    qInfo().noquote() << "[libobs]" << r.version << "up:" << fmt.width << "x" << fmt.height << fmt.rate.toString()
                      << "BGRA; modules:" << r.modules.join(", ") << (r.failedModules.isEmpty() ? "" : "; failed: " + r.failedModules.join(", "));
    // obs_shutdown before static destructors tear the graphics thread's feet away.
    if (QCoreApplication::instance()) qAddPostRoutine(LibObsSource::shutdownRuntime);
    return true;
}

/// "#rrggbb" / "#aarrggbb" -> libobs 0xAABBGGRR
bool colorFromHex(const QString& s, uint32_t* out) {
    if (!s.startsWith('#') || (s.size() != 7 && s.size() != 9)) return false;
    bool ok = false;
    const uint32_t v = s.mid(1).toUInt(&ok, 16);
    if (!ok) return false;
    const uint32_t a = s.size() == 9 ? (v >> 24) & 0xFF : 0xFF;
    const uint32_t rr = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    *out = (a << 24) | (b << 16) | (g << 8) | rr;
    return true;
}

obs_data_t* settingsFromJson(QJsonObject s) {
    for (auto it = s.begin(); it != s.end(); ++it) {
        uint32_t c = 0;
        if (it.value().isString() && colorFromHex(it.value().toString(), &c)) it.value() = double(c);
    }
    const QByteArray json = QJsonDocument(s).toJson(QJsonDocument::Compact);
    obs_data_t* d = obs_data_create_from_json(json.constData());
    return d ? d : obs_data_create();
}

double pctMs(std::vector<qint64> v, double p) {
    if (v.empty()) return -1.0;
    const size_t k = size_t(p * double(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + qint64(k), v.end());
    return double(v[k]) / 1e6;
}

} // namespace

// ===========================================================================
// LibObsSource
// ===========================================================================

struct LibObsSource::Impl {
    struct Item { QString name; obs_source_t* source = nullptr; obs_sceneitem_t* item = nullptr; QString scene; };
    struct Scene { QString name; obs_scene_t* scene = nullptr; };
    std::vector<Scene> scenes;
    std::vector<Item> items;
    obs_source_t* transition = nullptr;
    QString transitionId;
    QString program;                           // scene on air (or arriving)
    bool videoCb = false, audioCb = false;

    // producer-side timing (set by the raw video callback; read under LiveSourceBase::m_mutex)
    qint64 lastObsTs = 0, lastCopyNs = 0;
    std::atomic<qint64> callbacks{0}, audioCallbacks{0};
    std::vector<qint64> ages;                   // ring of pull ages (ns)
    size_t ageIdx = 0; bool ageFull = false;
    PullTiming lastPull;
    const void* lastServed = nullptr;
    qint64 filterUpdates = 0;
    QString filterError;

    // switch bookkeeping
    qint64 switchRequestedNs = 0, switchStartedNs = 0; QString switchTarget; int switches = 0;

    // command worker
    std::thread worker;
    std::mutex cmdMutex;
    std::condition_variable cmdCv;
    std::deque<QJsonObject> cmds;
    bool stop = false;

    Scene* scene(const QString& n) { for (auto& s : scenes) if (s.name == n) return &s; return nullptr; }
};

LibObsSource::LibObsSource(const QJsonObject& spec, QObject* parent) : LiveSourceBase(parent), m_impl(new Impl), m_spec(spec) {
    m_wantAudio = spec.value("audio").toBool(true);
    m_fillRaster = true;
    m_impl->ages.assign(4096, 0);
}

LibObsSource::~LibObsSource() { close(); delete m_impl; }

QString LibObsSource::defaultRoot() {
    const QString env = qEnvironmentVariable("PLAYPLAY_LIBOBS_ROOT");
    if (!env.isEmpty()) return QDir::fromNativeSeparators(env);
    const QString bundled = QCoreApplication::applicationDirPath() + "/libobs";
    if (QFileInfo::exists(bundled + "/bin/64bit/obs.dll")) return bundled;
#ifdef PLAYPLAY_LIBOBS_ROOT
    return QString::fromUtf8(PLAYPLAY_LIBOBS_ROOT);
#else
    return QString();
#endif
}

bool LibObsSource::runtimeStarted() { QMutexLocker l(&rt().mutex); return rt().started; }
qint64 LibObsSource::obsNowNs() { return qint64(os_gettime_ns()); }

void LibObsSource::shutdownRuntime() {
    Runtime& r = rt();
    QMutexLocker l(&r.mutex);
    if (!r.started) return;
    if (r.active.load()) { qWarning() << "[libobs] shutdownRuntime with a source still open; closing it"; }
    r.started = false; r.downForGood = true;
    l.unlock();
    if (LibObsSource* a = r.active.load()) a->close();
    obs_shutdown();
    qInfo() << "[libobs] shut down";
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

bool LibObsSource::open(const OutputFormat& fmt) {
    if (m_open) return true;
    LibObsSource* expected = nullptr;
    if (!rt().active.compare_exchange_strong(expected, this)) { setError("libobs: one source per process (libobs has a single program output)"); return false; }
    m_fmt = fmt;
    resetAudio();
    QString root = m_spec.value("root").toString();
    if (root.isEmpty()) root = defaultRoot();
    root = QDir::fromNativeSeparators(root);
    QString err;
    if (!startRuntime(root, fmt, &err)) { setError(err); rt().active = nullptr; return false; }
    if (!buildScenes(fmt, &err)) { setError(err); teardownScenes(); rt().active = nullptr; return false; }
    m_pool = std::make_unique<FramePool>(fmt.width, fmt.height, kMixerPixelFormat);

    struct video_scale_info vconv;
    memset(&vconv, 0, sizeof vconv);
    vconv.format = VIDEO_FORMAT_BGRA; vconv.width = uint32_t(fmt.width); vconv.height = uint32_t(fmt.height);
    vconv.range = VIDEO_RANGE_FULL; vconv.colorspace = VIDEO_CS_SRGB;
    obs_add_raw_video_callback(&vconv, &LibObsSource::rawVideo, this);
    m_impl->videoCb = true;
    if (m_wantAudio) {
        struct audio_convert_info aconv;
        memset(&aconv, 0, sizeof aconv);
        aconv.samples_per_sec = uint32_t(fmt.sampleRate); aconv.format = AUDIO_FORMAT_FLOAT_PLANAR;
        aconv.speakers = fmt.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO; aconv.allow_clipping = false;
        obs_add_raw_audio_callback(0, &aconv, &LibObsSource::rawAudio, this);
        m_impl->audioCb = true;
    }
    m_impl->stop = false;
    m_impl->worker = std::thread([this]() { workerLoop(); });
    m_open = true;
    setConnected(true);
    { QMutexLocker l(&m_mutex); m_statusText = QString("libobs %1 program %2").arg(rt().version, m_impl->program); }
    return true;
}

void LibObsSource::close() {
    if (!m_open) return;
    { std::lock_guard<std::mutex> g(m_impl->cmdMutex); m_impl->stop = true; }
    m_impl->cmdCv.notify_all();
    if (m_impl->worker.joinable()) m_impl->worker.join();
    if (m_impl->videoCb) { obs_remove_raw_video_callback(&LibObsSource::rawVideo, this); m_impl->videoCb = false; }
    if (m_impl->audioCb) { obs_remove_raw_audio_callback(0, &LibObsSource::rawAudio, this); m_impl->audioCb = false; }
    teardownScenes();
    { QMutexLocker l(&m_mutex); m_latest.reset(); m_impl->lastServed = nullptr; }
    m_pool.reset();
    m_open = false;
    setConnected(false);
    rt().active = nullptr;
}

// ---------------------------------------------------------------------------
// Scene graph from the spec
// ---------------------------------------------------------------------------

bool LibObsSource::buildScenes(const OutputFormat& fmt, QString* err) {
    Impl& im = *m_impl;
    const QJsonArray scenes = m_spec.value("scenes").toArray();
    if (scenes.isEmpty()) { *err = "libobs spec has no scenes"; return false; }
    int itemIndex = 0;
    for (const QJsonValue& sv : scenes) {
        const QJsonObject so = sv.toObject();
        const QString sname = so.value("name").toString(QString("scene%1").arg(im.scenes.size() + 1));
        obs_scene_t* scene = obs_scene_create(sname.toUtf8().constData());
        if (!scene) { *err = "obs_scene_create failed: " + sname; return false; }
        im.scenes.push_back({sname, scene});
        for (const QJsonValue& iv : so.value("items").toArray()) {
            const QJsonObject io = iv.toObject();
            const QString id = io.value("id").toString();
            const QString iname = io.value("name").toString(QString("%1_%2").arg(id).arg(++itemIndex));
            if (id.isEmpty()) { *err = "scene item without id in " + sname; return false; }
            if (!obs_source_get_display_name(id.toUtf8().constData())) { *err = "unknown libobs source id '" + id + "' (module not loaded?)"; return false; }
            QJsonObject settings = io.value("settings").toObject();
            if (id.startsWith("color_source")) {
                if (!settings.contains("width")) settings["width"] = fmt.width;
                if (!settings.contains("height")) settings["height"] = fmt.height;
            }
            obs_data_t* sd = settingsFromJson(settings);
            obs_source_t* src = obs_source_create(id.toUtf8().constData(), iname.toUtf8().constData(), sd, nullptr);
            obs_data_release(sd);
            if (!src) { *err = "obs_source_create failed: " + id; return false; }
            for (const QJsonValue& fv : io.value("filters").toArray()) {
                const QJsonObject fo = fv.toObject();
                const QString fid = fo.value("id").toString();
                if (!obs_source_get_display_name(fid.toUtf8().constData())) { *err = "unknown libobs filter id '" + fid + "'"; obs_source_release(src); return false; }
                obs_data_t* fd = settingsFromJson(fo.value("settings").toObject());
                obs_source_t* flt = obs_source_create(fid.toUtf8().constData(), fo.value("name").toString(fid).toUtf8().constData(), fd, nullptr);
                obs_data_release(fd);
                if (flt) { obs_source_filter_add(src, flt); obs_source_release(flt); }
            }
            obs_sceneitem_t* item = obs_scene_add(scene, src);
            if (!item) { *err = "obs_scene_add failed: " + iname; obs_source_release(src); return false; }
            const QString fit = io.value("fit").toString("stretch");
            struct vec2 bounds; bounds.x = float(fmt.width); bounds.y = float(fmt.height);
            if (io.contains("size")) { const QJsonObject sz = io.value("size").toObject(); bounds.x = float(sz.value("width").toDouble(fmt.width)); bounds.y = float(sz.value("height").toDouble(fmt.height)); }
            if (fit == "stretch") obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
            else if (fit == "inner") obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
            else if (fit == "outer") obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_OUTER);
            else obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
            if (fit != "none") obs_sceneitem_set_bounds(item, &bounds);
            if (io.contains("pos")) { const QJsonObject p = io.value("pos").toObject(); struct vec2 pos; pos.x = float(p.value("x").toDouble()); pos.y = float(p.value("y").toDouble()); obs_sceneitem_set_pos(item, &pos); }
            if (!io.value("visible").toBool(true)) obs_sceneitem_set_visible(item, false);
            im.items.push_back({iname, src, item, sname});   // we keep our create() reference until teardown
        }
    }
    // Transition + program
    const QJsonValue tv = m_spec.value("transition");
    QJsonObject tr = tv.isObject() ? tv.toObject() : QJsonObject();
    if (tv.isString()) tr["id"] = tv.toString();
    const QString tid = tr.value("id").toString("cut_transition");
    if (!obs_source_get_display_name(tid.toUtf8().constData())) { *err = "unknown transition id '" + tid + "' (obs-transitions module not loaded?)"; return false; }
    obs_data_t* td = settingsFromJson(tr.value("settings").toObject());
    im.transition = obs_source_create(tid.toUtf8().constData(), "pp_transition_1", td, nullptr);
    obs_data_release(td);
    if (!im.transition) { *err = "transition create failed: " + tid; return false; }
    im.transitionId = tid;
    obs_transition_set_size(im.transition, uint32_t(fmt.width), uint32_t(fmt.height));
    const QString program = m_spec.value("program").toString(im.scenes.front().name);
    Impl::Scene* ps = im.scene(program);
    if (!ps) { *err = "program scene not found: " + program; return false; }
    im.program = program;
    obs_transition_set(im.transition, obs_scene_get_source(ps->scene));
    obs_set_output_source(0, im.transition);
    return true;
}

void LibObsSource::teardownScenes() {
    Impl& im = *m_impl;
    if (im.transition) { obs_set_output_source(0, nullptr); obs_source_remove(im.transition); obs_source_release(im.transition); im.transition = nullptr; }
    for (auto& it : im.items) if (it.source) { obs_source_remove(it.source); obs_source_release(it.source); }
    im.items.clear();
    // OBS 33: the main canvas keeps its own reference on every scene (SCENE_REF) until obs_source_remove drops it,
    // exactly as the OBS frontend does when a scene is deleted. Without it the scene and its items leak.
    for (auto& s : im.scenes) if (s.scene) { obs_source_remove(obs_scene_get_source(s.scene)); obs_scene_release(s.scene); }
    im.scenes.clear();
    // libobs frees sources on a destruction thread; a scene's items are released only when that runs. Draining it
    // here keeps names free for the next open() and keeps obs_shutdown (which frees the source tables before it
    // waits on this queue) from finding "remaining" sources and double-destroying them.
    obs_wait_for_destroy_queue();
}

// ---------------------------------------------------------------------------
// Producer side: libobs' video / audio threads
// ---------------------------------------------------------------------------

void LibObsSource::rawVideo(void* param, struct video_data* frame) {
    auto* self = static_cast<LibObsSource*>(param);
    if (!frame || !frame->data[0] || !self->m_pool) return;
    FramePtr f = self->m_pool->get();
    if (!f) return;
    const int rowBytes = qMin(f->linesize[0], int(frame->linesize[0]));
    for (int y = 0; y < f->height; ++y)
        memcpy(f->data[0] + size_t(y) * f->linesize[0], frame->data[0] + size_t(y) * frame->linesize[0], size_t(rowBytes));
    f->pts = qint64(frame->timestamp);
    { QMutexLocker l(&self->m_mutex); self->m_impl->lastObsTs = qint64(frame->timestamp); self->m_impl->lastCopyNs = qint64(os_gettime_ns()); }
    self->m_impl->callbacks++;
    self->publishVideo(f);
}

void LibObsSource::rawAudio(void* param, size_t, struct audio_data* data) {
    auto* self = static_cast<LibObsSource*>(param);
    if (!data || data->frames == 0) return;
    const int ch = self->m_fmt.channels == 1 ? 1 : 2;
    const float* planes[2] = { reinterpret_cast<const float*>(data->data[0]), reinterpret_cast<const float*>(data->data[1]) };
    self->m_impl->audioCallbacks++;
    self->publishAudio(planes, true, ch, self->m_fmt.sampleRate, int(data->frames));
}

// ---------------------------------------------------------------------------
// Consumer side: clock thread
// ---------------------------------------------------------------------------

FramePtr LibObsSource::video(qint64 frame, double posSec) {
    FramePtr f = LiveSourceBase::video(frame, posSec);
    const qint64 now = qint64(os_gettime_ns());
    QMutexLocker l(&m_mutex);
    Impl& im = *m_impl;
    PullTiming t;
    t.pullNs = now;
    t.repeat = f && f.get() == im.lastServed;
    im.lastServed = f.get();
    if (f && f->pts > 0) {
        t.obsTimestampNs = f->pts;
        t.ageNs = now - f->pts;
        t.sinceCopyNs = now - im.lastCopyNs;
        im.ages[im.ageIdx] = t.ageNs;
        if (++im.ageIdx >= im.ages.size()) { im.ageIdx = 0; im.ageFull = true; }
    }
    im.lastPull = t;
    return f;
}

LibObsSource::PullTiming LibObsSource::lastPull() const { QMutexLocker l(&m_mutex); return m_impl->lastPull; }

// ---------------------------------------------------------------------------
// Commands (switch / item update) on the source's own thread
// ---------------------------------------------------------------------------

void LibObsSource::update(const QJsonObject& params) {
    if (!m_open) return;
    if (!params.contains("switch") && !params.contains("item")) return;
    QJsonObject cmd = params;
    cmd["_requestedNs"] = double(os_gettime_ns());
    { std::lock_guard<std::mutex> g(m_impl->cmdMutex); m_impl->cmds.push_back(cmd); }
    m_impl->cmdCv.notify_one();
}

void LibObsSource::workerLoop() {
    Impl& im = *m_impl;
    for (;;) {
        QJsonObject cmd;
        {
            std::unique_lock<std::mutex> lk(im.cmdMutex);
            im.cmdCv.wait(lk, [&] { return im.stop || !im.cmds.empty(); });
            if (im.stop) return;
            cmd = im.cmds.front(); im.cmds.pop_front();
        }
        if (cmd.contains("switch")) doSwitch(cmd);
        else if (cmd.contains("item")) doItemUpdate(cmd);
    }
}

void LibObsSource::doSwitch(const QJsonObject& cmd) {
    Impl& im = *m_impl;
    const QString target = cmd.value("switch").toString();
    Impl::Scene* ts = im.scene(target);
    if (!ts) { qWarning() << "[libobs] switch: no scene" << target; return; }
    // Transition for this switch: the spec's default, or one named in the command (string id or {id, durationMs, settings}).
    QJsonObject tr;
    const QJsonValue tv = cmd.value("transition");
    if (tv.isObject()) tr = tv.toObject(); else if (tv.isString()) tr["id"] = tv.toString();
    const QString tid = tr.value("id").toString(im.transitionId);
    if (tid != im.transitionId) {
        if (!obs_source_get_display_name(tid.toUtf8().constData())) { qWarning() << "[libobs] switch: unknown transition" << tid; return; }
        obs_data_t* td = settingsFromJson(tr.value("settings").toObject());
        const QByteArray tname = QString("pp_transition_%1").arg(im.switches + 2).toUtf8();   // unique: the old one lives until released
        obs_source_t* ntr = obs_source_create(tid.toUtf8().constData(), tname.constData(), td, nullptr);
        obs_data_release(td);
        if (!ntr) { qWarning() << "[libobs] switch: transition create failed" << tid; return; }
        obs_transition_set_size(ntr, uint32_t(m_fmt.width), uint32_t(m_fmt.height));
        if (Impl::Scene* cur = im.scene(im.program)) obs_transition_set(ntr, obs_scene_get_source(cur->scene));
        obs_set_output_source(0, ntr);
        obs_source_release(im.transition);
        im.transition = ntr; im.transitionId = tid;
    } else if (tr.contains("settings")) {
        obs_data_t* td = settingsFromJson(tr.value("settings").toObject());
        obs_source_update(im.transition, td);
        obs_data_release(td);
    }
    const uint32_t durationMs = uint32_t(cmd.value("durationMs").toInt(tr.value("durationMs").toInt(m_spec.value("transition").toObject().value("durationMs").toInt(300))));
    const bool ok = obs_transition_start(im.transition, OBS_TRANSITION_MODE_AUTO, durationMs, obs_scene_get_source(ts->scene));
    {
        QMutexLocker l(&m_mutex);
        im.switchRequestedNs = qint64(cmd.value("_requestedNs").toDouble());
        im.switchStartedNs = qint64(os_gettime_ns());
        im.switchTarget = target; im.switches++;
        if (ok) im.program = target;
        m_statusText = QString("libobs %1 program %2").arg(rt().version, im.program);
    }
    if (!ok) qWarning() << "[libobs] obs_transition_start refused (" << tid << "->" << target << ")";
}

void LibObsSource::doItemUpdate(const QJsonObject& cmd) {
    Impl& im = *m_impl;
    const QString name = cmd.value("item").toString();
    bool any = false;
    for (auto& it : im.items) {
        if (it.name != name || (cmd.contains("scene") && it.scene != cmd.value("scene").toString())) continue;
        any = true;
        if (cmd.value("settings").isObject()) { obs_data_t* d = settingsFromJson(cmd.value("settings").toObject()); obs_source_update(it.source, d); obs_data_release(d); }
        if (cmd.contains("visible")) obs_sceneitem_set_visible(it.item, cmd.value("visible").toBool(true));
        if (cmd.value("filter").isObject()) {
            const auto f = cmd.value("filter").toObject(); const QString id = f.value("id").toString(), filterName = f.value("name").toString(id);
            QString error;
            if (id != "chroma_key_filter_v2" && id != "color_key_filter_v2") error = "Unsupported live key filter: " + id;
            else {
                const auto bytes = filterName.toUtf8(); obs_source_t* filter = obs_source_get_filter_by_name(it.source, bytes.constData());
                if (f.value("remove").toBool()) { if (filter) obs_source_filter_remove(it.source, filter); }
                else {
                    obs_data_t* data = settingsFromJson(f.value("settings").toObject());
                    if (filter) obs_source_update(filter, data);
                    else { filter = obs_source_create(id.toUtf8().constData(), bytes.constData(), data, nullptr); if (filter) obs_source_filter_add(it.source, filter); else error = "Cannot create key filter: " + id; }
                    obs_data_release(data);
                }
                if (filter) obs_source_release(filter);
            }
            QMutexLocker l(&m_mutex); im.filterError = error; if (error.isEmpty()) ++im.filterUpdates;
        }
    }
    if (!any) { qWarning() << "[libobs] item update: no item" << name; if (cmd.contains("filter")) { QMutexLocker l(&m_mutex); im.filterError = "No matching item in this scene: " + name; } }
}

// ---------------------------------------------------------------------------
// Status (1 Hz telemetry)
// ---------------------------------------------------------------------------

QJsonObject LibObsSource::status() const {
    QJsonObject o = LiveSourceBase::status();
    Impl& im = *m_impl;
    QJsonObject obs;
    { QMutexLocker l(&rt().mutex); obs["version"] = rt().version; obs["started"] = rt().started; QJsonArray m; for (const QString& s : rt().modules) m << s; obs["modules"] = m; if (!rt().failedModules.isEmpty()) { QJsonArray f; for (const QString& s : rt().failedModules) f << s; obs["failedModules"] = f; } }
    obs["callbacks"] = double(im.callbacks.load()); obs["audioCallbacks"] = double(im.audioCallbacks.load());
    std::vector<qint64> ages;
    {
        QMutexLocker l(&m_mutex);
        obs["program"] = im.program; obs["transition"] = im.transitionId; obs["switches"] = im.switches;
        obs["filterUpdates"] = double(im.filterUpdates); obs["filterError"] = im.filterError;
        if (im.switchRequestedNs) { obs["switchTarget"] = im.switchTarget; obs["switchDispatchMs"] = double(im.switchStartedNs - im.switchRequestedNs) / 1e6; }
        if (im.lastPull.ageNs >= 0) { obs["lastAgeMs"] = double(im.lastPull.ageNs) / 1e6; obs["lastSinceCopyMs"] = double(im.lastPull.sinceCopyNs) / 1e6; }
        const size_t n = im.ageFull ? im.ages.size() : im.ageIdx;
        ages.assign(im.ages.begin(), im.ages.begin() + qint64(n));
        QJsonArray sc; for (const auto& s : im.scenes) sc << s.name; obs["scenes"] = sc;
    }
    if (!ages.empty()) {
        obs["ageSamples"] = double(ages.size());
        obs["ageP50Ms"] = pctMs(ages, 0.50); obs["ageP99Ms"] = pctMs(ages, 0.99); obs["ageMaxMs"] = double(*std::max_element(ages.begin(), ages.end())) / 1e6;
    }
    if (rt().started) { obs["obsFps"] = obs_get_active_fps(); obs["obsFrameTimeMs"] = double(obs_get_average_frame_time_ns()) / 1e6; obs["obsTotalFrames"] = double(obs_get_total_frames()); obs["obsLaggedFrames"] = double(obs_get_lagged_frames()); }
    o["obs"] = obs;
    return o;
}
