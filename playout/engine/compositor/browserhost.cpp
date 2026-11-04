#include "browserhost.h"
#include "../../core/browserlocator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRunnable>
#include <QSemaphore>
#include <QTcpServer>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QWaitCondition>
#include <QWebSocket>
#include <chrono>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ===========================================================================
// Shared helpers
// ===========================================================================

namespace {

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool verbose() {
    static const bool v = qEnvironmentVariableIntValue("PLAYPLAY_HTML_VERBOSE") != 0;
    return v;
}

/// PNG decoding runs here, never on the host thread: two threads are enough for ~58 frames/s of 1080p in total
/// (a decode is ~15-25 ms) and keep the cores free for the encoders.
QThreadPool* decodePool() {
    static QThreadPool* pool = [] { auto* p = new QThreadPool(); p->setMaxThreadCount(2); p->setThreadPriority(QThread::LowPriority); return p; }();
    return pool;
}

QString valueToString(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 15);
    if (v.isBool()) return v.toBool() ? "true" : "false";
    if (v.isNull() || v.isUndefined()) return QString();
    return QString::fromUtf8(QJsonDocument(v.isObject() ? QJsonDocument(v.toObject()) : QJsonDocument(v.toArray())).toJson(QJsonDocument::Compact));
}

/// JS that writes `fields` into the page: [data-field=name], #name and #nameText (the lower-third template names its
/// elements nameText/titleText); IMG gets src, INPUT/TEXTAREA/SELECT value, everything else textContent. Also calls a
/// CasparCG-style window.update(fields) when the template defines it and dispatches a `pp:fields` event. Returns the
/// number of elements/handlers reached.
QString fieldsScript(const QJsonObject& fields) {
    QString json = QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact));
    json.replace(QChar(0x2028), QStringLiteral("\\u2028")).replace(QChar(0x2029), QStringLiteral("\\u2029"));
    return QStringLiteral(
               "(function(f){var n=0;"
               "var set=function(e,v){var t=e.tagName;if(t==='IMG'){e.src=v}else if(t==='INPUT'||t==='TEXTAREA'||t==='SELECT'){e.value=v}else{e.textContent=v}n++};"
               "for(var k in f){var v=(f[k]===null||f[k]===undefined)?'':String(f[k]);"
               "var q=(window.CSS&&CSS.escape)?CSS.escape(k):k;"
               "document.querySelectorAll('[data-field=\"'+k.replace(/\"/g,'')+'\"]').forEach(function(e){set(e,v)});"
               "[k,k+'Text'].forEach(function(i){var e=document.getElementById(i);if(e){set(e,v)}})}"
               "try{if(typeof window.update==='function'){window.update(f);n++}}catch(e){}"
               "try{window.dispatchEvent(new CustomEvent('pp:fields',{detail:f}))}catch(e){}"
               "return n})(")
        + json + QStringLiteral(")");
}

} // namespace

// ===========================================================================
// PE-19: screencast ingest helpers
// ===========================================================================

namespace htmlingest {

Digest digest(const QByteArray& bytes) {
    // qHashBits is QtCore's AES-NI / SipHash hash: a 2 MB payload hashes in a fraction of a millisecond, far cheaper than
    // the base64 + PNG decode it can save. A match is confirmed byte for byte by the caller (sameBytes).
    Digest d;
    d.n = bytes.size();
    d.h = quint64(qHashBits(bytes.constData(), size_t(bytes.size()), size_t(0x9E3779B97F4A7C15ull)));
    return d;
}

bool splitScreencastData(QStringView text, QByteArray* b64, QString* rest) {
    // Chrome writes {"method":"Page.screencastFrame","params":{"data":"<base64>","metadata":{...},"sessionId":N},"sessionId":"..."}.
    // A literal "data":" can only be a key followed by a string value: inside a JSON string the quotes would be escaped.
    static constexpr QLatin1StringView key("\"data\":\"");
    const qsizetype k = text.indexOf(key);
    if (k < 0) return false;
    const qsizetype s = k + key.size();
    const qsizetype e = text.indexOf(u'"', s);
    if (e < 0) return false;
    const QStringView data = text.sliced(s, e - s);
    if (data.contains(u'\\')) return false;                  // escaped (Chrome never escapes base64): the slow path handles it
    *b64 = data.toLatin1();
    rest->clear();
    rest->reserve(text.size() - data.size());
    rest->append(text.first(s)).append(text.sliced(e));
    return true;
}

int decodeThreadsFor(int livePages) {
    static const int forced = qEnvironmentVariableIntValue("PLAYPLAY_HTML_DECODE_THREADS");
    if (forced > 0) return qBound(1, forced, 16);
    return qBound(2, livePages, 8);
}

bool fastParseEnabled() {
    static const bool v = qEnvironmentVariable("PLAYPLAY_HTML_FASTPARSE").trimmed() != QLatin1String("0");
    return v;
}

bool skipSameDefault() { return qEnvironmentVariable("PLAYPLAY_HTML_SKIP_SAME").trimmed() != QLatin1String("0"); }

} // namespace htmlingest

// ===========================================================================
// HtmlPageShared: everything the clock thread, the loader thread and the decode jobs touch
// ===========================================================================

struct HtmlPageShared {
    QMutex mtx;
    QWaitCondition cond;
    FramePtr latest;
    QSize size;
    quint64 frames = 0;               // published (decoded) frames
    qint64 lastFrameMs = 0;           // steady ms of the last published frame
    double decodeMs = 0;
    qint64 times[32] = {};            // publish times for the fps estimate
    int tHead = 0, tCount = 0;
    bool failed = false;              // the OPEN failed before any frame (waitForFirstFrame returns false)
    bool premul = false;              // PE-08: decode jobs publish premultiplied BGRA (fixed at creation)
    bool pngPremul = false;           // PE-08: the PNG bytes already are premultiplied (HtmlPageOptions::pngPremultiplied)
    bool skipSame = true;             // PE-19: identical payloads skip the decode (PLAYPLAY_HTML_SKIP_SAME, fixed at creation)
    double base64Ms = 0;              // PE-19: smoothed base64 decode time (in the decode job since PE-19)
    QString lastError, url, label, targetId;

    std::atomic<int> state{int(HtmlPage::State::Starting)};
    std::atomic<qint64> framesIn{0}, framesDropped{0}, decodeFailures{0}, lastPollMs{0};
    std::atomic<qint64> framesSkipped{0};                     // PE-19: payloads identical to the newest picture
    std::atomic<int> restarts{0}, nth{0};
    std::atomic<bool> paused{false}, resumeQueued{false}, decoding{false};
    std::atomic<bool> decodeFailed{false};                    // PE-19: the last decode failed: forget its digest
    std::unique_ptr<FramePool> pool;  // decode jobs only (one in flight per page)

    double fpsLocked(qint64 now) const {
        if (tCount < 2) return 0.0;
        const qint64 newest = times[(tHead - 1 + 32) % 32];
        if (now - newest > 1000) return 0.0;
        qint64 oldest = newest; int n = 0;
        for (int i = 0; i < tCount; ++i) { const qint64 t = times[(tHead - 1 - i + 64) % 32]; if (now - t > 2000) break; oldest = t; ++n; }
        return (n >= 2 && newest > oldest) ? double(n - 1) * 1000.0 / double(newest - oldest) : 0.0;
    }
};

namespace {

/// One decode job: PNG -> straight-alpha ARGB32 -> pooled BGRA FramePtr (the engine's overlay convention: the
/// compositor reads overlay frames as QImage::Format_ARGB32) -> publish.
/// PE-08 (sh->premul): PNG -> premultiplied ARGB32 here, once per picture and off the clock thread, so the compositor
/// blends it as it is instead of premultiplying the page's visible box on every draw of every mix. An opaque page
/// (RGB32, alpha 0xFF) is valid premultiplied already and is not converted.
/// PE-08 (sh->pngPremul): headless Chromium writes PREMULTIPLIED colours into its screencast PNGs (measured: a 50 % red box
/// decodes as 128,0,0,128, 25 % white as 64,64,64,64), so reading them as straight premultiplies translucency twice. With
/// the flag the bytes are taken for what they are: as is for a premultiplied page (no conversion at all), un-premultiplied
/// once for a straight one.
/// PE-19: `b64` is the screencast's base64 text; it is decoded here, off the host thread. decodeMs stays PNG decode +
/// convert + copy (comparable with before); the base64 step is base64Ms.
void runDecode(std::shared_ptr<HtmlPageShared> sh, QByteArray b64) {
    QElapsedTimer tb; tb.start();
    const QByteArray png = QByteArray::fromBase64(b64);
    b64 = QByteArray();
    const double b64ms = double(tb.nsecsElapsed()) / 1e6;
    QElapsedTimer t; t.start();
    QImage img;
    FramePtr f;
    if (img.loadFromData(png, "PNG") && !img.isNull()) {
        if (sh->pngPremul) {
            if (img.format() != QImage::Format_ARGB32 && img.format() != QImage::Format_RGB32) img.convertTo(QImage::Format_ARGB32);
            if (img.format() == QImage::Format_ARGB32) img.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
            if (!sh->premul && img.format() == QImage::Format_ARGB32_Premultiplied) img.convertTo(QImage::Format_ARGB32);
        }
        else if (sh->premul) { if (img.format() != QImage::Format_ARGB32_Premultiplied && img.format() != QImage::Format_RGB32) img.convertTo(QImage::Format_ARGB32_Premultiplied); }
        else if (img.format() != QImage::Format_ARGB32) img = img.convertToFormat(QImage::Format_ARGB32);
        const int w = img.width(), h = img.height();
        if (!sh->pool || sh->pool->width() != w || sh->pool->height() != h) sh->pool = std::make_unique<FramePool>(w, h, kMixerPixelFormat);
        f = sh->pool->get();
        if (f) for (int y = 0; y < h; ++y) memcpy(f->data[0] + size_t(y) * f->linesize[0], img.constScanLine(y), size_t(w) * 4);
    }
    const double ms = double(t.nsecsElapsed()) / 1e6;
    if (!f) { sh->decodeFailures++; sh->decodeFailed = true; }
    {
        QMutexLocker l(&sh->mtx);
        sh->base64Ms = sh->base64Ms <= 0 ? b64ms : sh->base64Ms * 0.9 + b64ms * 0.1;
        if (f) {
            sh->latest = f;
            sh->size = QSize(f->width, f->height);
            ++sh->frames;
            sh->lastFrameMs = nowMs();
            sh->times[sh->tHead] = sh->lastFrameMs; sh->tHead = (sh->tHead + 1) % 32; if (sh->tCount < 32) ++sh->tCount;
            sh->decodeMs = sh->frames == 1 ? ms : sh->decodeMs * 0.9 + ms * 0.1;
        }
        sh->cond.wakeAll();
    }
    sh->decoding = false;
}

} // namespace

// ===========================================================================
// HtmlPage
// ===========================================================================

QString HtmlPage::stateName(State s) {
    switch (s) {
    case State::Starting: return "starting";
    case State::Loading: return "loading";
    case State::Streaming: return "streaming";
    case State::Paused: return "paused";
    case State::Restarting: return "restarting";
    case State::Failed: return "failed";
    case State::Closed: return "closed";
    }
    return "unknown";
}

HtmlPage::HtmlPage(BrowserHost* host, const HtmlPageOptions& opt, std::shared_ptr<HtmlPageShared> sh)
    : QObject(nullptr), m_host(host), m_opt(opt), m_sh(std::move(sh)) {
    m_label = opt.label.isEmpty() ? QStringLiteral("page") : opt.label;
    m_url = opt.url;
    m_createdMs = nowMs();
    m_minIntervalMs = opt.maxFps > 0 ? qMax<qint64>(1, qint64(1000.0 / opt.maxFps) - 3) : 0;
    m_nth = opt.everyNthFrame > 0 ? opt.everyNthFrame : 1;
    m_fields = opt.fields;
    m_pumpTimer.setSingleShot(true);
    connect(&m_pumpTimer, &QTimer::timeout, this, &HtmlPage::pump);
    m_sh->lastPollMs = m_createdMs;
    m_sh->nth = m_nth;
}

HtmlPage::~HtmlPage() { if (!m_closed) doClose(); }

// ---- thread-safe surface ----

FramePtr HtmlPage::latestFrame() {
    m_sh->lastPollMs = nowMs();
    if (m_sh->paused.load() && !m_sh->resumeQueued.exchange(true)) {
        QMetaObject::invokeMethod(this, [this] {
            m_sh->resumeQueued = false;
            resumeIfPaused();
        }, Qt::QueuedConnection);
    }
    QMutexLocker l(&m_sh->mtx);
    return m_sh->latest;
}

QSize HtmlPage::frameSize() const { QMutexLocker l(&m_sh->mtx); return m_sh->size; }
quint64 HtmlPage::frameCount() const { QMutexLocker l(&m_sh->mtx); return m_sh->frames; }
HtmlPage::State HtmlPage::state() const { return State(m_sh->state.load()); }
QString HtmlPage::url() const { QMutexLocker l(&m_sh->mtx); return m_sh->url; }
QString HtmlPage::lastError() const { QMutexLocker l(&m_sh->mtx); return m_sh->lastError; }

bool HtmlPage::waitForFirstFrame(int timeoutMs, QString* error) {
    QElapsedTimer t; t.start();
    QMutexLocker l(&m_sh->mtx);
    while (m_sh->frames == 0 && !m_sh->failed) {
        const qint64 left = qint64(timeoutMs) - t.elapsed();
        if (left <= 0) break;
        m_sh->cond.wait(&m_sh->mtx, left);
    }
    if (m_sh->failed) { if (error) *error = m_sh->lastError; return false; }
    if (m_sh->frames > 0) return true;
    if (error) {
        *error = QString("no frame from %1 within %2 ms (page state: %3%4)").arg(m_sh->url).arg(timeoutMs).arg(stateName(state()),
                     m_sh->lastError.isEmpty() ? QString() : QString("; last error: %1").arg(m_sh->lastError));
    }
    return false;
}

QJsonObject HtmlPage::status() const {
    QJsonObject o;
    const qint64 now = nowMs();
    QMutexLocker l(&m_sh->mtx);
    o["url"] = m_sh->url;
    o["state"] = stateName(State(m_sh->state.load()));
    o["frames"] = double(m_sh->frames);
    o["framesIn"] = double(m_sh->framesIn.load());
    o["framesDropped"] = double(m_sh->framesDropped.load());
    o["decodeFailures"] = double(m_sh->decodeFailures.load());
    o["fps"] = double(qRound(m_sh->fpsLocked(now) * 10.0)) / 10.0;
    if (m_sh->lastFrameMs) o["lastFrameAgeMs"] = double(now - m_sh->lastFrameMs);
    o["restarts"] = m_sh->restarts.load();
    o["lastError"] = m_sh->lastError;
    o["width"] = m_sh->size.width();
    o["height"] = m_sh->size.height();
    o["decodeMs"] = double(qRound(m_sh->decodeMs * 10.0)) / 10.0;
    o["everyNthFrame"] = m_sh->nth.load();
    o["paused"] = m_sh->paused.load();
    o["targetId"] = m_sh->targetId;
    if (m_sh->premul) o["premultiplied"] = true;        // PE-08 (only when on: the default status JSON is unchanged)
    if (m_sh->pngPremul) o["pngPremultiplied"] = true;
    o["framesSkipped"] = double(m_sh->framesSkipped.load());  // PE-19: identical payloads not decoded again
    o["base64Ms"] = double(qRound(m_sh->base64Ms * 10.0)) / 10.0;
    if (!m_sh->skipSame) o["skipSame"] = false;
    return o;
}

void HtmlPage::navigate(const QString& url) {
    QMetaObject::invokeMethod(this, [this, url] { doNavigate(url, true); }, Qt::QueuedConnection);
}

void HtmlPage::evaluate(const QString& js, std::function<void(bool, const QJsonValue&)> done) {
    QMetaObject::invokeMethod(this, [this, js, done] { doEvaluate(js, done); }, Qt::QueuedConnection);
}

void HtmlPage::setFields(const QJsonObject& fields, bool reloadOnMiss) {
    QMetaObject::invokeMethod(this, [this, fields, reloadOnMiss] { doSetFields(fields, reloadOnMiss); }, Qt::QueuedConnection);
}

void HtmlPage::animate(const QString& what) {
    QString js;
    if (what == "out") js = QStringLiteral("window.CGAnim && window.CGAnim.animateOut && (window.CGAnim.animateOut(), true)");
    else if (what == "in") js = QStringLiteral("window.CGAnim && window.CGAnim.animateIn && (window.CGAnim.animateIn(), true)");
    else if (what == "replay") js = QStringLiteral("window.CGAnim && window.CGAnim.replay && (window.CGAnim.replay(), true)");
    else return;
    evaluate(js);
}

void HtmlPage::close() {
    QMetaObject::invokeMethod(this, [this] { doClose(); }, Qt::QueuedConnection);
}

// ---- host thread ----

void HtmlPage::setState(State s) {
    const State old = State(m_sh->state.exchange(int(s)));
    if (old != s) qInfo().noquote() << QString("[Html] %1 %2 -> %3").arg(m_label, stateName(old), stateName(s));
}

bool HtmlPage::isLive() const {
    const State s = state();
    return m_attached && (s == State::Loading || s == State::Streaming);
}

void HtmlPage::logRate(const QString& level, const QString& text) {
    const qint64 now = nowMs();
    if (now - m_logWindowMs > 5000) {
        if (m_logSuppressed > 0) qWarning().noquote() << QString("[Html] %1 (%2 more messages suppressed)").arg(m_label).arg(m_logSuppressed);
        m_logWindowMs = now; m_logCount = 0; m_logSuppressed = 0;
    }
    if (m_logCount >= 8) { ++m_logSuppressed; return; }
    ++m_logCount;
    if (level == "warn") qWarning().noquote() << QString("[Html] %1 %2").arg(m_label, text);
    else qInfo().noquote() << QString("[Html] %1 %2").arg(m_label, text);
}

void HtmlPage::noteError(const QString& why, bool warn) {
    { QMutexLocker l(&m_sh->mtx); m_sh->lastError = why; }
    if (warn) logRate("warn", why);
}

void HtmlPage::failOpen(const QString& why) {
    noteError(why, true);
    QMutexLocker l(&m_sh->mtx);
    if (m_sh->frames == 0) {
        m_sh->failed = true;
        m_sh->state = int(State::Failed);
        m_sh->cond.wakeAll();
    }
}

void HtmlPage::closeTargetId(const QString& targetId) {
    if (targetId.isEmpty() || !m_host || !m_host->ready()) return;
    m_host->send("Target.closeTarget", QJsonObject{{"targetId", targetId}});
}

void HtmlPage::attach() {
    if (m_closed || !m_host || !m_host->ready()) return;
    const int gen = ++m_gen;
    if (m_attached) { const QString old = m_targetId; dropSession(); closeTargetId(old); }
    m_loaded = false; m_screencasting = false; m_expectFrame = false; m_mainFrameId.clear(); m_httpError.clear();
    { QMutexLocker l(&m_sh->mtx); if (m_sh->frames == 0) m_sh->state = int(State::Starting); else m_sh->state = int(State::Restarting); }
    QJsonObject p{{"url", "about:blank"}, {"width", m_opt.width}, {"height", m_opt.height}, {"newWindow", true}};
    m_host->send("Target.createTarget", p, QString(), this, [this, gen](const QJsonObject& r, const QString& e) {
        const QString tid = r.value("targetId").toString();
        if (m_closed || gen != m_gen) { if (e.isEmpty()) closeTargetId(tid); return; }
        if (!e.isEmpty() || tid.isEmpty()) { noteError("Target.createTarget failed: " + (e.isEmpty() ? QString("no targetId") : e), true); reattachSoon(2000); return; }
        m_targetId = tid;
        m_host->send("Target.attachToTarget", QJsonObject{{"targetId", tid}, {"flatten", true}}, QString(), this,
                     [this, gen, tid](const QJsonObject& r2, const QString& e2) {
            if (m_closed || gen != m_gen) { closeTargetId(tid); return; }
            const QString sid = r2.value("sessionId").toString();
            if (!e2.isEmpty() || sid.isEmpty()) { noteError("Target.attachToTarget failed: " + (e2.isEmpty() ? QString("no sessionId") : e2), true); closeTargetId(tid); m_targetId.clear(); reattachSoon(2000); return; }
            m_sessionId = sid; m_attached = true;
            m_host->bindSession(sid, this);
            m_host->targetAttached(true);
            { QMutexLocker l(&m_sh->mtx); m_sh->targetId = tid; }
            setup(gen);
        });
    });
}

void HtmlPage::dropSession() {
    if (!m_sessionId.isEmpty() && m_host) m_host->unbindSession(m_sessionId);
    if (m_attached && m_host) m_host->targetAttached(false);
    m_attached = false; m_sessionId.clear(); m_targetId.clear();
    m_screencasting = false; m_loaded = false; m_expectFrame = false;
    ++m_gen;
    QMutexLocker l(&m_sh->mtx); m_sh->targetId.clear();
}

void HtmlPage::setup(int gen) {
    const QString sid = m_sessionId;
    m_host->send("Page.enable", QJsonObject(), sid);
    m_host->send("Runtime.enable", QJsonObject(), sid);
    m_host->send("Network.enable", QJsonObject(), sid);
    m_host->send("Emulation.setDeviceMetricsOverride", QJsonObject{{"width", m_opt.width}, {"height", m_opt.height}, {"deviceScaleFactor", 1}, {"mobile", false}}, sid);
    if (m_opt.transparent)
        m_host->send("Emulation.setDefaultBackgroundColorOverride", QJsonObject{{"color", QJsonObject{{"r", 0}, {"g", 0}, {"b", 0}, {"a", 0}}}}, sid);
    setState(State::Loading);
    m_loadStartMs = nowMs();
    m_loaded = false;
    m_host->send("Page.navigate", QJsonObject{{"url", m_url}}, sid, this, [this, gen](const QJsonObject& r, const QString& e) { onNavigateReply(gen, r, e); });
}

void HtmlPage::onNavigateReply(int gen, const QJsonObject& r, const QString& e) {
    if (m_closed || gen != m_gen) return;
    if (!e.isEmpty()) { failOpen("Page.navigate failed: " + e); return; }
    m_mainFrameId = r.value("frameId").toString();
    const QString errText = r.value("errorText").toString();
    if (!errText.isEmpty()) failOpen(QString("page load failed: %1 (%2)").arg(errText, m_url.left(200)));
}

void HtmlPage::onLoaded() {
    m_loaded = true;
    if (!m_fields.isEmpty()) m_host->send("Runtime.evaluate", QJsonObject{{"expression", fieldsScript(m_fields)}, {"returnByValue", true}, {"silent", true}}, m_sessionId);
    flushQueued();
    if (state() == State::Loading) startStreaming();
    else if (m_screencasting) armWatchdog(true);              // re-take: a frame must follow the load
}

void HtmlPage::startStreaming() {
    if (m_closed || m_sessionId.isEmpty()) return;
    m_host->send("Page.bringToFront", QJsonObject(), m_sessionId);
    startScreencast();
    if (state() != State::Failed) setState(State::Streaming);
}


// Chrome's screencast is damage-driven: a static page that finished painting before the screencast started never
// produces a frame. Nudge one repaint: a near-transparent probe pixel appears for two animation frames, then goes,
// so the last frame delivered is pixel-identical to the page.
static const char* kRepaintKick =
    "(()=>{const d=document.createElement('div');d.style.cssText='position:fixed;left:3px;top:3px;width:1px;height:1px;"
    "pointer-events:none;z-index:2147483647;background:rgba(0,0,0,0.004)';(document.body||document.documentElement).appendChild(d);"
    "requestAnimationFrame(()=>requestAnimationFrame(()=>d.remove()));return true})()";
void HtmlPage::startScreencast() {
    int nth = m_host->forcedNth() > 0 ? m_host->forcedNth() : (m_opt.everyNthFrame > 0 ? m_opt.everyNthFrame : m_host->autoNth());
    nth = qBound(1, nth, 8);
    m_nth = nth; m_sh->nth = nth;
    m_host->send("Page.startScreencast", QJsonObject{{"format", "png"}, {"everyNthFrame", nth}}, m_sessionId);
    m_screencasting = true;
    m_host->send("Runtime.evaluate", QJsonObject{{"expression", QString::fromLatin1(kRepaintKick)}, {"silent", true}}, m_sessionId);
    m_sh->paused = false;
    armWatchdog(true);
}

void HtmlPage::armWatchdog(bool reset) {
    m_expectFrame = true;
    m_armMs = nowMs();
    if (reset) m_wdAttempts = 0;
}

void HtmlPage::restartScreencast(const char* why) {
    if (m_closed || m_sessionId.isEmpty()) return;
    qInfo().noquote() << QString("[Html] %1 restarting screencast (%2, attempt %3)").arg(m_label, QString::fromLatin1(why)).arg(m_wdAttempts);
    m_host->send("Page.bringToFront", QJsonObject(), m_sessionId);
    m_host->send("Page.stopScreencast", QJsonObject(), m_sessionId);
    m_host->send("Page.startScreencast", QJsonObject{{"format", "png"}, {"everyNthFrame", m_nth}}, m_sessionId);
    m_screencasting = true;
    m_host->send("Runtime.evaluate", QJsonObject{{"expression", QString::fromLatin1(kRepaintKick)}, {"silent", true}}, m_sessionId);
    armWatchdog(false);
}

void HtmlPage::setNth(int nth) {
    nth = qBound(1, nth, 8);
    if (nth == m_nth) return;
    m_nth = nth; m_sh->nth = nth;
    if (m_screencasting && !m_sessionId.isEmpty()) restartScreencast("everyNthFrame changed");
}

void HtmlPage::resumeIfPaused() {
    if (m_closed || state() != State::Paused) return;
    m_sh->paused = false;
    startScreencast();
    setState(State::Streaming);
}

void HtmlPage::slowTick(qint64 now) {
    if (m_closed || m_sessionId.isEmpty()) return;
    // The load event never came (hung subresource): stream what the page shows anyway.
    if (state() == State::Loading && now - m_loadStartMs > m_opt.loadTimeoutMs) {
        noteError(QString("load event not fired within %1 ms; streaming anyway").arg(m_opt.loadTimeoutMs), true);
        startStreaming();
    }
    if (m_attached && !m_loaded && now - m_loadStartMs > 2 * qint64(m_opt.loadTimeoutMs) + 2000) { m_loaded = true; flushQueued(); }
    // Frame watchdog: a window that stays without frames after start/navigation is revived by stop+start.
    if (m_screencasting && m_expectFrame) {
        const qint64 wait = m_wdAttempts < 3 ? 1500 : 10000;
        if (now - m_armMs >= wait) {
            if (m_wdAttempts == 3) noteError("no frames from the page after 3 screencast restarts (still retrying)", true);
            ++m_wdAttempts;
            restartScreencast("no frame");
        }
    }
    // Idle pause: nobody asked for a frame (layer hidden): stop the screencast and the decodes.
    if (m_opt.idlePauseMs > 0 && state() == State::Streaming && m_screencasting && !m_expectFrame && m_sh->frames > 0
        && now - m_sh->lastPollMs.load() > m_opt.idlePauseMs) {
        m_host->send("Page.stopScreencast", QJsonObject(), m_sessionId);
        m_screencasting = false;
        m_sh->paused = true;
        setState(State::Paused);
    }
}

void HtmlPage::onBrowserGone() {
    if (m_closed) return;
    dropSession();
    m_sh->restarts++;
    m_pending.clear();
    for (const Queued& q : m_queued) if (q.done) q.done(false, QJsonValue(QStringLiteral("browser restarted")));
    m_queued.clear();
    setState(State::Restarting);
}

void HtmlPage::onBrowserShutdown() {
    if (m_closed) return;
    m_closed = true; ++m_gen;
    m_sessionId.clear(); m_targetId.clear(); m_attached = false;
    setState(State::Closed);
    QMutexLocker l(&m_sh->mtx);
    if (m_sh->frames == 0) { m_sh->failed = true; if (m_sh->lastError.isEmpty()) m_sh->lastError = "browser shut down"; }
    m_sh->cond.wakeAll();
}

void HtmlPage::onTargetLost(const QString& why) {
    if (m_closed) return;
    noteError("page lost: " + why, true);
    const QString tid = m_targetId;
    dropSession();
    closeTargetId(tid);
    m_sh->restarts++;
    setState(State::Restarting);
    reattachSoon(500);
}

void HtmlPage::onTargetDestroyed(const QString& targetId) {
    if (!m_closed && !targetId.isEmpty() && targetId == m_targetId) onTargetLost("target destroyed");
}

void HtmlPage::reattachSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { if (!m_closed && !m_attached && m_host && m_host->ready()) attach(); });
}

void HtmlPage::onEvent(const QString& method, const QJsonObject& params) {
    if (m_closed) return;
    if (method == "Page.screencastFrame") onScreencastFrame(params);
    else if (method == "Page.loadEventFired") onLoaded();
    else if (method == "Page.javascriptDialogOpening") {
        // alert/confirm/prompt must never block the render: dismiss (beforeunload is accepted so a re-take can navigate)
        const QString type = params.value("type").toString();
        logRate("info", QString("dismissed JS %1 dialog: %2").arg(type, params.value("message").toString().left(120)));
        m_host->send("Page.handleJavaScriptDialog", QJsonObject{{"accept", type == "beforeunload"}}, m_sessionId);
    }
    else if (method == "Runtime.consoleAPICalled") onConsole(params);
    else if (method == "Runtime.exceptionThrown") onException(params);
    else if (method == "Network.responseReceived") onResponse(params);
    else if (method == "Inspector.targetCrashed") onTargetLost("renderer crashed");
}

void HtmlPage::onScreencastFrame(const QJsonObject& params, QByteArray b64) {
    m_host->send("Page.screencastFrameAck", QJsonObject{{"sessionId", params.value("sessionId").toInt()}}, m_sessionId);
    m_sh->framesIn++;
    m_expectFrame = false; m_wdAttempts = 0;
    if (b64.isNull()) b64 = params.value("data").toString().toLatin1();
    if (m_sh->decodeFailed.exchange(false)) { m_sentDigest = htmlingest::Digest(); m_sent.clear(); }   // a failed picture is never "the same"
    if (m_sh->skipSame && !b64.isEmpty()) {
        // PE-19: the screencast is damage-driven, but an animation that ends on the same pixels (a looping element under an
        // opaque box, a blinking caret off screen, opacity steps that round to the same bytes) still sends whole PNGs.
        // The picture that will be on air next is m_pending when one waits, else the one last handed to a decode.
        using htmlingest::sameBytes;
        const htmlingest::Digest d = htmlingest::digest(b64);
        if (m_pending.isEmpty() ? sameBytes(d, b64, m_sentDigest, m_sent) : sameBytes(d, b64, m_pendingDigest, m_pending)) { m_sh->framesSkipped++; return; }
        if (!m_pending.isEmpty() && sameBytes(d, b64, m_sentDigest, m_sent)) {   // newer than the waiting one, equal to what airs
            m_pending.clear(); m_pendingDigest = htmlingest::Digest();
            m_sh->framesDropped++; m_sh->framesSkipped++;
            return;
        }
        m_pendingDigest = d;
    }
    if (!m_pending.isEmpty()) m_sh->framesDropped++;          // the previous picture was never decoded: this one is newer
    m_pending = std::move(b64);
    pump();
}

void HtmlPage::pump() {
    if (m_pending.isEmpty()) return;
    if (m_sh->decoding.load()) { m_pumpTimer.start(4); return; }
    const qint64 now = nowMs();
    if (m_minIntervalMs > 0 && now - m_lastDecodeStartMs < m_minIntervalMs) { m_pumpTimer.start(int(m_minIntervalMs - (now - m_lastDecodeStartMs))); return; }
    QByteArray b64 = std::move(m_pending);
    m_pending.clear();
    m_sentDigest = m_pendingDigest; m_pendingDigest = htmlingest::Digest();
    m_sent = m_sh->skipSame ? b64 : QByteArray();            // implicitly shared with the decode job's copy
    m_lastDecodeStartMs = now;
    m_sh->decoding = true;
    std::shared_ptr<HtmlPageShared> sh = m_sh;
    decodePool()->start(QRunnable::create([sh, b64]() mutable { runDecode(std::move(sh), std::move(b64)); }));
}

void HtmlPage::onConsole(const QJsonObject& params) {
    const QString type = params.value("type").toString();
    const bool isErr = type == "error" || type == "assert";
    if (!isErr && type != "warning" && !verbose()) return;
    QStringList parts;
    for (const QJsonValue& a : params.value("args").toArray()) {
        const QJsonObject o = a.toObject();
        parts << (o.contains("value") ? valueToString(o.value("value")) : o.value("description").toString());
    }
    logRate(isErr ? "warn" : "info", QString("console.%1: %2").arg(type, parts.join(' ').left(300)));
}

void HtmlPage::onException(const QJsonObject& params) {
    const QJsonObject d = params.value("exceptionDetails").toObject();
    QString text = d.value("exception").toObject().value("description").toString();
    if (text.isEmpty()) text = d.value("text").toString();
    logRate("warn", QString("uncaught: %1").arg(text.left(300)));
}

void HtmlPage::onResponse(const QJsonObject& params) {
    if (params.value("type").toString() != "Document") return;
    const QString fid = params.value("frameId").toString();
    if (!m_mainFrameId.isEmpty() && !fid.isEmpty() && fid != m_mainFrameId) return;
    const QJsonObject resp = params.value("response").toObject();
    const int status = resp.value("status").toInt();
    if (status < 400) return;
    m_httpError = QString("HTTP %1 %2 for %3").arg(status).arg(resp.value("statusText").toString(), resp.value("url").toString().left(200));
    if (m_sh->frames == 0) failOpen(m_httpError); else noteError(m_httpError, true);
}

void HtmlPage::doNavigate(const QString& url, bool explicitTake) {
    if (m_closed) return;
    m_url = url;
    { QMutexLocker l(&m_sh->mtx); m_sh->url = url; }
    m_sh->lastPollMs = nowMs();
    m_loaded = false; m_httpError.clear(); m_mainFrameId.clear();
    if (explicitTake) m_fields = QJsonObject();               // a re-take shows what the NEW url says, not stale field edits
    resumeIfPaused();
    if (!m_attached || m_sessionId.isEmpty()) return;          // the pending (re-)attach loads m_url
    m_loadStartMs = nowMs();
    const int gen = m_gen;
    m_host->send("Page.navigate", QJsonObject{{"url", url}}, m_sessionId, this, [this, gen](const QJsonObject& r, const QString& e) { onNavigateReply(gen, r, e); });
}

void HtmlPage::doEvaluate(const QString& js, std::function<void(bool, const QJsonValue&)> done) {
    if (m_closed) { if (done) done(false, QJsonValue(QStringLiteral("page closed"))); return; }
    m_sh->lastPollMs = nowMs();
    resumeIfPaused();
    if (!m_attached || !m_loaded || m_sessionId.isEmpty()) {
        if (m_queued.size() >= 64) m_queued.removeFirst();
        m_queued.append({js, done});
        return;
    }
    m_host->send("Runtime.evaluate", QJsonObject{{"expression", js}, {"returnByValue", true}, {"silent", true}}, m_sessionId, this,
                 [this, done](const QJsonObject& r, const QString& e) {
        if (!e.isEmpty()) { if (done) done(false, QJsonValue(e)); return; }
        const QJsonObject ex = r.value("exceptionDetails").toObject();
        if (!ex.isEmpty()) {
            QString msg = ex.value("exception").toObject().value("description").toString();
            if (msg.isEmpty()) msg = ex.value("text").toString();
            noteError("script error: " + msg.left(300), true);
            if (done) done(false, QJsonValue(msg));
            return;
        }
        if (done) done(true, r.value("result").toObject().value("value"));
    });
}

void HtmlPage::flushQueued() {
    if (m_queued.isEmpty()) return;
    const QList<Queued> q = m_queued;
    m_queued.clear();
    for (const Queued& i : q) doEvaluate(i.js, i.done);
}

QString HtmlPage::mergedUrl(const QJsonObject& fields) const {
    QUrl u(m_url);
    if (!u.isValid() || u.scheme() == "data" || u.scheme() == "about" || u.scheme() == "javascript") return QString();
    QUrlQuery q(u);
    for (auto it = fields.begin(); it != fields.end(); ++it) { q.removeAllQueryItems(it.key()); q.addQueryItem(it.key(), valueToString(it.value())); }
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

void HtmlPage::doSetFields(const QJsonObject& fields, bool reloadOnMiss) {
    if (m_closed || fields.isEmpty()) return;
    for (auto it = fields.begin(); it != fields.end(); ++it) m_fields[it.key()] = it.value();
    doEvaluate(fieldsScript(fields), [this, fields, reloadOnMiss](bool ok, const QJsonValue& v) {
        if (!ok || v.toInt(0) > 0) return;
        const QString merged = reloadOnMiss ? mergedUrl(fields) : QString();
        if (merged.isEmpty()) { noteError(QString("fields matched nothing in %1 (no [data-field], #name or window.update)").arg(m_url.left(120)), false); return; }
        qInfo().noquote() << QString("[Html] %1 fields matched no element: re-taking with the fields in the URL query").arg(m_label);
        doNavigate(merged, false);
        m_fields = QJsonObject();                              // now part of the URL
    });
}

void HtmlPage::doClose() {
    if (m_closed) return;
    m_closed = true;
    const QString tid = m_targetId;
    dropSession();
    closeTargetId(tid);
    setState(State::Closed);
    for (const Queued& q : m_queued) if (q.done) q.done(false, QJsonValue(QStringLiteral("page closed")));
    m_queued.clear(); m_pending.clear();
    if (m_host) m_host->unregisterPage(this);
    QMutexLocker l(&m_sh->mtx);
    if (m_sh->frames == 0) { m_sh->failed = true; if (m_sh->lastError.isEmpty()) m_sh->lastError = "page closed"; }
    m_sh->cond.wakeAll();
}

// ===========================================================================
// BrowserHost
// ===========================================================================

namespace {
QMutex g_hostMtx;
BrowserHost* g_host = nullptr;
QThread* g_hostThread = nullptr;
bool g_quitHooked = false;

bool processAlive(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    const DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
#else
    return QDir(QString("/proc/%1").arg(pid)).exists();
#endif
}
} // namespace

BrowserHost* BrowserHost::instance() {
    QMutexLocker l(&g_hostMtx);
    if (!g_host) {
        g_hostThread = new QThread();
        g_hostThread->setObjectName("PlayPlayBrowserHost");
        g_host = new BrowserHost();
        g_host->moveToThread(g_hostThread);
        QObject::connect(g_hostThread, &QThread::finished, g_host, &QObject::deleteLater);
        g_hostThread->start();
        BrowserHost* h = g_host;
        QMetaObject::invokeMethod(h, [h] { h->initOnHost(); }, Qt::QueuedConnection);
        if (!g_quitHooked && QCoreApplication::instance()) {
            g_quitHooked = true;
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, QCoreApplication::instance(), [] { BrowserHost::shutdown(); }, Qt::DirectConnection);
        }
    }
    return g_host;
}

void BrowserHost::prewarm() {
    BrowserHost* h = instance();
    QMetaObject::invokeMethod(h, [h] { h->ensureLaunched(); }, Qt::QueuedConnection);
}

void BrowserHost::shutdown() {
    BrowserHost* h; QThread* t;
    { QMutexLocker l(&g_hostMtx); h = g_host; t = g_hostThread; g_host = nullptr; g_hostThread = nullptr; }
    if (!h) return;
    h->invokeSync([h] { h->stopOnHost(); }, 5000);
    t->quit();
    t->wait(4000);
}

BrowserHost::BrowserHost(QObject* parent) : QObject(parent) {
    m_infoState = "idle";
    m_forcedNth = qEnvironmentVariableIntValue("PLAYPLAY_HTML_NTH_FRAME");
}

BrowserHost::~BrowserHost() {}

QString BrowserHost::stateName(State s) {
    switch (s) {
    case State::Idle: return "idle";
    case State::Launching: return "launching";
    case State::Connecting: return "connecting";
    case State::Ready: return "ready";
    case State::Backoff: return "restarting";
    case State::Missing: return "missing";
    case State::Stopping: return "stopping";
    }
    return "unknown";
}

void BrowserHost::setState(State s) {
    m_state = s;
    QMutexLocker l(&m_infoMtx);
    m_infoState = stateName(s);
}

void BrowserHost::setError(const QString& e) { QMutexLocker l(&m_infoMtx); m_infoError = e; }

void BrowserHost::setInfo(const QString& path, const QString& version, const QString& product) {
    QMutexLocker l(&m_infoMtx);
    m_infoPath = path; m_infoVersion = version; m_infoProduct = product;
}

void BrowserHost::setBrowserPath(const QString& preferred) {
    QMetaObject::invokeMethod(this, [this, preferred] { m_preferred = preferred; QMutexLocker l(&m_infoMtx); m_infoPreferred = preferred; }, Qt::QueuedConnection);
}

QString BrowserHost::browserPath() const { QMutexLocker l(&m_infoMtx); return m_infoPath; }
QString BrowserHost::browserVersion() const { QMutexLocker l(&m_infoMtx); return m_infoVersion; }
QString BrowserHost::lastError() const { QMutexLocker l(&m_infoMtx); return m_infoError; }

QJsonObject BrowserHost::status() const {
    QJsonObject o;
    QMutexLocker l(&m_infoMtx);
    o["state"] = m_infoState; o["path"] = m_infoPath; o["version"] = m_infoVersion; o["product"] = m_infoProduct;
    o["pid"] = double(m_infoPid); o["port"] = m_infoPort; o["profile"] = m_infoProfile; o["lastError"] = m_infoError;
    o["restarts"] = m_restarts.load(); o["pages"] = m_pageCount.load(); o["targets"] = m_targetCount.load();
    const qint64 r = m_infoReadyMs.load();
    if (r && m_infoState == "ready") o["uptimeMs"] = double(nowMs() - r);
    o["decodeThreads"] = decodePool()->maxThreadCount();       // PE-19
    return o;
}

bool BrowserHost::invokeSync(const std::function<void()>& fn, int timeoutMs) {
    if (QThread::currentThread() == thread()) { fn(); return true; }
    auto sem = std::make_shared<QSemaphore>();
    QMetaObject::invokeMethod(this, [fn, sem] { fn(); sem->release(); }, Qt::QueuedConnection);
    return sem->tryAcquire(1, timeoutMs);
}

void BrowserHost::initOnHost() {
    m_nam = new QNetworkAccessManager(this);
    m_nam->setProxy(QNetworkProxy::NoProxy);
    m_pollTimer = new QTimer(this); m_pollTimer->setInterval(150);
    connect(m_pollTimer, &QTimer::timeout, this, &BrowserHost::pollVersion);
    m_restartTimer = new QTimer(this); m_restartTimer->setSingleShot(true);
    connect(m_restartTimer, &QTimer::timeout, this, [this] { if (m_state == State::Backoff) launch(); });
    m_tickTimer = new QTimer(this); m_tickTimer->setInterval(500);
    connect(m_tickTimer, &QTimer::timeout, this, &BrowserHost::tick);
    m_tickTimer->start();
}

std::shared_ptr<HtmlPage> BrowserHost::createPage(const HtmlPageOptions& opt, QString* error) {
    struct R { std::shared_ptr<HtmlPage> page; QString err; };
    auto r = std::make_shared<R>();
    if (!invokeSync([this, opt, r] { r->page = createPageOnHost(opt, &r->err); }, 5000)) {
        if (error) *error = "browser host thread did not answer within 5 s";
        return nullptr;
    }
    if (!r->page && error) *error = r->err;
    return r->page;
}

QString BrowserHost::resolveExe(QString* why) {
    const QString exe = cgbrowser::find(m_preferred);
    if (exe.isEmpty() && why)
        *why = "no Chromium-family browser found (set settings cg.browserPath or PLAYPLAY_BROWSER); tried: " + cgbrowser::candidates(m_preferred).join("; ");
    return exe;
}

std::shared_ptr<HtmlPage> BrowserHost::createPageOnHost(const HtmlPageOptions& opt, QString* error) {
    QString why;
    const QString exe = resolveExe(&why);
    if (exe.isEmpty()) {
        if (error) *error = why;
        setError(why);
        if (m_state == State::Idle) setState(State::Missing);
        return nullptr;
    }
    if (m_state == State::Missing) setState(State::Idle);
    auto sh = std::make_shared<HtmlPageShared>();
    sh->url = opt.url; sh->label = opt.label; sh->premul = opt.premultiplied; sh->pngPremul = opt.pngPremultiplied;
    sh->skipSame = htmlingest::skipSameDefault();             // PE-19 (per page, so a test can compare both in one process)
    HtmlPage* raw = new HtmlPage(this, opt, sh);
    std::shared_ptr<HtmlPage> page(raw, [](HtmlPage* p) { QMetaObject::invokeMethod(p, "deleteLater", Qt::QueuedConnection); });
    registerPage(raw);
    ensureLaunched();
    if (m_state == State::Ready) raw->attach();
    return page;
}

void BrowserHost::registerPage(HtmlPage* p) { m_pages.append(p); m_pageCount++; }
void BrowserHost::unregisterPage(HtmlPage* p) { if (m_pages.removeOne(p)) m_pageCount--; }

void BrowserHost::ensureLaunched() { if (m_state == State::Idle) launch(); }

void BrowserHost::cleanStaleProfiles() {
    QDir root(QDir::tempPath() + "/playplay-html");
    const qint64 me = QCoreApplication::applicationPid();
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const qint64 pid = name.section('-', 0, 0).toLongLong(&ok);
        if (!ok || pid == me || processAlive(pid)) continue;
        QDir(root.filePath(name)).removeRecursively();
    }
}

void BrowserHost::removeProfile() {
    if (!m_profileDir.isEmpty()) QDir(m_profileDir).removeRecursively();
}

void BrowserHost::adopt(QProcess* p) {
#ifdef Q_OS_WIN
    if (!p || p->processId() == 0) return;
    if (!m_job) {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(h, JobObjectExtendedLimitInformation, &li, sizeof(li)); m_job = h; }
    }
    if (!m_job) return;
    HANDLE ph = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(p->processId()));
    if (ph) { if (!AssignProcessToJobObject(static_cast<HANDLE>(m_job), ph)) qWarning() << "[Html] could not put the browser into the kill-on-close job"; CloseHandle(ph); }
#else
    Q_UNUSED(p);
#endif
}

void BrowserHost::launch() {
    QString why;
    const QString exe = resolveExe(&why);
    if (exe.isEmpty()) { setError(why); setState(State::Missing); return; }
    m_exe = exe;
    if (!m_staleCleaned) { cleanStaleProfiles(); m_staleCleaned = true; }
    QTcpServer srv;
    if (!srv.listen(QHostAddress::LocalHost, 0)) { setError("no free local port: " + srv.errorString()); scheduleRestart(); return; }
    m_port = srv.serverPort();
    srv.close();
    m_profileDir = QDir::tempPath() + "/playplay-html/" + QString::number(QCoreApplication::applicationPid()) + "-" + QString::number(++m_profileSeq);
    QDir().mkpath(m_profileDir);
    QStringList args = {
        "--headless=new", "--remote-debugging-port=" + QString::number(m_port), "--remote-allow-origins=*",
        "--user-data-dir=" + QDir::toNativeSeparators(m_profileDir), "--window-size=1920,1080", "--hide-scrollbars", "--mute-audio",
        "--disable-extensions", "--disable-component-extensions-with-background-pages", "--no-first-run", "--no-default-browser-check",
        "--force-device-scale-factor=1", "--autoplay-policy=no-user-gesture-required", "--allow-file-access-from-files",
        "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
        "--disable-features=CalculateNativeWinOcclusion,Translate,MediaRouter", "--disable-component-update", "--disable-sync",
        "--disable-default-apps", "--metrics-recording-only", "--deny-permission-prompts", "--noerrdialogs", "--password-store=basic"};
    if (qEnvironmentVariableIntValue("PLAYPLAY_HTML_GPU") == 0) args << "--disable-gpu";
    const QString extra = qEnvironmentVariable("PLAYPLAY_HTML_EXTRA_ARGS").trimmed();
    if (!extra.isEmpty()) args << extra.split(' ', Qt::SkipEmptyParts);
    args << "about:blank";                                      // the anchor tab: keeps the browser alive when every overlay window is closed

    m_proc = new QProcess(this);
    m_proc->setStandardOutputFile(QProcess::nullDevice());
    m_proc->setStandardErrorFile(QProcess::nullDevice());
    QProcess* proc = m_proc;
    connect(proc, &QProcess::started, this, [this, proc] {
        if (proc != m_proc) return;
        adopt(proc);
        QMutexLocker l(&m_infoMtx); m_infoPid = qint64(proc->processId());
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (proc == m_proc && e == QProcess::FailedToStart) onBrowserLost("browser failed to start: " + proc->errorString());
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, proc](int code, QProcess::ExitStatus) {
        if (proc == m_proc) onBrowserLost(QString("browser exited (code %1)").arg(code));
    });
    m_launchMs = nowMs();
    m_pollBusy = false;
    setState(State::Launching);
    { QMutexLocker l(&m_infoMtx); m_infoPort = m_port; m_infoProfile = m_profileDir; m_infoPid = 0; }
    setInfo(exe, cgbrowser::version(exe), QString());
    qInfo().noquote() << QString("[Html] launching %1 (port %2)").arg(exe).arg(m_port);
    proc->start(exe, args);
    m_pollTimer->start();
}

void BrowserHost::pollVersion() {
    if (m_state != State::Launching) { m_pollTimer->stop(); return; }
    if (nowMs() - m_launchMs > 20000) { onBrowserLost("browser did not open its DevTools port within 20 s"); return; }
    if (m_pollBusy) return;
    m_pollBusy = true;
    QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/json/version").arg(m_port)));
    req.setTransferTimeout(1500);
    QNetworkReply* rep = m_nam->get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        m_pollBusy = false;
        rep->deleteLater();
        if (m_state != State::Launching || rep->error() != QNetworkReply::NoError) return;
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        const QString ws = o.value("webSocketDebuggerUrl").toString();
        if (ws.isEmpty()) return;
        m_pollTimer->stop();
        m_product = o.value("Browser").toString();
        setInfo(m_exe, cgbrowser::version(m_exe), m_product);
        openSocket(ws);
    });
}

void BrowserHost::openSocket(const QString& wsUrl) {
    setState(State::Connecting);
    m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    QWebSocket* ws = m_ws;
    connect(ws, &QWebSocket::connected, this, &BrowserHost::onSocketConnected);
    connect(ws, &QWebSocket::disconnected, this, [this, ws] { if (ws == m_ws && m_state != State::Stopping) onBrowserLost("DevTools websocket closed"); });
    connect(ws, &QWebSocket::textMessageReceived, this, &BrowserHost::onMessage);
    connect(ws, &QWebSocket::errorOccurred, this, [this, ws](QAbstractSocket::SocketError) { if (ws == m_ws && m_state == State::Connecting) onBrowserLost("DevTools websocket: " + ws->errorString()); });
    ws->open(QUrl(wsUrl));
}

void BrowserHost::onSocketConnected() {
    setState(State::Ready);
    m_readyMs = nowMs();
    m_infoReadyMs = m_readyMs;
    setError(QString());
    send("Browser.setDownloadBehavior", QJsonObject{{"behavior", "deny"}});
    send("Target.setDiscoverTargets", QJsonObject{{"discover", true}});
    qInfo().noquote() << QString("[Html] browser ready: %1 (%2 ms after launch)").arg(m_product).arg(m_readyMs - m_launchMs);
    const QList<HtmlPage*> pages = m_pages;
    for (HtmlPage* p : pages) p->attach();
}

void BrowserHost::onBrowserLost(const QString& why) {
    if (m_state == State::Stopping) return;
    qWarning().noquote() << "[Html] browser lost:" << why;
    setError(why);
    m_pollTimer->stop();
    if (m_ws) { QWebSocket* ws = m_ws; m_ws = nullptr; ws->disconnect(this); ws->abort(); ws->deleteLater(); }
    if (m_proc) {
        QProcess* p = m_proc; m_proc = nullptr; p->disconnect(this);
        if (p->state() != QProcess::NotRunning) p->kill();
        p->deleteLater();
    }
    m_sessions.clear();
    const QList<HtmlPage*> pages = m_pages;
    for (HtmlPage* pg : pages) pg->onBrowserGone();
    QHash<int, Pending> pend; pend.swap(m_pending);
    for (const Pending& p : pend) if (p.cb && (!p.hasOwner || !p.owner.isNull())) p.cb(QJsonObject(), "browser lost: " + why);
    m_targetCount = 0;
    removeProfile();
    ++m_restarts;
    scheduleRestart();
}

void BrowserHost::scheduleRestart() {
    if (m_pages.isEmpty()) { setState(State::Idle); return; }
    setState(State::Backoff);
    static const int delays[] = {1000, 2000, 4000, 8000, 15000};
    const int d = delays[qMin(m_failStreak, 4)];
    ++m_failStreak;
    m_restartTimer->start(d);
}

void BrowserHost::tick() {
    const qint64 now = nowMs();
    if ((m_state == State::Launching || m_state == State::Connecting) && now - m_launchMs > 25000) { onBrowserLost("browser start timed out"); return; }
    if (m_state == State::Ready && now - m_readyMs > 20000) m_failStreak = 0;
    QList<Pending> expired;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now - it->sentMs > 15000) { expired << it.value(); it = m_pending.erase(it); } else ++it;
    }
    for (const Pending& p : expired) if (p.cb && (!p.hasOwner || !p.owner.isNull())) p.cb(QJsonObject(), "timeout: " + p.method);
    const QList<HtmlPage*> pages = m_pages;
    for (HtmlPage* p : pages) p->slowTick(now);
    rebalance();
    // PE-19: PLAYPLAY_HTML_STATS=1 logs each page's ingest counters every 10 s (isolated-engine measurements; off by default)
    static const bool stats = qEnvironmentVariableIntValue("PLAYPLAY_HTML_STATS") != 0;
    if (stats && now - m_lastStatsMs >= 10000) {
        m_lastStatsMs = now;
        for (HtmlPage* p : pages) {
            const QJsonObject s = p->status();
            qInfo().noquote() << QString("[Html] ingest %1 in=%2 decoded=%3 skipped=%4 dropped=%5 decodeMs=%6 base64Ms=%7 fps=%8 threads=%9")
                                     .arg(p->m_label).arg(s.value("framesIn").toDouble()).arg(s.value("frames").toDouble())
                                     .arg(s.value("framesSkipped").toDouble()).arg(s.value("framesDropped").toDouble())
                                     .arg(s.value("decodeMs").toDouble()).arg(s.value("base64Ms").toDouble()).arg(s.value("fps").toDouble()).arg(m_decodeThreads);
        }
    }
}

void BrowserHost::rebalance() {
    int live = 0;
    for (HtmlPage* p : m_pages) if (p->isLive()) ++live;
    // PE-19: one decode slot per streaming page (each page has at most one decode in flight), so a third page no longer
    // waits in 4 ms pump retries behind two others. Never below today's 2 threads; the threads are low priority.
    const int threads = htmlingest::decodeThreadsFor(live);
    if (threads != m_decodeThreads) {
        decodePool()->setMaxThreadCount(threads);
        if (verbose()) qInfo().noquote() << QString("[Html] decode threads %1 -> %2 (%3 streaming pages)").arg(m_decodeThreads).arg(threads).arg(live);
        m_decodeThreads = threads;
    }
    if (m_forcedNth > 0) { m_autoNth = m_forcedNth; return; }
    // Always every frame: the screencast is damage-driven and everyNthFrame skips compositor frames, so with nth=2 a one-off
    // change (new text, the repaint kick) is a single frame and is dropped half the time. CPU is bounded by the decode throttle.
    m_autoNth = 1; Q_UNUSED(live);
    for (HtmlPage* p : m_pages) if (p->autoNth()) p->setNth(m_autoNth);
}

void BrowserHost::stopOnHost() {
    setState(State::Stopping);
    if (m_pollTimer) m_pollTimer->stop();
    if (m_restartTimer) m_restartTimer->stop();
    if (m_tickTimer) m_tickTimer->stop();
    const QList<HtmlPage*> pages = m_pages;
    for (HtmlPage* p : pages) p->onBrowserShutdown();
    m_pages.clear(); m_pageCount = 0; m_targetCount = 0;
    if (m_ws) { QWebSocket* ws = m_ws; m_ws = nullptr; ws->disconnect(this); ws->abort(); }
    if (m_proc) {
        QProcess* p = m_proc; m_proc = nullptr; p->disconnect(this);
        if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(2000); }
    }
    removeProfile();
}

int BrowserHost::send(const QString& method, const QJsonObject& params, const QString& sessionId, QObject* owner, Reply cb) {
    const int id = m_nextId++;
    if (!m_ws || !m_ws->isValid()) {
        if (cb) { QPointer<QObject> o(owner); const bool has = owner != nullptr; QTimer::singleShot(0, this, [cb, o, has] { if (!has || !o.isNull()) cb(QJsonObject(), "browser not connected"); }); }
        return id;
    }
    QJsonObject m; m["id"] = id; m["method"] = method;
    if (!params.isEmpty()) m["params"] = params;
    if (!sessionId.isEmpty()) m["sessionId"] = sessionId;
    m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(m).toJson(QJsonDocument::Compact)));
    if (cb || method != "Page.screencastFrameAck") {
        Pending p; p.owner = owner; p.hasOwner = owner != nullptr; p.cb = std::move(cb); p.sentMs = nowMs(); p.method = method;
        m_pending.insert(id, std::move(p));
    }
    return id;
}

void BrowserHost::onMessage(const QString& text) {
    // PE-19: a 1080p screencast message is megabytes of base64. The byte-level path lifts the picture out of the UTF-16
    // text in one pass and parses only the small rest, instead of text.toUtf8() + a JSON string + toString().toLatin1().
    QJsonObject o;
    QByteArray b64;
    if (text.size() >= 16384 && htmlingest::fastParseEnabled()) {
        QString rest;
        if (htmlingest::splitScreencastData(text, &b64, &rest)) {
            o = QJsonDocument::fromJson(rest.toUtf8()).object();
            const bool ok = o.value("method").toString() == QLatin1String("Page.screencastFrame") && !o.contains("id")
                            && !o.value("sessionId").toString().isEmpty() && o.value("params").toObject().value("data").toString().isEmpty();
            if (ok) {
                if (HtmlPage* p = m_sessions.value(o.value("sessionId").toString(), nullptr)) { if (!p->m_closed) p->onScreencastFrame(o.value("params").toObject(), std::move(b64)); }
                return;
            }
            b64.clear();                                       // not the shape expected: parse the whole message below
        }
    }
    o = QJsonDocument::fromJson(text.toUtf8()).object();
    if (o.contains("id")) {
        auto it = m_pending.find(o.value("id").toInt());
        if (it == m_pending.end()) return;
        Pending p = std::move(it.value());
        m_pending.erase(it);
        const QJsonObject err = o.value("error").toObject();
        const QString e = err.isEmpty() ? QString() : err.value("message").toString("error");
        if (!e.isEmpty() && !p.cb) qWarning().noquote() << QString("[Html] CDP %1 failed: %2").arg(p.method, e);
        if (p.cb && (!p.hasOwner || !p.owner.isNull())) p.cb(o.value("result").toObject(), e);
        return;
    }
    const QString method = o.value("method").toString();
    const QJsonObject params = o.value("params").toObject();
    const QString sid = o.value("sessionId").toString();
    if (!sid.isEmpty()) {
        HtmlPage* p = m_sessions.value(sid, nullptr);
        if (p) p->onEvent(method, params);
        return;
    }
    onBrowserEvent(method, params);
}

void BrowserHost::onBrowserEvent(const QString& method, const QJsonObject& params) {
    if (method == "Target.targetCreated") {
        const QJsonObject info = params.value("targetInfo").toObject();
        if (info.value("type").toString() == "page" && !info.value("openerId").toString().isEmpty()) {   // popup: never on air
            qInfo().noquote() << "[Html] closed popup" << info.value("url").toString().left(120);
            send("Target.closeTarget", QJsonObject{{"targetId", info.value("targetId").toString()}});
        }
    } else if (method == "Target.targetDestroyed") {
        const QString tid = params.value("targetId").toString();
        const QList<HtmlPage*> pages = m_pages;
        for (HtmlPage* p : pages) p->onTargetDestroyed(tid);
    } else if (method == "Target.targetCrashed") {
        const QString tid = params.value("targetId").toString();
        const QList<HtmlPage*> pages = m_pages;
        for (HtmlPage* p : pages) if (p->m_targetId == tid) p->onTargetLost("renderer crashed");
    } else if (method == "Target.detachedFromTarget") {
        if (HtmlPage* p = m_sessions.value(params.value("sessionId").toString(), nullptr)) p->onTargetLost("detached from target");
    }
}
