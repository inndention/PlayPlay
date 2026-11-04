#include "provider/openrouter.h"

#include "core/clock.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "provider/limits.h"
#include "provider/payload.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace studio::provider {

namespace {

// ECMAScript WhiteSpace + LineTerminator (String.prototype.trim)
bool jsSpace(char16_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000
        || c == 0xFEFF;
}
QStringView jsTrim(QStringView s) {
    qsizetype a = 0, b = s.size();
    while (a < b && jsSpace(s[a].unicode())) ++a;
    while (b > a && jsSpace(s[b - 1].unicode())) --b;
    return s.mid(a, b - a);
}
bool nullish(const QJsonValue& v) { return v.isUndefined() || v.isNull(); }

// JSON.parse: any value or nothing (json::parse refuses trailing text and wraps scalars, as JSON.parse does)
std::optional<QJsonValue> jsonParse(QStringView s) {
    const QJsonValue v = json::parse(s.toUtf8());
    if (v.isUndefined()) return std::nullopt;
    return v;
}

// String(v) for the scalars an answer carries where the TS expects text
QString textOf(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return json::jsNumber(v.toDouble());
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isNull()) return QStringLiteral("null");
    if (v.isUndefined()) return QStringLiteral("undefined");
    return QString::fromUtf8(json::compact(v));
}
// a JS truthy value (for `x ? ... : ...` on answer fields)
bool truthy(const QJsonValue& v) {
    if (v.isString()) return !v.toString().isEmpty();
    if (v.isDouble()) return v.toDouble() != 0 && !std::isnan(v.toDouble());
    if (v.isBool()) return v.toBool();
    return v.isObject() || v.isArray();
}

QJsonValue at(const QJsonValue& v, QLatin1StringView key) {
    return v.isObject() ? v.toObject().value(key) : QJsonValue(QJsonValue::Undefined);
}

std::optional<qint64> intOf(const QJsonValue& v) {
    if (!v.isDouble()) return std::nullopt;
    return qint64(v.toDouble());
}

} // namespace

QString stripFences(QStringView s) {
    // /^```(?:json)?\s*([\s\S]*?)```$/ on the trimmed text, then the capture trimmed. Written out by hand: data::regex
    // refuses \S inside a class (it has no exact PCRE2 form), and the expression is simple enough.
    const QStringView t = jsTrim(s);
    if (t.size() >= 6 && t.startsWith(u"```") && t.endsWith(u"```")) {
        QStringView inner = t.mid(3, t.size() - 6);
        if (inner.startsWith(u"json")) inner = inner.mid(4);   // greedy and always possible (the capture may be empty)
        return jsTrim(inner).toString();
    }
    return t.toString();
}

QJsonValue salvageJson(QStringView text) {
    const QString t = stripFences(text);
    if (auto v = jsonParse(t)) return *v;
    qsizetype start = -1;
    for (qsizetype i = 0; i < t.size(); ++i)
        if (t[i] == u'{' || t[i] == u'[') { start = i; break; }
    if (start < 0) return QJsonValue(QJsonValue::Undefined);
    const QStringView body = QStringView(t).mid(start);
    {
        const qsizetype last = body.lastIndexOf(body[0] == u'{' ? u'}' : u']');
        if (auto v = jsonParse(body.left(last + 1))) return *v;   // last = -1 -> "" (fails, as slice(0, 0) does)
    }
    // truncated: walk the text tracking strings and brackets, cut back to the last point where a value was complete,
    // close what is open
    QString stack, lastStack;
    bool inStr = false, esc = false;
    qsizetype lastOk = -1;
    for (qsizetype i = 0; i < body.size(); ++i) {
        const QChar c = body[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == u'\\') esc = true;
            else if (c == u'"') inStr = false;
            continue;
        }
        if (c == u'"') inStr = true;
        else if (c == u'{') stack += u'}';
        else if (c == u'[') stack += u']';
        else if (c == u'}' || c == u']') {
            if (!stack.isEmpty()) stack.chop(1);
            lastOk = i + 1;
            lastStack = stack;
        } else if (c == u',' && !stack.isEmpty() && stack.back() == u']') {
            // only between array items: a half-filled object is not repaired
            lastOk = i;
            lastStack = stack;
        }
    }
    if (lastOk <= 0) return QJsonValue(QJsonValue::Undefined);
    // .replace(/[,:\s]+$/, '')
    QStringView cut = body.left(lastOk);
    while (!cut.isEmpty() && (cut.back() == u',' || cut.back() == u':' || jsSpace(cut.back().unicode()))) cut.chop(1);
    QString closers;
    for (qsizetype i = lastStack.size() - 1; i >= 0; --i) closers += lastStack[i];
    for (QStringView tail : {QStringView(), QStringView(u"\"\""), QStringView(u"null")}) {
        if (auto v = jsonParse(cut.toString() + tail.toString() + closers)) return *v;
    }
    return QJsonValue(QJsonValue::Undefined);
}

QJsonObject chatBody(const ChatRequest& req, bool viaProvider) {
    QJsonObject body;
    if (req.messages) {
        body.insert(QStringLiteral("messages"), *req.messages);
    } else {
        QJsonArray msgs;
        if (!req.system.isEmpty()) msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), req.system}});
        msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), req.user}});
        body.insert(QStringLiteral("messages"), msgs);
    }
    body.insert(QStringLiteral("max_tokens"), req.maxTokens.value_or(4000));
    body.insert(QStringLiteral("usage"), QJsonObject{{QStringLiteral("include"), true}});
    if (req.models.size() > 1) body.insert(QStringLiteral("models"), QJsonArray::fromStringList(req.models));
    else if (!req.models.isEmpty()) body.insert(QStringLiteral("model"), req.models.first());
    if (req.temperature) body.insert(QStringLiteral("temperature"), *req.temperature);
    if (req.reasoning) body.insert(QStringLiteral("reasoning"), *req.reasoning);
    else if (!req.reasoningEffort.isEmpty())
        body.insert(QStringLiteral("reasoning"), req.reasoningEffort == u"none" ? QJsonObject{{QStringLiteral("enabled"), false}}
                                                                               : QJsonObject{{QStringLiteral("effort"), req.reasoningEffort}});
    if (req.jsonSchema) {
        body.insert(QStringLiteral("response_format"),
                    req.jsonMode == u"json_object"
                        ? QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}}
                        : QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                      {QStringLiteral("json_schema"), QJsonObject{{QStringLiteral("name"), req.jsonSchema->name},
                                                                                  {QStringLiteral("strict"), true},
                                                                                  {QStringLiteral("schema"), req.jsonSchema->schema}}}});
        body.insert(QStringLiteral("plugins"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("response-healing")}}});
    }
    QJsonArray tools = req.tools;
    if (req.webSearch) {
        QJsonObject p{{QStringLiteral("max_results"), req.webSearch->maxResults}};
        if (req.webSearch->maxTotal) p.insert(QStringLiteral("max_total_results"), req.webSearch->maxTotal);
        if (req.webSearch->maxUses) p.insert(QStringLiteral("max_uses"), req.webSearch->maxUses);
        if (!req.webSearch->contextSize.isEmpty()) p.insert(QStringLiteral("search_context_size"), req.webSearch->contextSize);
        tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("openrouter:web_search")}, {QStringLiteral("parameters"), p}});
    }
    if (!tools.isEmpty()) body.insert(QStringLiteral("tools"), tools);
    if (!req.toolChoice.isEmpty() && !req.tools.isEmpty()) body.insert(QStringLiteral("tool_choice"), req.toolChoice);
    if (viaProvider) {
        // a local / pool service speaks OpenAI-compatible chat: no OpenRouter routing prefs, plugins or fallback list
        body.remove(QStringLiteral("plugins"));
        body.remove(QStringLiteral("models"));
        if (!req.models.isEmpty()) body.insert(QStringLiteral("model"), req.models.first());
    } else {
        QJsonObject prefs{{QStringLiteral("require_parameters"), bool(req.jsonSchema)}};
        for (auto it = req.provider.begin(); it != req.provider.end(); ++it) prefs.insert(it.key(), it.value());
        body.insert(QStringLiteral("provider"), prefs);
    }
    return body;
}

ChatResult parseChat(const QJsonObject& data, const ChatRequest& req) {
    ChatResult r;
    const QJsonValue choice = data.value(QLatin1StringView("choices")).toArray().at(0);
    const QJsonValue msgV = at(choice, QLatin1StringView("message"));
    const QJsonObject msg = msgV.toObject();
    const QJsonValue content = msg.value(QLatin1StringView("content"));
    if (content.isString()) {
        r.text = content.toString();
    } else if (content.isArray()) {
        for (const QJsonValue& p : content.toArray()) {
            const QJsonValue t = at(p, QLatin1StringView("text"));
            if (!nullish(t)) r.text += textOf(t);
        }
    }
    for (const QJsonValue& a : msg.value(QLatin1StringView("annotations")).toArray()) {
        if (at(a, QLatin1StringView("type")) != QJsonValue(QStringLiteral("url_citation"))) continue;
        const QJsonValue uc = at(a, QLatin1StringView("url_citation"));
        QJsonValue url = at(uc, QLatin1StringView("url"));
        if (nullish(url)) url = at(a, QLatin1StringView("url"));
        QJsonValue title = at(uc, QLatin1StringView("title"));
        if (nullish(title)) title = at(a, QLatin1StringView("title"));
        if (!truthy(url)) continue;
        r.citations.push_back({textOf(url), nullish(title) ? QString() : textOf(title)});
    }
    if (req.jsonSchema) {
        if (auto v = jsonParse(stripFences(r.text))) {
            r.json = *v;
        } else {
            r.json = salvageJson(r.text);
            r.jsonSalvaged = !r.json.isUndefined();
        }
    }
    const QJsonValue model = data.value(QLatin1StringView("model"));
    r.model = nullish(model) ? req.models.value(0) : textOf(model);
    const QJsonValue usage = data.value(QLatin1StringView("usage"));
    if (const QJsonValue cost = at(usage, QLatin1StringView("cost")); cost.isDouble()) {
        r.costUsd = cost.toDouble();
        bool ok = false;
        const money::Micro m = money::fromUsd(cost.toDouble(), &ok);
        if (ok) r.costMicro = m;
    }
    r.promptTokens = intOf(at(usage, QLatin1StringView("prompt_tokens")));
    r.completionTokens = intOf(at(usage, QLatin1StringView("completion_tokens")));
    r.reasoningTokens = intOf(at(at(usage, QLatin1StringView("completion_tokens_details")), QLatin1StringView("reasoning_tokens")));
    if (const QJsonValue id = data.value(QLatin1StringView("id")); !nullish(id)) r.generationId = textOf(id);
    int i = 0;
    for (const QJsonValue& c : msg.value(QLatin1StringView("tool_calls")).toArray()) {
        const QJsonValue fn = at(c, QLatin1StringView("function"));
        const QJsonValue name = at(fn, QLatin1StringView("name"));
        if (!truthy(name)) continue;
        ToolCall tc;
        const QJsonValue id = at(c, QLatin1StringView("id"));
        tc.id = nullish(id) ? QStringLiteral("call_%1").arg(i) : textOf(id);
        tc.name = textOf(name);
        const QJsonValue args = at(fn, QLatin1StringView("arguments"));
        tc.arguments = args.isString() ? args.toString()
                                       : QString::fromUtf8(json::compact(nullish(args) ? QJsonValue(QJsonObject()) : args));
        r.toolCalls.push_back(tc);
        ++i;
    }
    if (const QJsonValue rs = msg.value(QLatin1StringView("reasoning")); rs.isString() && !jsTrim(rs.toString()).isEmpty())
        r.reasoning = rs.toString();
    if (const QJsonValue fr = at(choice, QLatin1StringView("finish_reason")); truthy(fr)) r.finishReason = textOf(fr);
    r.message = msg;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------

OpenRouter::OpenRouter(Wire& wire, QObject* parent) : QObject(parent), m_wire(wire), m_link(std::make_shared<Link>()) {
    m_link->self = this;
}

quint64 OpenRouter::json(const QString& path, const std::optional<QJsonObject>& body, QObject* context, JsonDone done, Failed failed,
                         qint64 timeoutMs, const AbortSignal& signal, const HeaderList& headers, int retries) {
    Request rq;
    rq.method = body ? "POST" : "GET";
    rq.path = path;
    if (body) {
        rq.body = QJsonDocument(*body).toJson(QJsonDocument::Compact);
        const QJsonValue model = body->value(QLatin1StringView("model"));
        if (model.isString()) rq.quotaKey = rq.model = model.toString();
    }
    rq.timeoutMs = timeoutMs;
    rq.retries = retries;
    rq.signal = signal;
    rq.headers = headers;
    const QString dpath = path.section(QLatin1Char('?'), 0, 0);
    return m_wire.request(std::move(rq), context,
                          [done = std::move(done), failed, dpath](const Response& r) {
                              const QJsonValue v = r.json();
                              if (v.isUndefined()) {
                                  WireError e;
                                  e.kind = WireError::Kind::Http;
                                  e.status = r.status;
                                  e.message = QStringLiteral("%1 the answer from %2 is not JSON").arg(r.status).arg(dpath);
                                  e.attempts = r.attempts;
                                  if (failed) failed(e);
                                  return;
                              }
                              if (done) done(v, r);
                          },
                          failed);
}

quint64 OpenRouter::chat(const ChatRequest& req, QObject* context, ChatDone done, Failed failed) {
    const bool viaProvider = !m_wire.providerBase(req.models.value(0)).isEmpty();
    const QJsonObject body = chatBody(req, viaProvider);
    Request rq;
    rq.method = "POST";
    rq.path = QStringLiteral("/chat/completions");
    rq.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    // TS json(): the quota key is the body's single model (none with a fallback list); the provider base and the
    // provider sub-gate follow the primary model
    if (const QJsonValue m = body.value(QLatin1StringView("model")); m.isString()) rq.quotaKey = m.toString();
    rq.model = req.models.value(0);
    rq.timeoutMs = req.timeoutMs;
    rq.retries = req.retries;
    rq.signal = req.signal;
    return m_wire.request(std::move(rq), context,
                          [req, done = std::move(done), failed](const Response& r) {
                              const QJsonValue v = r.json();
                              if (!v.isObject()) {
                                  WireError e;
                                  e.kind = WireError::Kind::Http;
                                  e.status = r.status;
                                  e.message = QStringLiteral("%1 the answer from /chat/completions is not a JSON object").arg(r.status);
                                  e.attempts = r.attempts;
                                  if (failed) failed(e);
                                  return;
                              }
                              ChatResult res = parseChat(v.toObject(), req);
                              res.attempts = r.attempts;
                              res.http2 = r.http2;
                              if (done) done(res);
                          },
                          failed);
}

// =============================================================================================================
// Part 2 (ST-07b): media, transcription, embeddings, rerank, cost lookups
// =============================================================================================================

namespace {

constexpr QStringView kCat = u"openrouter";

void costInto(const QJsonValue& cost, std::optional<double>& usd, std::optional<money::Micro>& micro) {
    if (!cost.isDouble() || !std::isfinite(cost.toDouble())) return;
    usd = cost.toDouble();
    bool ok = false;
    const money::Micro m = money::fromUsd(cost.toDouble(), &ok);
    if (ok) micro = m;
}

std::optional<money::Micro> microOf(const std::optional<double>& usd) {
    if (!usd) return std::nullopt;
    bool ok = false;
    const money::Micro m = money::fromUsd(*usd, &ok);
    return ok ? std::optional<money::Micro>(m) : std::nullopt;
}

// X-Est-Cost: String(estUsd), only when the TS sends it (images: > 0; video / speech: truthy)
HeaderList estCost(const std::optional<double>& est, bool positiveOnly) {
    if (!est || std::isnan(*est) || *est == 0 || (positiveOnly && *est < 0)) return {};
    return {{QByteArrayLiteral("X-Est-Cost"), json::jsNumber(*est).toLatin1()}};
}

// written atomically (QSaveFile): a reader never sees half a file; empty = written, else why not
QString writeFile(const QString& path, const QByteArray& data) {
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) return QStringLiteral("cannot create the folder %1").arg(QDir::toNativeSeparators(fi.absolutePath()));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return f.errorString();
    if (f.write(data) != data.size()) {
        const QString e = f.errorString();
        f.cancelWriting();
        return e;
    }
    if (!f.commit()) return f.errorString();
    return {};
}

// `work` on the pool, then `then` on owner's thread (never when owner is gone). The work must not touch owner.
template <class T>
void runOff(QObject* owner, QThreadPool* pool, std::function<T()> work, std::function<void(T)> then) {
    auto* w = new QFutureWatcher<T>(owner);
    QObject::connect(w, &QFutureWatcherBase::finished, owner, [w, then = std::move(then)]() {
        T v = w->result();
        w->deleteLater();
        then(std::move(v));
    });
    w->setFuture(QtConcurrent::run(pool, std::move(work)));
}

template <class F>
void postTo(QObject* target, F f) {
    if (target) QMetaObject::invokeMethod(target, std::move(f), Qt::QueuedConnection);
}

// TS String(x?.status ?? '').toLowerCase()
QString lowerStatus(const QJsonValue& v) { return nullish(v) ? QString() : textOf(v).toLower(); }

// TS errText: a string as is, else its message, else the JSON (300 chars)
QString errText(const QJsonValue& e) {
    if (e.isString()) return e.toString();
    if (const QJsonValue m = at(e, QLatin1StringView("message")); truthy(m)) return textOf(m);
    if (truthy(e)) return QString::fromUtf8(json::compact(e)).left(300);
    return {};
}

// TS videoCostOf: usage.cost ?? cost, a finite number
std::optional<double> videoCost(const QJsonObject& s) {
    QJsonValue c = at(s.value(QLatin1StringView("usage")), QLatin1StringView("cost"));
    if (nullish(c)) c = s.value(QLatin1StringView("cost"));
    if (c.isDouble() && std::isfinite(c.toDouble())) return c.toDouble();
    return std::nullopt;
}

struct ImageWork {
    bool json = false;
    ImageResult res;
};

// On the worker pool: parse the answer, decode each b64_json, write the files.
ImageWork decodeImages(const QByteArray& body, const QString& outDir, const QString& baseName) {
    ImageWork w;
    const QJsonValue v = json::parse(body, nullptr, qsizetype(1) << 30);
    if (v.isUndefined()) return w;
    w.json = true;
    costInto(at(at(v, QLatin1StringView("usage")), QLatin1StringView("cost")), w.res.costUsd, w.res.costMicro);
    QStringList errors;
    const QJsonArray data = at(v, QLatin1StringView("data")).toArray();
    for (qsizetype i = 0; i < data.size(); ++i) {
        const QJsonValue d = data.at(i);
        const QJsonValue mt = at(d, QLatin1StringView("media_type"));
        ImageFile f;
        f.mediaType = nullish(mt) ? QStringLiteral("image/png") : textOf(mt);
        const QJsonValue b64v = at(d, QLatin1StringView("b64_json"));
        if (!b64v.isString() || b64v.toString().isEmpty()) {
            errors << QStringLiteral("image %1: the answer has no b64_json").arg(i + 1);
            continue;
        }
        QByteArray b64 = b64v.toString().toLatin1();
        if (b64.startsWith("data:")) b64 = b64.mid(b64.indexOf(',') + 1);   // a data: URI instead of bare base64
        // strict: a corrupted payload is reported in writeError, not hashed and written as if it were an image
        const auto decoded = QByteArray::fromBase64Encoding(b64, QByteArray::AbortOnBase64DecodingErrors);
        const QByteArray bytes = decoded ? *decoded : QByteArray();
        if (bytes.isEmpty()) {
            errors << QStringLiteral("image %1: b64_json does not decode").arg(i + 1);
            continue;
        }
        f.size = bytes.size();
        f.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        if (outDir.isEmpty()) {
            f.data = bytes;
        } else {
            const QString path = QDir(outDir).filePath(QStringLiteral("%1-%2.%3").arg(baseName).arg(i + 1).arg(imageExtension(f.mediaType)));
            if (const QString err = writeFile(path, bytes); !err.isEmpty()) {
                errors << QStringLiteral("image %1: %2").arg(i + 1).arg(err);
                continue;
            }
            f.path = path;
        }
        w.res.images.push_back(f);
    }
    w.res.writeError = errors.join(QStringLiteral("; "));
    return w;
}

WireError notJson(int status, QStringView path, int attempts) {
    WireError e;
    e.kind = WireError::Kind::Http;
    e.status = status;
    e.message = QStringLiteral("%1 the answer from %2 is not JSON").arg(status).arg(path);
    e.attempts = attempts;
    return e;
}

} // namespace

QJsonObject imageBody(const ImageRequest& req) {
    QJsonObject body{{QStringLiteral("model"), req.model}, {QStringLiteral("prompt"), req.prompt}};
    if (!req.aspectRatio.isEmpty()) body.insert(QStringLiteral("aspect_ratio"), req.aspectRatio);
    if (!req.quality.isEmpty()) body.insert(QStringLiteral("quality"), req.quality);
    if (req.n > 1) body.insert(QStringLiteral("n"), req.n);
    if (!req.references.isEmpty()) {
        QJsonArray refs;
        for (const QString& url : req.references)
            refs.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")}, {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), url}}}});
        body.insert(QStringLiteral("input_references"), refs);
    }
    return body;
}

QString imageExtension(QStringView mediaType) {
    const QString t = mediaType.toString().section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (t == u"image/png") return QStringLiteral("png");
    if (t == u"image/jpeg" || t == u"image/jpg") return QStringLiteral("jpg");
    if (t == u"image/webp") return QStringLiteral("webp");
    if (t == u"image/gif") return QStringLiteral("gif");
    if (t == u"image/avif") return QStringLiteral("avif");
    return QStringLiteral("bin");
}

QJsonObject videoBody(const VideoRequest& req) {
    QJsonObject body{{QStringLiteral("model"), req.model}, {QStringLiteral("prompt"), req.prompt}};
    if (req.duration) body.insert(QStringLiteral("duration"), req.duration);
    if (!req.resolution.isEmpty()) body.insert(QStringLiteral("resolution"), req.resolution);
    if (!req.aspectRatio.isEmpty()) body.insert(QStringLiteral("aspect_ratio"), req.aspectRatio);
    if (req.generateAudio) body.insert(QStringLiteral("generate_audio"), *req.generateAudio);
    QJsonArray frames;
    const auto frame = [](const QString& url, const char* kind) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                           {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), url}}},
                           {QStringLiteral("frame_type"), QLatin1String(kind)}};
    };
    if (!req.firstFrameUrl.isEmpty()) frames.append(frame(req.firstFrameUrl, "first_frame"));
    if (!req.lastFrameUrl.isEmpty()) frames.append(frame(req.lastFrameUrl, "last_frame"));
    if (!frames.isEmpty()) body.insert(QStringLiteral("frame_images"), frames);
    if (!req.videoRefUrl.isEmpty())
        body.insert(QStringLiteral("input_references"),
                    QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("video_url")},
                                           {QStringLiteral("video_url"), QJsonObject{{QStringLiteral("url"), req.videoRefUrl}}}}});
    return body;
}

QString videoPollPath(const QJsonValue& u, const QString& base) {
    if (!u.isString()) return {};
    const QString t = jsTrim(u.toString()).toString();
    if (t.isEmpty()) return {};
    static const QRegularExpression absolute(QStringLiteral("^https?://"), QRegularExpression::CaseInsensitiveOption);
    if (absolute.match(t).hasMatch()) return t;
    QString p = t.startsWith(QLatin1Char('/')) ? t : QLatin1Char('/') + t;
    QString prefix;
    const QUrl b(base, QUrl::StrictMode);
    if (b.isValid() && !b.isRelative()) {
        prefix = b.path();
        if (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);   // .replace(/\/$/, '')
    }
    if (!prefix.isEmpty() && (p == prefix || p.startsWith(prefix + QLatin1Char('/')))) p = p.mid(prefix.size());
    else if (p.startsWith(QLatin1String("/api/v1/"))) p = p.mid(7);
    return p;
}

bool videoInProgress(QStringView s) {
    return s == u"pending" || s == u"queued" || s == u"in_progress" || s == u"processing" || s == u"running" || s == u"starting"
        || s == u"submitted";
}

QJsonObject speechBody(const SpeechRequest& req) {
    // JSON.stringify drops the undefined members: voice with a reference, speed when not given
    QJsonObject body{{QStringLiteral("model"), req.model}, {QStringLiteral("input"), req.input}, {QStringLiteral("response_format"), req.format}};
    if (!req.reference && !req.voice.isEmpty()) body.insert(QStringLiteral("voice"), req.voice);
    if (req.speed) body.insert(QStringLiteral("speed"), *req.speed);
    if (req.reference) {
        QJsonArray refs{QJsonObject{{QStringLiteral("type"), QStringLiteral("input_audio")},
                                    {QStringLiteral("input_audio"), QJsonObject{{QStringLiteral("data"), req.reference->audioDataUrl}}}}};
        if (!req.reference->transcript.isEmpty())
            refs.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), req.reference->transcript}});
        body.insert(QStringLiteral("input_references"), refs);
    }
    return body;
}

QJsonObject transcribeBody(const QString& model, const QByteArray& audioB64, const QString& format, const QString& responseFormat,
                           const QString& language, bool diarize, bool viaProvider) {
    QJsonObject body{{QStringLiteral("model"), model},
                     {QStringLiteral("input_audio"), QJsonObject{{QStringLiteral("data"), QString::fromLatin1(audioB64)}, {QStringLiteral("format"), format}}},
                     {QStringLiteral("response_format"), responseFormat}};
    if (!language.isEmpty()) body.insert(QStringLiteral("language"), language);
    // only the serving provider's entry is forwarded by OpenRouter; a local / pool provider gets none
    if (diarize && responseFormat == u"verbose_json" && !viaProvider)
        body.insert(QStringLiteral("provider"),
                    QJsonObject{{QStringLiteral("options"),
                                 QJsonObject{{QStringLiteral("azure"), QJsonObject{{QStringLiteral("diarization"), QJsonObject{{QStringLiteral("enabled"), true}}}}},
                                             {QStringLiteral("deepgram"), QJsonObject{{QStringLiteral("diarize"), true}}}}}});
    return body;
}

TranscribeResult parseTranscription(const QJsonObject& data, const QString& responseFormat) {
    TranscribeResult r;
    const QJsonValue text = data.value(QLatin1StringView("text"));
    r.text = nullish(text) ? QString() : textOf(text);
    for (const QJsonValue& s : data.value(QLatin1StringView("segments")).toArray()) {
        TranscriptSegment seg;
        seg.start = at(s, QLatin1StringView("start")).toDouble();
        seg.end = at(s, QLatin1StringView("end")).toDouble();
        const QJsonValue t = at(s, QLatin1StringView("text"));
        seg.text = jsTrim(nullish(t) ? QString() : textOf(t)).toString();
        const QJsonValue spk = at(s, QLatin1StringView("speaker"));
        if (spk.isDouble() || (spk.isString() && !spk.toString().isEmpty())) seg.speaker = spk;
        r.segments.push_back(seg);
    }
    if (const QJsonValue d = data.value(QLatin1StringView("duration")); d.isDouble()) r.duration = d.toDouble();
    std::optional<money::Micro> micro;
    costInto(at(data.value(QLatin1StringView("usage")), QLatin1StringView("cost")), r.costUsd, micro);
    r.costMicro = micro.value_or(0);
    r.responseFormat = responseFormat;
    r.pieces = 1;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------

struct OpenRouter::Op {
    enum class Phase { Idle, Wire, Accept, Sleep, Worker };
    quint64 id = 0;
    QPointer<QObject> ctx;
    bool hadCtx = false;
    bool over = false;
    Phase phase = Phase::Idle;
    AbortSignal signal;
    AbortSignal::ListenerId listener = 0;
    Clock::TimerId sleep = 0;
    // video
    VideoRequest video;
    VideoDone vdone;
    VideoFailed vfailed;
    QString base;                        // another provider's base (empty = OpenRouter)
    QString rawId;                       // the job's own id (may be empty)
    QString jobId;                       // rawId, else the poll path (TS jobId || pollUrl)
    QString pollPath;
    bool accepted = false;
    qint64 startedMs = 0;
    int polls = 0;
    std::optional<double> cost;
    // transcription
    QString model;
    TranscribeOptions topt;
    QString want;
    QList<payload::WavPiece> pieces;
    double byteRate = 32000;             // bytes per second of the WAV pieces (16 kHz mono 16-bit: 32,000)
    int next = 0;
    TranscribeResult out;
    TranscribeDone tdone;
    TranscribeFailed tfailed;

    bool alive() const { return !over && (!hadCtx || ctx); }
    QObject* target(QObject* self) const { return hadCtx ? ctx.data() : self; }
};

OpenRouter::~OpenRouter() {
    {
        std::lock_guard<std::mutex> g(m_link->m);
        m_link->self = nullptr;
    }
    for (const auto& op : std::as_const(m_ops)) {
        op->over = true;
        if (op->listener) op->signal.removeListener(op->listener);
        op->listener = 0;
    }
    m_ops.clear();   // pending clock timers have this as their context: they die with it
}

QThreadPool* OpenRouter::pool() const { return m_pool ? m_pool : QThreadPool::globalInstance(); }

std::shared_ptr<OpenRouter::Op> OpenRouter::newOp(QObject* context) {
    auto op = std::make_shared<Op>();
    op->id = ++m_nextOp;
    op->ctx = context;
    op->hadCtx = context != nullptr;
    m_ops.insert(op->id, op);
    return op;
}

void OpenRouter::endOp(const std::shared_ptr<Op>& op) {
    op->over = true;
    if (op->sleep) { m_wire.clock().cancel(op->sleep); op->sleep = 0; }
    if (op->listener) { op->signal.removeListener(op->listener); op->listener = 0; }
    m_ops.remove(op->id);
}

void OpenRouter::watchAbort(const std::shared_ptr<Op>& op) {
    if (!op->signal.valid()) return;
    std::shared_ptr<Link> link = m_link;
    const quint64 id = op->id;
    op->listener = op->signal.onAbort([link, id]() {
        std::lock_guard<std::mutex> g(link->m);
        if (!link->self) return;
        OpenRouter* self = link->self;
        QMetaObject::invokeMethod(self, [self, id]() { self->aborted(id); }, Qt::QueuedConnection);
    });
}

void OpenRouter::aborted(quint64 id) {
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->over) return;
    // a request on the wire is stopped by the Wire (its failure ends the call); a worker step finishes first
    if (op->phase != Op::Phase::Sleep && op->phase != Op::Phase::Accept) return;
    VideoError e;
    e.error.kind = WireError::Kind::Cancelled;
    e.error.message = op->signal.reason().isEmpty() ? QStringLiteral("Cancelled") : op->signal.reason();
    videoFail(op, e);
}

// ---------------- images ----------------

quint64 OpenRouter::generateImages(const ImageRequest& req, QObject* context, ImageDone done, Failed failed) {
    auto op = newOp(context);
    if (!req.outDir.isEmpty() && !ids::isSafe(req.baseName)) {
        WireError e;
        e.kind = WireError::Kind::Invalid;
        e.message = QStringLiteral("not a usable file name for the images: \"%1\" (letters, digits, '.', '_' and '-' only)").arg(req.baseName.left(80));
        e.sentNothing = true;
        postTo(op->target(this), [failed, e]() { if (failed) failed(e); });
        endOp(op);
        return op->id;
    }
    Request rq;
    rq.method = "POST";
    rq.path = QStringLiteral("/images");
    rq.body = QJsonDocument(imageBody(req)).toJson(QJsonDocument::Compact);
    rq.model = rq.quotaKey = req.model;
    rq.timeoutMs = req.timeoutMs;
    rq.signal = req.signal;
    rq.safety = req.safety;
    rq.headers = estCost(req.estUsd, true);
    op->phase = Op::Phase::Wire;
    const QString outDir = req.outDir, baseName = req.baseName;
    m_wire.request(std::move(rq), this,
                   [this, op, outDir, baseName, done, failed](const Response& r) {
                       // the images are paid for: they are decoded and written even when the caller is gone
                       op->phase = Op::Phase::Worker;
                       const QByteArray body = r.body;
                       const int status = r.status, attempts = r.attempts;
                       const bool h2 = r.http2;
                       runOff<ImageWork>(this, pool(), [body, outDir, baseName]() { return decodeImages(body, outDir, baseName); },
                                         [this, op, done, failed, status, attempts, h2](ImageWork w) {
                                             if (!w.json) {
                                                 const WireError e = notJson(status, u"/images", attempts);
                                                 postTo(op->target(this), [failed, e]() { if (failed) failed(e); });
                                             } else {
                                                 w.res.attempts = attempts;
                                                 w.res.http2 = h2;
                                                 if (!w.res.writeError.isEmpty()) log::warn(kCat, QStringLiteral("/images: %1").arg(w.res.writeError));
                                                 postTo(op->target(this), [done, res = w.res]() { if (done) done(res); });
                                             }
                                             endOp(op);
                                         });
                   },
                   [this, op, failed](const WireError& e) {
                       postTo(op->target(this), [failed, e]() { if (failed) failed(e); });
                       endOp(op);
                   });
    return op->id;
}

// ---------------- video ----------------

quint64 OpenRouter::generateVideo(const VideoRequest& req, QObject* context, VideoDone done, VideoFailed failed) {
    auto op = newOp(context);
    op->video = req;
    op->vdone = std::move(done);
    op->vfailed = std::move(failed);
    op->signal = req.signal;
    op->base = m_wire.providerBase(req.model);
    Request rq;
    rq.method = "POST";
    rq.path = QStringLiteral("/videos");
    rq.body = QJsonDocument(videoBody(req)).toJson(QJsonDocument::Compact);
    rq.model = rq.quotaKey = req.model;
    rq.timeoutMs = req.submitTimeoutMs;
    rq.signal = req.signal;
    rq.safety = req.safety;
    rq.headers = estCost(req.estUsd, false);
    op->phase = Op::Phase::Wire;
    watchAbort(op);
    m_wire.request(std::move(rq), this, [this, op](const Response& r) { videoSubmitted(op, r); },
                   [this, op](const WireError& e) {
                       VideoError v;
                       v.error = e;   // e.outcomeUnknown: the job may exist although no answer said so
                       videoFail(op, v);
                   });
    return op->id;
}

quint64 OpenRouter::resumeVideo(const VideoRequest& req, const QString& jobId, const QString& pollPath, QObject* context, VideoDone done,
                                VideoFailed failed) {
    auto op = newOp(context);
    op->video = req;
    op->video.onAccepted = nullptr;
    op->vdone = std::move(done);
    op->vfailed = std::move(failed);
    op->signal = req.signal;
    op->base = m_wire.providerBase(req.model);
    op->accepted = true;
    op->rawId = jobId;
    const QString apiBase = op->base.isEmpty() ? m_wire.baseUrl() : op->base;
    op->pollPath = !pollPath.isEmpty() ? videoPollPath(pollPath, apiBase) : jobId.isEmpty() ? QString() : QStringLiteral("/videos/") + jobId;
    op->jobId = jobId.isEmpty() ? op->pollPath : jobId;
    if (op->pollPath.isEmpty()) {
        VideoError e;
        e.error.kind = WireError::Kind::Invalid;
        e.error.message = QStringLiteral("a video job to resume needs its id or its polling URL");
        e.error.sentNothing = true;
        e.jobStatus = QStringLiteral("no_job_id");
        videoFail(op, e);
        return op->id;
    }
    op->startedMs = m_wire.clock().steadyMs();
    watchAbort(op);
    videoFetch(op, 0);
    return op->id;
}

void OpenRouter::videoSubmitted(const std::shared_ptr<Op>& op, const Response& r) {
    if (op->over) return;
    const QJsonValue v = r.json();
    const QJsonObject job = v.toObject();
    op->accepted = true;   // a 2xx to the submit: from here on money may be owed
    const QJsonValue idv = job.value(QLatin1StringView("id"));
    op->rawId = nullish(idv) ? QString() : textOf(idv);
    const QString apiBase = op->base.isEmpty() ? m_wire.baseUrl() : op->base;
    QString poll = videoPollPath(job.value(QLatin1StringView("polling_url")), apiBase);
    if (poll.isEmpty() && !op->rawId.isEmpty()) poll = QStringLiteral("/videos/") + op->rawId;
    if (!v.isObject() || poll.isEmpty()) {
        VideoError e;
        e.error = v.isObject() ? WireError{} : notJson(r.status, u"/videos", r.attempts);
        if (v.isObject()) {
            e.error.kind = WireError::Kind::Http;
            e.error.status = r.status;
            e.error.attempts = r.attempts;
            e.error.message = QStringLiteral("%1 the provider accepted the video job but its answer has no job id or polling URL").arg(r.status);
        }
        e.jobStatus = QStringLiteral("no_job_id");
        videoFail(op, e);
        return;
    }
    op->pollPath = poll;
    op->jobId = op->rawId.isEmpty() ? poll : op->rawId;
    op->startedMs = m_wire.clock().steadyMs();
    log::info(kCat, QStringLiteral("video job %1 accepted (%2)").arg(op->jobId, op->video.model));
    if (!op->alive()) {
        log::warn(kCat, QStringLiteral("video job %1 was accepted but its caller is gone: not polled, reconcile it").arg(op->jobId));
        endOp(op);
        return;
    }
    if (!op->video.onAccepted) {
        videoPoll(op, job);
        return;
    }
    // the caller records the job (commit before poll); the first poll waits for its proceed(true)
    op->phase = Op::Phase::Accept;
    auto once = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<Link> link = m_link;
    const quint64 id = op->id;
    std::function<void(bool)> proceed = [link, id, once, job](bool ok) {
        if (once->exchange(true)) return;
        std::lock_guard<std::mutex> g(link->m);
        if (!link->self) return;
        OpenRouter* self = link->self;
        QMetaObject::invokeMethod(self, [self, id, ok, job]() { self->accepted(id, ok, job); }, Qt::QueuedConnection);
    };
    postTo(op->target(this), [cb = op->video.onAccepted, jobId = op->jobId, pollPath = op->pollPath, proceed]() { cb(jobId, pollPath, proceed); });
}

void OpenRouter::accepted(quint64 id, bool ok, const QJsonObject& job) {
    const std::shared_ptr<Op> op = m_ops.value(id);
    if (!op || op->over || op->phase != Op::Phase::Accept) return;
    if (!ok) {
        VideoError e;
        e.error.kind = WireError::Kind::Invalid;
        e.error.message = QStringLiteral("video job %1 was accepted but could not be recorded: it was not polled (reconcile it before trying again)").arg(op->jobId);
        e.jobStatus = QStringLiteral("unrecorded");
        videoFail(op, e);
        return;
    }
    videoPoll(op, job);
}

void OpenRouter::videoPoll(const std::shared_ptr<Op>& op, const QJsonObject& status) {
    if (op->over) return;
    if (!op->alive()) {
        log::warn(kCat, QStringLiteral("video job %1: its caller is gone, polling stopped (reconcile it)").arg(op->jobId));
        endOp(op);
        return;
    }
    const QString s = lowerStatus(status.value(QLatin1StringView("status")));
    if (s == u"completed") {
        videoDownload(op, status);
        return;
    }
    // failed, cancelled, expired ... (and any status this client does not know): final, never polled for 20 minutes
    if (!s.isEmpty() && !videoInProgress(s)) {
        VideoError e;
        const QString why = errText(status.value(QLatin1StringView("error")));
        e.error.kind = WireError::Kind::Http;
        e.error.message = QStringLiteral("video generation %1%2").arg(s, why.isEmpty() ? QString() : QStringLiteral(": ") + why);
        e.jobStatus = s;
        e.costUsd = videoCost(status);
        videoFail(op, e);
        return;
    }
    if (m_wire.clock().steadyMs() - op->startedMs > op->video.maxWaitMs) {
        VideoError e;
        e.error.kind = WireError::Kind::Timeout;
        e.error.message = QStringLiteral("video generation timed out");
        e.jobStatus = QStringLiteral("timeout");
        videoFail(op, e);
        return;
    }
    videoFetch(op, s == u"pending" ? op->video.pollPendingMs : op->video.pollMs);
}

void OpenRouter::videoFetch(const std::shared_ptr<Op>& op, qint64 delayMs) {
    op->phase = Op::Phase::Sleep;
    op->sleep = m_wire.clock().callAfter(delayMs, this, [this, op]() {
        op->sleep = 0;
        if (op->over) return;
        if (!op->alive()) {
            log::warn(kCat, QStringLiteral("video job %1: its caller is gone, polling stopped (reconcile it)").arg(op->jobId));
            endOp(op);
            return;
        }
        Request rq;
        rq.method = "GET";
        rq.path = op->pollPath;
        rq.model = op->video.model;   // a provider's job is polled at that provider
        rq.timeoutMs = op->video.pollTimeoutMs;
        rq.signal = op->signal;
        op->phase = Op::Phase::Wire;
        m_wire.request(std::move(rq), this,
                       [this, op](const Response& r) {
                           if (op->over) return;
                           const QJsonValue v = r.json();
                           if (v.isUndefined()) {
                               VideoError e;
                               e.error = notJson(r.status, u"the video poll", r.attempts);
                               videoFail(op, e);
                               return;
                           }
                           ++op->polls;
                           const QJsonObject st = v.toObject();
                           if (op->video.onStatus) {
                               const QJsonValue sv = st.value(QLatin1StringView("status"));
                               postTo(op->target(this), [cb = op->video.onStatus, s = nullish(sv) ? QString() : textOf(sv)]() { cb(s); });
                           }
                           videoPoll(op, st);
                       },
                       [this, op](const WireError& e) {
                           VideoError v;
                           v.error = e;
                           videoFail(op, v);
                       });
    });
}

void OpenRouter::videoDownload(const std::shared_ptr<Op>& op, const QJsonObject& status) {
    // the clip is made (and billed): report the cost before the download, which can still fail
    op->cost = videoCost(status);
    if (op->video.onCompleted) postTo(op->target(this), [cb = op->video.onCompleted, c = op->cost]() { cb(c); });
    const QString apiBase = op->base.isEmpty() ? m_wire.baseUrl() : op->base;
    // the signed CDN URL when given (the key is not sent to another origin), else the authenticated content endpoint
    QString url = videoPollPath(status.value(QLatin1StringView("unsigned_urls")).toArray().at(0), apiBase);
    if (url.isEmpty()) url = op->rawId.isEmpty() ? op->pollPath + QStringLiteral("/content") : QStringLiteral("/videos/%1/content").arg(op->rawId);
    Request rq;
    rq.method = "GET";
    rq.path = url;
    rq.model = op->video.model;
    rq.timeoutMs = op->video.contentTimeoutMs;
    rq.maxResponseBytes = op->video.maxBytes;
    rq.signal = op->signal;
    op->phase = Op::Phase::Wire;
    m_wire.request(std::move(rq), this,
                   [this, op](const Response& r) {
                       if (op->over) return;
                       VideoResult res;
                       res.bytes = r.body;
                       res.size = r.body.size();
                       res.jobId = op->jobId;
                       res.costUsd = op->cost;
                       res.costMicro = microOf(op->cost);
                       res.polls = op->polls;
                       log::info(kCat, QStringLiteral("video job %1 downloaded (%2 bytes, %3 polls)").arg(op->jobId).arg(res.size).arg(op->polls));
                       const QString outPath = op->video.outPath;
                       if (outPath.isEmpty()) {
                           postTo(op->target(this), [cb = op->vdone, res]() { if (cb) cb(res); });
                           endOp(op);
                           return;
                       }
                       op->phase = Op::Phase::Worker;
                       const QByteArray bytes = r.body;
                       runOff<QString>(this, pool(), [outPath, bytes]() { return writeFile(outPath, bytes); }, [this, op, res, outPath](QString err) mutable {
                           if (err.isEmpty()) {
                               res.path = outPath;
                               res.bytes.clear();
                           } else {
                               res.writeError = err;
                               log::warn(kCat, QStringLiteral("video job %1: the clip could not be written: %2").arg(op->jobId, err));
                           }
                           postTo(op->target(this), [cb = op->vdone, res]() { if (cb) cb(res); });
                           endOp(op);
                       });
                   },
                   [this, op](const WireError& e) {
                       VideoError v;
                       v.error = e;
                       videoFail(op, v);
                   });
}

void OpenRouter::videoFail(const std::shared_ptr<Op>& op, VideoError e) {
    if (op->over) return;
    e.jobId = op->jobId;
    e.accepted = op->accepted;
    if (!e.costUsd) e.costUsd = op->cost;
    e.costMicro = microOf(e.costUsd);
    if (e.accepted)
        log::warn(kCat, QStringLiteral("video job %1 ended without a clip (%2): reconcile it, do not submit again").arg(op->jobId, e.error.message.left(200)));
    postTo(op->target(this), [cb = op->vfailed, e]() { if (cb) cb(e); });
    endOp(op);
}

// ---------------- speech ----------------

quint64 OpenRouter::speak(const SpeechRequest& req, QObject* context, SpeechDone done, Failed failed) {
    auto op = newOp(context);
    if (req.reference && req.reference->audioDataUrl.size() > kMaxVoiceSample) {
        WireError e;
        e.kind = WireError::Kind::TooLarge;
        e.status = 413;
        e.message = QStringLiteral("413 The voice sample is too large (%1 encoded): voice cloning takes at most 20 MB. Record a shorter sample "
                                   "(10–30 s is ideal).")
                        .arg(payload::fmtMB(req.reference->audioDataUrl.size()));
        e.sentNothing = true;
        postTo(op->target(this), [failed, e]() { if (failed) failed(e); });
        endOp(op);
        return op->id;
    }
    Request rq;
    rq.method = "POST";
    rq.path = QStringLiteral("/audio/speech");
    rq.body = QJsonDocument(speechBody(req)).toJson(QJsonDocument::Compact);
    rq.model = rq.quotaKey = req.model;
    rq.timeoutMs = req.timeoutMs;
    rq.signal = req.signal;
    rq.safety = req.safety;
    rq.headers = estCost(req.estUsd, false);
    op->phase = Op::Phase::Wire;
    const QString outPath = req.outPath, format = req.format;
    m_wire.request(std::move(rq), this,
                   [this, op, outPath, format, done](const Response& r) {
                       SpeechResult s;
                       s.bytes = r.body;
                       s.size = r.body.size();
                       s.format = format;
                       s.generationId = QString::fromLatin1(r.header("x-generation-id"));
                       s.attempts = r.attempts;
                       if (outPath.isEmpty()) {
                           postTo(op->target(this), [done, s]() { if (done) done(s); });
                           endOp(op);
                           return;
                       }
                       op->phase = Op::Phase::Worker;
                       const QByteArray bytes = r.body;
                       runOff<QString>(this, pool(), [outPath, bytes]() { return writeFile(outPath, bytes); }, [this, op, s, outPath, done](QString err) mutable {
                           if (err.isEmpty()) {
                               s.path = outPath;
                               s.bytes.clear();
                           } else {
                               s.writeError = err;
                               log::warn(kCat, QStringLiteral("/audio/speech: the audio could not be written: %1").arg(err));
                           }
                           postTo(op->target(this), [done, s]() { if (done) done(s); });
                           endOp(op);
                       });
                   },
                   [this, op, failed](const WireError& e) {
                       postTo(op->target(this), [failed, e]() { if (failed) failed(e); });
                       endOp(op);
                   });
    return op->id;
}

// ---------------- transcription ----------------

quint64 OpenRouter::transcribe(const QString& model, const QByteArray& audio, const QString& format, const TranscribeOptions& opts, QObject* context,
                               TranscribeDone done, TranscribeFailed failed) {
    auto op = newOp(context);
    op->model = model;
    op->topt = opts;
    op->signal = opts.signal;
    op->tdone = std::move(done);
    op->tfailed = std::move(failed);
    const QString want = !opts.responseFormat.isEmpty() ? opts.responseFormat
        : limits::rejectsVerboseJson(model)             ? QStringLiteral("json")
                                                        : QStringLiteral("verbose_json");
    sttSend(op, audio, format, want, [this, op](const TranscribeResult& r) {
        postTo(op->target(this), [cb = op->tdone, r]() { if (cb) cb(r); });
        endOp(op);
    });
    return op->id;
}

quint64 OpenRouter::transcribeLong(const QString& model, const QByteArray& audio, const QString& format, const TranscribeOptions& opts,
                                   QObject* context, TranscribeDone done, TranscribeFailed failed) {
    auto op = newOp(context);
    op->model = model;
    op->topt = opts;
    op->signal = opts.signal;
    op->tdone = std::move(done);
    op->tfailed = std::move(failed);
    const limits::SttLimits sl = limits::sttFor(model);
    const qint64 maxSingle = opts.maxSingleBytes > 0 ? opts.maxSingleBytes : sl.maxSingleBytes;
    const int pieceSeconds = opts.pieceSeconds > 0 ? opts.pieceSeconds : sl.pieceSeconds;
    const QString want = !opts.responseFormat.isEmpty() ? opts.responseFormat
        : limits::rejectsVerboseJson(model)             ? QStringLiteral("json")
                                                        : QStringLiteral("verbose_json");
    const auto single = [this, op](const TranscribeResult& r) {
        postTo(op->target(this), [cb = op->tdone, r]() { if (cb) cb(r); });
        endOp(op);
    };
    if (format != u"wav") {
        if (audio.size() > maxSingle) {
            WireError e;
            e.kind = WireError::Kind::TooLarge;
            e.status = 413;
            e.message = QStringLiteral("413 This %1 file is %2; this transcription model takes at most %3 per piece. It needs converting to WAV (the "
                                       "studio does this in the browser) or trimming.")
                            .arg(format, payload::fmtMB(audio.size()), payload::fmtMB(maxSingle));
            e.sentNothing = true;
            sttFail(op, e);
            return op->id;
        }
        sttSend(op, audio, format, want, single);
        return op->id;
    }
    // splitting copies the whole file: on the worker pool
    op->phase = Op::Phase::Worker;
    runOff<QList<payload::WavPiece>>(this, pool(), [audio, pieceSeconds]() { return payload::splitWav(audio, pieceSeconds); },
                                     [this, op, audio, want, single](QList<payload::WavPiece> pieces) {
                                         if (op->over) return;
                                         if (!op->alive()) { endOp(op); return; }
                                         if (pieces.size() <= 1) {
                                             sttSend(op, audio, QStringLiteral("wav"), want, single);
                                             return;
                                         }
                                         op->pieces = std::move(pieces);
                                         op->want = want;
                                         op->out.costUsd = 0;
                                         op->out.duration = 0;
                                         if (const auto wi = payload::wavInfo(audio); wi && wi->sampleRate > 0 && wi->channels > 0 && wi->bitsPerSample > 0)
                                             op->byteRate = double(wi->sampleRate) * wi->channels * (wi->bitsPerSample / 8.0);
                                         sttPiece(op);
                                     });
    return op->id;
}

void OpenRouter::sttPiece(const std::shared_ptr<Op>& op) {
    if (op->over) return;
    if (!op->alive()) { endOp(op); return; }
    const int n = int(op->pieces.size());
    if (op->next >= n) {
        postTo(op->target(this), [cb = op->tdone, r = op->out]() { if (cb) cb(r); });
        endOp(op);
        return;
    }
    const int i = op->next;
    if (op->topt.onPiece) postTo(op->target(this), [cb = op->topt.onPiece, i, n]() { cb(i + 1, n); });
    if (op->signal.aborted()) {
        WireError e;
        e.kind = WireError::Kind::Cancelled;
        e.message = op->signal.reason().isEmpty() ? QStringLiteral("Cancelled") : op->signal.reason();
        e.sentNothing = true;   // nothing of this piece; the pieces before it are in `partial`
        sttFail(op, e);
        return;
    }
    const payload::WavPiece p = op->pieces.at(i);
    sttSend(op, p.bytes, QStringLiteral("wav"), op->want, [this, op, p](const TranscribeResult& r) {
        // once a piece fell back to json, ask for json straight away for the rest
        if (!r.responseFormat.isEmpty()) {
            op->out.responseFormat = r.responseFormat;
            op->want = r.responseFormat;
        }
        const double pieceLen = double(p.bytes.size() - 44) / op->byteRate;
        QList<TranscriptSegment> segs = r.segments;
        if (segs.isEmpty() && !r.text.isEmpty()) segs.push_back({0, r.duration.value_or(pieceLen), r.text, QJsonValue(QJsonValue::Undefined)});
        for (TranscriptSegment s : segs) {
            s.start += p.offset;
            s.end += p.offset;
            op->out.segments.push_back(s);
        }
        if (!r.text.isEmpty()) op->out.text = op->out.text.isEmpty() ? r.text : op->out.text + QLatin1Char(' ') + r.text;
        op->out.duration = p.offset + r.duration.value_or(pieceLen);
        op->out.costUsd = op->out.costUsd.value_or(0) + r.costUsd.value_or(0);
        op->out.costMicro = money::add(op->out.costMicro, r.costMicro);
        ++op->out.pieces;
        ++op->next;
        sttPiece(op);
    });
}

void OpenRouter::sttSend(const std::shared_ptr<Op>& op, const QByteArray& audio, const QString& format, const QString& want,
                         std::function<void(const TranscribeResult&)> then) {
    if (op->over) return;
    if (!op->alive()) { endOp(op); return; }
    op->phase = Op::Phase::Worker;
    const bool viaProvider = !m_wire.providerBase(op->model).isEmpty();
    const QString model = op->model, language = op->topt.language;
    const bool diarize = op->topt.diarize;
    // base64 of a 10-25 MB piece and its JSON body: on the worker pool
    runOff<QByteArray>(
        this, pool(),
        [audio, model, format, want, language, diarize, viaProvider]() {
            return QJsonDocument(transcribeBody(model, audio.toBase64(), format, want, language, diarize, viaProvider)).toJson(QJsonDocument::Compact);
        },
        [this, op, audio, format, want, then](QByteArray body) {
            if (op->over) return;
            if (!op->alive()) { endOp(op); return; }
            Request rq;
            rq.method = "POST";
            rq.path = QStringLiteral("/audio/transcriptions");
            rq.body = std::move(body);
            rq.model = rq.quotaKey = op->model;
            rq.timeoutMs = op->topt.timeoutMs;
            rq.signal = op->signal;
            op->phase = Op::Phase::Wire;
            m_wire.request(std::move(rq), this,
                           [this, op, want, then](const Response& r) {
                               if (op->over) return;
                               const QJsonValue v = r.json();
                               if (v.isUndefined()) {
                                   sttFail(op, notJson(r.status, u"/audio/transcriptions", r.attempts));
                                   return;
                               }
                               then(parseTranscription(v.toObject(), want));
                           },
                           [this, op, audio, format, want, then](const WireError& e) {
                               if (op->over) return;
                               // models without structured output reject verbose_json with a 400: ask once more for plain
                               // json (a 400 is not billed)
                               if (want == u"verbose_json" && e.kind == WireError::Kind::Http && e.status == 400) {
                                   log::warn(kCat, QStringLiteral("%1 rejected verbose_json; retrying with json").arg(op->model));
                                   sttSend(op, audio, format, QStringLiteral("json"), then);
                                   return;
                               }
                               sttFail(op, e);
                           });
        });
}

void OpenRouter::sttFail(const std::shared_ptr<Op>& op, const WireError& e) {
    if (op->over) return;
    postTo(op->target(this), [cb = op->tfailed, e, partial = op->out]() { if (cb) cb(e, partial); });
    endOp(op);
}

// ---------------- embeddings / rerank / misc ----------------

quint64 OpenRouter::embed(const QString& model, const QStringList& input, QObject* context, EmbedDone done, Failed failed, const AbortSignal& signal) {
    const QJsonObject body{{QStringLiteral("model"), model}, {QStringLiteral("input"), QJsonArray::fromStringList(input)},
                           {QStringLiteral("encoding_format"), QStringLiteral("float")}};
    return json(QStringLiteral("/embeddings"), body, context,
                [done = std::move(done)](const QJsonValue& data, const Response&) {
                    EmbedResult res;
                    const QJsonArray arr = at(data, QLatin1StringView("data")).toArray();
                    QList<QJsonValue> items(arr.begin(), arr.end());
                    std::stable_sort(items.begin(), items.end(), [](const QJsonValue& a, const QJsonValue& b) {
                        return at(a, QLatin1StringView("index")).toDouble() < at(b, QLatin1StringView("index")).toDouble();
                    });
                    for (const QJsonValue& d : items) {
                        QVector<double> vec;
                        const QJsonArray e = at(d, QLatin1StringView("embedding")).toArray();
                        vec.reserve(e.size());
                        for (const QJsonValue& x : e) vec.push_back(x.toDouble());
                        res.vectors.push_back(vec);
                    }
                    costInto(at(at(data, QLatin1StringView("usage")), QLatin1StringView("cost")), res.costUsd, res.costMicro);
                    if (done) done(res);
                },
                std::move(failed), 180000, signal);
}

quint64 OpenRouter::rerank(const QString& model, const QString& query, const QStringList& documents, int topN, QObject* context, RerankDone done,
                           Failed failed, const AbortSignal& signal) {
    const QJsonObject body{{QStringLiteral("model"), model}, {QStringLiteral("query"), query},
                           {QStringLiteral("documents"), QJsonArray::fromStringList(documents)}, {QStringLiteral("top_n"), topN}};
    return json(QStringLiteral("/rerank"), body, context,
                [done = std::move(done)](const QJsonValue& data, const Response&) {
                    RerankResult res;
                    for (const QJsonValue& r : at(data, QLatin1StringView("results")).toArray())
                        res.results.push_back({int(at(r, QLatin1StringView("index")).toDouble()), at(r, QLatin1StringView("relevance_score")).toDouble()});
                    costInto(at(at(data, QLatin1StringView("usage")), QLatin1StringView("cost")), res.costUsd, res.costMicro);
                    if (done) done(res);
                },
                std::move(failed), 180000, signal);
}

quint64 OpenRouter::generationCost(const QString& id, QObject* context, CostDone done) {
    // encodeURIComponent: everything but A-Z a-z 0-9 - _ . ! ~ * ' ( )
    const QString path = QStringLiteral("/generation?id=") + QString::fromLatin1(QUrl::toPercentEncoding(id, QByteArrayLiteral("!*'()")));
    return json(path, std::nullopt, context,
                [done](const QJsonValue& data, const Response&) {
                    const QJsonValue d = at(data, QLatin1StringView("data"));
                    QJsonValue c = at(d, QLatin1StringView("total_cost"));
                    if (nullish(c)) c = at(d, QLatin1StringView("usage"));
                    std::optional<double> usd;
                    std::optional<money::Micro> micro;
                    costInto(c, usd, micro);
                    if (done) done(usd, micro);
                },
                [done](const WireError&) {
                    if (done) done(std::nullopt, std::nullopt);
                });
}

quint64 OpenRouter::credits(QObject* context, CreditsDone done) {
    return json(QStringLiteral("/credits"), std::nullopt, context,
                [done](const QJsonValue& data, const Response&) {
                    const QJsonValue d = at(data, QLatin1StringView("data"));
                    if (!d.isObject()) {   // TS: d.data.total_credits throws -> undefined
                        if (done) done(std::nullopt);
                        return;
                    }
                    Credits c;
                    if (const QJsonValue t = at(d, QLatin1StringView("total_credits")); t.isDouble()) c.total = t.toDouble();
                    if (const QJsonValue u = at(d, QLatin1StringView("total_usage")); u.isDouble()) c.used = u.toDouble();
                    if (done) done(c);
                },
                [done](const WireError&) {
                    if (done) done(std::nullopt);
                });
}

} // namespace studio::provider
