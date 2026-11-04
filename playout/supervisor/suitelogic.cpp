#include "suitelogic.h"
#include "livestream.h"

#include <QHash>
#include <QHostAddress>
#include <QRegularExpression>
#include <QUrl>
#include <QtMath>
#include <algorithm>

namespace suites {

// =====================================================================================================
// render
// =====================================================================================================
int nodeBusy(const QJsonObject& health) {
    const QJsonObject q = health.value("queue").toObject();
    return q.value("running").toInt() + q.value("queued").toInt();
}

static bool optedIn(const QJsonObject& node) {
    if (node.contains("render")) return node.value("render").toBool();
    QString notes = node.value("notes").toString().toLower(); notes.remove(' ');
    return notes.contains("render=true") || notes.contains("mesh-render=true");
}

bool nodeEligible(const QJsonObject& node, QString* why) {
    auto no = [why](const QString& w) { if (why) *why = w; return false; };
    if (!node.value("enabled").toBool(true)) return no("disabled");
    if (!optedIn(node)) return no("not opted in to render work");
    if (node.value("primary").toBool()) return no("a primary playout node never takes render work");
    const QJsonObject h = node.value("health").toObject();
    if (!h.value("ok").toBool()) return no(h.value("reason").toString("not reachable"));
    if (h.value("paused").toBool()) return no("background work paused (on air)");
    if (h.value("failoverRole").toString() == "primary") return no("failover primary");
    const QString kind = node.value("kind").toString();
    if (kind == "spotnode") { if (!h.value("ready").toBool()) return no(h.value("reason").toString("spot-node not ready")); }
    else if (kind == "playplay") {
        if (!h.value("meshRender").toBool()) return no("peer PlayPlay predates durable render requests");
        if (nodeBusy(h) > node.value("maxBusy").toInt(1)) return no("busy");
    } else if (nodeBusy(h) > node.value("maxBusy").toInt(1)) return no("busy");
    if (why) why->clear();
    return true;
}

QJsonObject chooseRenderNode(const QJsonArray& nodes, const QString& kind) {
    QJsonObject best; int bestBusy = 1 << 30; QString bestName;
    for (const QJsonValue& v : nodes) {
        const QJsonObject n = v.toObject();
        if (!kind.isEmpty() && n.value("kind").toString() != kind) continue;
        if (!nodeEligible(n)) continue;
        const int busy = nodeBusy(n.value("health").toObject());
        const QString name = n.value("name").toString(n.value("id").toString());
        if (busy < bestBusy || (busy == bestBusy && name < bestName)) { best = n; bestBusy = busy; bestName = name; }
    }
    return best;
}

QJsonArray planSegments(double durationSec, int segments, const QStringList& workers) {
    QJsonArray out;
    if (durationSec <= 0 || segments < 1) return out;
    segments = qBound(1, segments, 50);
    const QStringList w = workers.isEmpty() ? QStringList{"local"} : workers;
    const double step = durationSec / segments;
    for (int i = 0; i < segments; ++i) {
        const double start = std::round(step * i * 1000.0) / 1000.0;
        const double end = i == segments - 1 ? durationSec : std::round(step * (i + 1) * 1000.0) / 1000.0;
        QJsonObject s; s["index"] = i; s["startSec"] = start; s["durSec"] = std::round((end - start) * 1000.0) / 1000.0; s["worker"] = w.at(i % w.size());
        out.append(s);
    }
    return out;
}

QStringList presetArgs(const QString& preset, bool* ok) {
    if (ok) *ok = true;
    const QStringList aac = {"-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2"};
    auto video = [&aac](int w, int h, const QString& crf, const QString& speed) {
        QStringList a = {"-vf", QString("scale=%1:%2:force_original_aspect_ratio=decrease,pad=%1:%2:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=30").arg(w).arg(h),
                         "-c:v", "libx264", "-preset", speed, "-crf", crf, "-pix_fmt", "yuv420p", "-g", "60"};
        return a + aac + QStringList{"-movflags", "+faststart"};
    };
    if (preset == "h264_1080p30") return video(1920, 1080, "20", "veryfast");
    if (preset == "h264_720p30") return video(1280, 720, "21", "veryfast");
    if (preset == "proxy_540p") return QStringList{"-vf", "scale=-2:540,fps=30", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "28", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k", "-movflags", "+faststart"};
    if (preset == "audio_wav") return QStringList{"-vn", "-c:a", "pcm_s16le", "-ar", "48000", "-ac", "2"};
    if (preset == "loudnorm") return QStringList{"-c:v", "copy", "-af", "loudnorm=I=-24:TP=-2:LRA=7", "-c:a", "aac", "-b:a", "192k", "-ar", "48000"};
    if (preset == "copy") return QStringList{"-c", "copy"};
    if (ok) *ok = false;
    return {};
}

QString presetExt(const QString& preset) { return preset == "audio_wav" ? "wav" : "mp4"; }

double progressSeconds(const QByteArray& chunk) {
    double last = -1;
    const QList<QByteArray> lines = chunk.split('\n');
    for (const QByteArray& raw : lines) {
        const QByteArray l = raw.trimmed();
        if (l.startsWith("out_time_us=") || l.startsWith("out_time_ms=")) {   // both are microseconds in ffmpeg's -progress
            bool ok = false; const qint64 us = l.mid(l.indexOf('=') + 1).toLongLong(&ok); if (ok && us >= 0) last = us / 1e6;
        } else if (l.startsWith("out_time=")) {
            const QList<QByteArray> p = l.mid(9).split(':');
            if (p.size() == 3) { bool a, b, c; const double s = p[0].toDouble(&a) * 3600 + p[1].toDouble(&b) * 60 + p[2].toDouble(&c); if (a && b && c && s >= 0) last = s; }
        }
    }
    return last;
}

// =====================================================================================================
// Star nodes / stations
// =====================================================================================================
static const QList<QPair<QString, QRegularExpression>>& allowTable() {
    static const QList<QPair<QString, QRegularExpression>> t = [] {
        const QString id = "[A-Za-z0-9_\\-]+";
        const QList<QPair<QString, QString>> raw = {
            {"GET", "^/api/status$"}, {"GET", "^/api/v1/status$"}, {"GET", "^/api/state$"},
            {"GET", "^/api/channels$"}, {"GET", "^/api/channels/" + id + "/state$"},
            {"GET", "^/api/schedule/" + id + "$"}, {"GET", "^/api/schedule/" + id + "/\\d/\\d{1,2}$"},
            {"PUT", "^/api/schedule/" + id + "/\\d/\\d{1,2}$"},
            {"POST", "^/api/schedule/" + id + "/\\d/shuffle$"}, {"POST", "^/api/schedule/" + id + "/\\d/\\d{1,2}/(shuffle|autofill)$"},
            {"POST", "^/api/schedule/" + id + "/hour-recipe$"}, {"POST", "^/api/hour-recipes/week-plan$"},
            {"GET", "^/api/hour-recipes$"}, {"PUT", "^/api/hour-recipes$"},
            {"GET", "^/api/inputs$"}, {"GET", "^/api/media$"}, {"GET", "^/api/media/(?!ingest$)" + id + "$"},
            {"GET", "^/api/media/subcategories$"}, {"GET", "^/api/media/series$"},
            {"POST", "^/api/containers/resolve$"}, {"POST", "^/api/media/ingest$"}, {"GET", "^/api/ingest/folder/" + id + "$"},
            {"GET", "^/api/newsroom/overview$"}, {"POST", "^/api/newsroom/spot$"}, {"POST", "^/api/newsroom/jobs/" + id + "/run$"},
            {"GET", "^/api/newsroom/render-request/" + id + "$"},
            {"GET", "^/api/events/recent$"}, {"GET", "^/api/fs/home$"}, {"GET", "^/api/fs/list$"},
            // C++ stations: the whole system's read surface, plus transport and remote-pad presses (the pad holds the operator's own buttons)
            {"GET", "^/api/v1/(outputs|readiness|system/stats|live/facebook|remote/pad|spots/status|mux/status|channels)$"},
            {"GET", "^/api/v1/(channels|outputs|flightplan|overlays)/" + id + "(/outputs)?$"},
            {"POST", "^/api/v1/channels/" + id + "/transport$"}, {"POST", "^/api/v1/remote/pad/" + id + "/press$"},
        };
        QList<QPair<QString, QRegularExpression>> o;
        for (const auto& p : raw) o.append({p.first, QRegularExpression(p.second)});
        return o;
    }();
    return t;
}

bool stationPathAllowed(const QString& method, const QString& path) {
    const QString m = method.trimmed().toUpper();
    const QString bare = path.section('?', 0, 0);
    if (!bare.startsWith("/api/") || bare.contains("..") || bare.contains("//")) return false;
    for (const auto& a : allowTable()) if (a.first == m && a.second.match(bare).hasMatch()) return true;
    return false;
}

QStringList stationAllowPatterns() {
    QStringList o; for (const auto& a : allowTable()) o << a.first + " " + a.second.pattern(); return o;
}

QStringList fleetReadCommands() { return {"on_air", "system_health", "as_played_today", "run_report", "adherence", "schedule_hour", "upcoming", "channel_list", "outputs", "facebook_status", "system_stats", "readiness", "spots_status", "library_stats"}; }
bool fleetCommandAllowed(const QString& command) { return fleetReadCommands().contains(command); }
QStringList starNodeActions() {
    return {"fleet.status", "fleet.query", "fleet.asPlayed", "fleet.analytics", "sync.jobs", "sync.run", "swarm.index", "browse",
            "state", "service.restart", "station.status", "station.capabilities", "station.call", "exec"};
}
QStringList restartableServices() { return {"playplay-server", "mediamtx", "obs-ch1", "obs-ch2", "obs-ch3", "obs-ch4"}; }
bool starNodeActionAllowed(const QString& action) { return starNodeActions().contains(action); }

// =====================================================================================================
// publisher
// =====================================================================================================
QString draftTier(const QString& text) {
    static const QRegularExpression rx(
        "\\b(obituar\\w*|passed away|funeral|died|dies|death\\w*|dead|killed|fatal\\w*|homicide\\w*|murder\\w*|manslaughter|shoot\\w*|shot dead|stabb\\w*|"
        "arrest\\w*|charged|indict\\w*|crime\\w*|criminal|assault\\w*|injur\\w*|wounded|hospitali[sz]ed|minors?|juvenile\\w*|child(?:ren)?|teen\\w*|"
        "alleg\\w*|accus\\w*|suspect\\w*|abuse\\w*)\\b",
        QRegularExpression::CaseInsensitiveOption);
    return rx.match(text).hasMatch() ? "absolute" : "default";
}

bool publicHttpsUrl(const QString& url) {
    const QUrl u(url.trimmed());
    if (!u.isValid() || u.scheme().toLower() != "https") return false;
    const QString host = u.host().toLower();
    if (host.isEmpty() || host == "localhost" || host.endsWith(".local") || host.endsWith(".lan") || host.endsWith(".internal") || host.endsWith(".ts.net")) return false;
    QHostAddress a;
    if (a.setAddress(host)) {
        if (a.isLoopback() || a.isLinkLocal() || a.isNull()) return false;
        if (a.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 ip = a.toIPv4Address();
            auto in = [ip](quint32 net, int bits) { const quint32 mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits); return (ip & mask) == (net & mask); };
            if (in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) || in(0x64400000, 10) || in(0x7F000000, 8) || in(0xA9FE0000, 16) || in(0, 8)) return false;
        } else if (a.isInSubnet(QHostAddress("fc00::"), 7)) return false;
        return true;
    }
    return host.contains('.');
}

QJsonObject classifyDraft(const QJsonObject& item, const QString& mode) {
    QJsonObject o;
    const QString source = item.value("source").toString("manual");
    const QString text = item.value("title").toString() + "\n" + item.value("body").toString() + "\n" + item.value("caption").toString();
    const QString tier = draftTier(text);
    bool aiText = item.value("ai").toObject().value("text").toBool();
    o["tier"] = tier;
    if (source == "agent_draft") {
        aiText = true;
        bool hasSource = false;
        for (const QJsonValue& v : item.value("sourceLinks").toArray()) if (publicHttpsUrl(v.toString())) { hasSource = true; break; }
        if (!hasSource) { o["status"] = "held"; o["why"] = "an agent draft needs at least one public https source link"; o["aiText"] = aiText; return o; }
    }
    o["aiText"] = aiText;
    if (tier == "absolute") { o["status"] = "held"; o["why"] = "sensitive subject (obituaries, crime, injury or death, minors, allegations): a person must approve it"; return o; }
    if (item.value("status").toString() == "draft") { o["status"] = "draft"; o["why"] = "kept as a draft"; return o; }
    if (mode == "auto" && (source == "spot_job" || source == "spot_studio")) { o["status"] = "approved"; o["why"] = "auto mode: finished spots post without a human step"; return o; }
    o["status"] = "pending_approval"; o["why"] = mode == "auto" ? "only finished spots are auto-approved" : "review mode: waiting for an approver";
    return o;
}

bool canTransition(const QString& from, const QString& to) {
    static const QHash<QString, QStringList> t = {
        {"draft", {"pending_approval", "held", "approved", "skipped"}},
        {"pending_approval", {"approved", "held", "skipped", "draft"}},
        {"held", {"pending_approval", "approved", "skipped", "draft"}},
        {"approved", {"scheduled", "publishing", "held", "skipped", "pending_approval"}},
        {"scheduled", {"publishing", "approved", "held", "skipped"}},
        {"publishing", {"published", "failed", "approved"}},
        {"failed", {"approved", "skipped", "publishing", "held"}},
        {"published", {"corrected", "retracted"}},
        {"corrected", {"retracted"}},
        {"skipped", {"pending_approval", "draft"}},
        {"retracted", {}},
    };
    return t.value(from).contains(to);
}

QString withAiLabel(const QString& text, const QJsonObject& ai, const QString& label) {
    const bool made = ai.value("text").toBool() || ai.value("voice").toBool() || ai.value("images").toBool() || ai.value("video").toBool();
    if (!made || label.trimmed().isEmpty() || text.contains(label.trimmed())) return text;
    return text.trimmed().isEmpty() ? label.trimmed() : text.trimmed() + "\n\n" + label.trimmed();
}

QString socialBlurb(const QString& headline, const QString& narration, const QString& tagline) {
    QString lead;
    for (const QString& sent : QString(narration).replace('\n', ' ').split(". ", Qt::SkipEmptyParts)) {
        if (lead.size() + sent.size() > 220) break;
        QString s = sent.trimmed(); if (s.endsWith('.')) s.chop(1);
        lead += s + ". ";
    }
    QStringList parts;
    if (!headline.trimmed().isEmpty()) parts << headline.trimmed();
    auto strip = [](QString s) { s = s.trimmed(); while (s.endsWith('.') || s.endsWith(' ')) s.chop(1); while (s.startsWith('.') || s.startsWith(' ')) s.remove(0, 1); return s; };
    if (!lead.trimmed().isEmpty() && strip(lead) != strip(headline)) parts << lead.trimmed();
    if (!tagline.trimmed().isEmpty()) parts << tagline.trimmed();
    return parts.join("\n\n");
}

bool statusInGroup(const QString& status, const QString& group) {
    if (group.isEmpty() || group == "all") return true;
    if (group == "open") return QStringList{"draft", "pending_approval", "held", "approved", "scheduled", "publishing", "failed"}.contains(status);
    if (group == "waiting") return QStringList{"pending_approval", "held"}.contains(status);
    if (group == "needs_attention") return QStringList{"held", "failed"}.contains(status);
    if (group == "done") return QStringList{"published", "skipped", "retracted", "corrected"}.contains(status);
    return group.split(',').contains(status);
}

// =====================================================================================================
// spot studio
// =====================================================================================================
QStringList sentences(const QString& text) {
    static const QRegularExpression rx("(?<=[.!?])\\s+");
    QStringList out;
    for (const QString& s : text.simplified().split(rx, Qt::SkipEmptyParts)) { const QString t = s.trimmed(); if (!t.isEmpty()) out << t; }
    return out;
}

int sceneCountFor(int lengthSec) { return lengthSec <= 6 ? 2 : lengthSec <= 15 ? 3 : lengthSec <= 30 ? 5 : 8; }

static double round1(double v) { return std::round(v * 10.0) / 10.0; }

QJsonArray balanceScenes(const QJsonArray& scenes, double lengthSec) {
    QJsonArray out; const int n = scenes.size();
    if (n == 0 || lengthSec <= 0) return scenes;
    QList<double> w; double total = 0;
    for (const QJsonValue& v : scenes) { double d = v.toObject().value("durationSec").toDouble(); if (d <= 0) d = 1; w << d; total += d; }
    const double minD = n * 0.8 > lengthSec ? lengthSec / n : 0.8;
    QList<double> d; double used = 0;
    for (int i = 0; i < n - 1; ++i) { double x = round1(qMax(minD, w[i] / total * lengthSec)); d << x; used += x; }
    double last = round1(lengthSec - used);
    if (last < minD && n > 1) {                     // the minimum pushed the others over: take the deficit from the longest scenes
        double need = minD - last;
        while (need > 0.049) {
            int k = 0; for (int i = 1; i < d.size(); ++i) if (d[i] > d[k]) k = i;
            const double give = qMin(need, d[k] - minD); if (give <= 0.049) break;
            d[k] = round1(d[k] - give); need = round1(need - give);
        }
        used = 0; for (double x : d) used += x; last = round1(lengthSec - used);
    }
    d << last;
    for (int i = 0; i < n; ++i) { QJsonObject s = scenes.at(i).toObject(); s["n"] = i + 1; s["durationSec"] = d[i]; out.append(s); }
    return out;
}

static QString clipText(const QString& s, int n) { const QString t = s.simplified(); return t.size() <= n ? t : t.left(n - 1).trimmed() + QChar(0x2026); }
static int words(const QString& s) { return s.simplified().split(' ', Qt::SkipEmptyParts).size(); }

QString narrationOf(const QJsonArray& scenes, const QJsonObject& endCard) {
    QStringList parts;
    for (const QJsonValue& v : scenes) { const QString vo = v.toObject().value("vo").toString().trimmed(); if (!vo.isEmpty()) parts << vo; }
    const QString evo = endCard.value("vo").toString().trimmed(); if (!evo.isEmpty()) parts << evo;
    return parts.join(' ');
}

QJsonObject draftScript(const QJsonObject& opts, int concept) {
    const QString brief = opts.value("brief").toString().trimmed();
    const int length = qBound(6, opts.value("lengthSec").toInt(30), 120);
    const QJsonObject brand = opts.value("brand").toObject();
    const QString name = brand.value("name").toString(opts.value("client").toString()).trimmed();
    const QString cta = brand.value("cta").toString().trimmed(), url = brand.value("url").toString().trimmed();
    QStringList facts = sentences(brief);
    if (facts.isEmpty()) facts << (name.isEmpty() ? QString("Here's something worth knowing.") : name + ".");
    auto lowerFirst = [](QString s) { if (!s.isEmpty() && s.size() > 1 && s[1].isLower()) s[0] = s[0].toLower(); return s; };

    QJsonArray concepts;
    { QJsonObject c; c["title"] = "Straight sell"; c["angle"] = "Lead with the offer, then the details, then the call to action."; c["hook"] = facts.first(); concepts.append(c); }
    { QJsonObject c; c["title"] = "Name first"; c["angle"] = "Open on the name so it sticks, then the reason to act."; c["hook"] = name.isEmpty() ? facts.first() : name + ". " + facts.first(); concepts.append(c); }
    { QJsonObject c; c["title"] = "Close to home"; c["angle"] = "Warm, local framing of the same facts."; c["hook"] = "Right here in our community, " + lowerFirst(facts.first()); concepts.append(c); }
    concept = qBound(1, concept, concepts.size());
    const int sel = qBound(1, opts.value("concepts").toInt(3), 3);
    while (concepts.size() > sel) concepts.removeLast();
    concept = qBound(1, concept, concepts.size());

    int n = sceneCountFor(length);
    const int content = qMax(1, qMin(n - 1, facts.size()));
    n = content + 1;
    static const QStringList beats = {"hook", "detail", "benefit", "proof", "offer", "detail", "reason", "reminder"};
    QJsonArray scenes;
    QStringList rest = facts.mid(1);
    for (int i = 0; i < content; ++i) {
        QString vo;
        if (i == 0) vo = concepts.at(concept - 1).toObject().value("hook").toString();
        else {
            const int remaining = content - i; const int take = qMax(1, int(std::ceil(double(rest.size()) / remaining)));
            vo = QStringList(rest.mid(0, take)).join(' '); rest = rest.mid(take);
        }
        QJsonObject s; s["beat"] = beats.value(i, "detail"); s["vo"] = vo; s["onScreen"] = clipText(vo, 40);
        s["visual"] = QString("Card: %1").arg(clipText(vo, 60)); s["durationSec"] = qMax(1, words(vo));
        scenes.append(s);
    }
    QJsonObject endCard; endCard["headline"] = name; endCard["cta"] = cta.isEmpty() ? QString("Learn more") : cta; endCard["url"] = url;
    endCard["vo"] = name.isEmpty() ? endCard.value("cta").toString() + "." : name + ". " + endCard.value("cta").toString() + ".";
    { QJsonObject s; s["beat"] = "end card"; s["vo"] = QString(); s["onScreen"] = clipText(name.isEmpty() ? endCard.value("cta").toString() : name, 40);
      s["visual"] = "End card: name, call to action" + QString(url.isEmpty() ? "" : ", " + url); s["durationSec"] = qMax(2, words(endCard.value("vo").toString())); s["endCard"] = true; scenes.append(s); }
    scenes = balanceScenes(scenes, length);

    QJsonArray questions;
    auto ask = [&questions](const QString& id, const QString& q) { QJsonObject o; o["id"] = id; o["q"] = q; questions.append(o); };
    if (name.isEmpty()) ask("brand.name", "What is the business or brand name for the end card?");
    if (cta.isEmpty()) ask("brand.cta", "What should viewers do (the call to action)?");
    if (url.isEmpty()) ask("brand.url", "Website or phone number for the end card?");
    const int totalWords = words(narrationOf(scenes, endCard));
    if (totalWords > length * 2.7) ask("lengthSec", QString("The script has %1 words, long for %2 s: shorten the brief or choose a longer spot?").arg(totalWords).arg(length));
    QJsonObject o; o["concepts"] = concepts; o["concept"] = concept; o["scenes"] = scenes; o["endCard"] = endCard; o["questions"] = questions; o["words"] = totalWords;
    return o;
}

// =====================================================================================================
// AI models
// =====================================================================================================
QString defaultAiModel() { return "anthropic/claude-sonnet-5.5"; }

bool isLegacyModel(const QString& id) {
    static const QRegularExpression rx(
        "^(?:openai/(?:gpt-4|gpt-3\\.5|chatgpt-4o|text-davinci)|anthropic/claude-(?:3|2|instant)|google/(?:gemini-(?:1\\.|2\\.0|pro\\b|pro-vision|flash-1)|palm))",
        QRegularExpression::CaseInsensitiveOption);
    const QString t = id.trimmed();
    if (t.isEmpty()) return false;
    if (!t.contains('/')) { static const QRegularExpression bare("^(?:gpt-4|gpt-3\\.5|claude-3|claude-2|gemini-1|gemini-2\\.0)", QRegularExpression::CaseInsensitiveOption); return bare.match(t).hasMatch(); }
    return rx.match(t).hasMatch();
}

QString resolveAiModel(const QString& requested) {
    const QString t = requested.trimmed();
    return t.isEmpty() || isLegacyModel(t) ? defaultAiModel() : t;
}

static double perMtok(const QJsonValue& v) { return std::round(v.toVariant().toDouble() * 1e6 * 1000.0) / 1000.0; }

QJsonArray filterModelCatalog(const QJsonArray& data, qint64 nowSec, int maxAgeDays) {
    QList<QJsonObject> rows;
    for (const QJsonValue& v : data) {
        const QJsonObject m = v.toObject(); const QString id = m.value("id").toString();
        if (id.isEmpty() || isLegacyModel(id)) continue;
        const QJsonObject arch = m.value("architecture").toObject();
        const QJsonArray in = arch.value("input_modalities").toArray(), out = arch.value("output_modalities").toArray();
        if (!out.isEmpty() && !out.contains("text")) continue;
        if (!in.isEmpty() && !in.contains("text")) continue;
        if (in.isEmpty() && out.isEmpty() && !arch.value("modality").toString("text->text").endsWith("->text")) continue;
        const qint64 created = qint64(m.value("created").toDouble());
        if (maxAgeDays > 0 && created > 0 && nowSec - created > qint64(maxAgeDays) * 86400) continue;
        const QJsonObject pr = m.value("pricing").toObject();
        QJsonObject r; r["id"] = id; r["name"] = m.value("name").toString(id); r["created"] = double(created); r["context"] = m.value("context_length").toDouble();
        r["promptPerMtok"] = perMtok(pr.value("prompt")); r["completionPerMtok"] = perMtok(pr.value("completion")); r["vision"] = in.contains("image");
        rows << r;
    }
    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) { const double x = a.value("created").toDouble(), y = b.value("created").toDouble(); return x != y ? x > y : a.value("id").toString() < b.value("id").toString(); });
    QJsonArray out; for (const QJsonObject& r : rows) out.append(r); return out;
}

QJsonArray builtinModelCatalog() {
    struct M { const char* id; const char* name; qint64 created; double ctx, in, out; };
    static const M list[] = {   // measured from https://openrouter.ai/api/v1/models on 2026-10-23
        {"anthropic/claude-sonnet-5.5", "Anthropic: Claude Sonnet 5.5", 1790618686, 1000000, 2, 10},
        {"anthropic/claude-haiku-5.5", "Anthropic: Claude Haiku 5.5", 1791397883, 1000000, 0.1, 0.5},
        {"anthropic/claude-opus-5.5", "Anthropic: Claude Opus 5.5", 1790094732, 1000000, 4, 20},
        {"openai/gpt-6.1-sol", "OpenAI: GPT-6.1 Sol", 1790702882, 1050000, 2, 10},
        {"openai/gpt-6-luna", "OpenAI: GPT-6 Luna", 1790100786, 1050000, 0.1, 0.5},
        {"google/gemini-4-argon", "Google: Gemini 4 Argon", 1791907477, 1048576, 2, 10},
        {"google/gemini-3.8-flash", "Google: Gemini 3.8 Flash", 1788362056, 1048576, 0.825, 4.125},
        {"x-ai/grok-4.7", "xAI: Grok 4.7", 1790007541, 500000, 2, 6},
        {"deepseek/deepseek-v4.1-flash-1017", "DeepSeek: DeepSeek V4.1 Flash 1017", 1792225624, 1048576, 0.3, 1.2},
        {"mistralai/mistral-large-4-0", "Mistral: Mistral Large 4", 1791294122, 1048576, 1.36, 4.18},
        {"qwen/qwen3.8-max-prime", "Qwen: Qwen3.8 Max Prime", 1790191228, 1000000, 4, 12},
        {"moonshotai/kimi-k3", "MoonshotAI: Kimi K3", 1784215858, 1048576, 0.239, 11.936},
    };
    QJsonArray data;
    for (const M& m : list) {
        QJsonObject o; o["id"] = m.id; o["name"] = m.name; o["created"] = double(m.created); o["context_length"] = m.ctx;
        o["pricing"] = QJsonObject{{"prompt", QString::number(m.in / 1e6, 'g', 10)}, {"completion", QString::number(m.out / 1e6, 'g', 10)}};
        o["architecture"] = QJsonObject{{"input_modalities", QJsonArray{"text"}}, {"output_modalities", QJsonArray{"text"}}};
        data.append(o);
    }
    return filterModelCatalog(data, 0, 0);
}

// =====================================================================================================
// agents
// =====================================================================================================
QJsonArray operatorCommands() {
    struct C { const char* name; const char* risk; const char* desc; QList<QPair<const char*, const char*>> params; };
    static const QList<C> table = {
        {"channel_list", "read", "Every channel with its engine state and what is on air", {}},
        {"on_air", "read", "What is on air now and next", {{"channel", "channel id, blank = all"}}},
        {"system_health", "read", "Supervisor health: uptime, engines, alerts, disk", {}},
        {"schedule_hour", "read", "The items of one hour", {{"channel", "required"}, {"date", "yyyy-MM-dd, default today"}, {"hour", "0-23, default now"}}},
        {"upcoming", "read", "The next items on a channel", {{"channel", "required"}, {"count", "default 10"}}},
        {"library_stats", "read", "Counts of the media library by type", {}},
        {"find_media", "read", "Search titles and transcripts", {{"q", "text"}, {"limit", "default 20"}}},
        {"as_played_today", "read", "Today's as-run on a channel", {{"channel", "required"}}},
        {"expiring_soon", "read", "Media whose play window ends soon", {{"days", "default 14"}}},
        {"spots_status", "read", "Generated spots: current render, queue, jobs", {}},
        {"render_status", "read", "Render nodes and jobs", {}},
        {"publish_queue", "read", "Publishing items waiting or failed", {}},
        {"star_nodes", "read", "Enrolled Star nodes and their health", {}},
        {"skip_current", "action", "Skip the item on air (takes effect immediately)", {{"channel", "required"}}},
        {"arm", "action", "Arm a channel's automation", {{"channel", "required"}}},
        {"disarm", "action", "Disarm a channel's automation", {{"channel", "required"}}},
        {"snap", "action", "Snap a channel to the wall-clock position", {{"channel", "required"}}},
        {"run_spot_job", "action", "Run a generated-spot job now", {{"id", "spot job id"}}},
        {"enforce_markers", "action", "Re-place every GEN marker in the grid", {}},
        {"publish_approve", "action", "Approve a publishing item", {{"id", "publisher item id"}}},
        // --- the whole system: discovery, streaming, outputs, pad, spots, stats ---------------------------------
        {"capabilities", "read", "Every REST route this station serves, grouped, with how an agent may use each (api_get / api_call / blocked)", {{"group", "channels, spots, live ...; blank = group counts only"}}},
        {"api_get", "read", "GET any station route (credentials are hidden). Find routes with capabilities", {{"path", "e.g. /outputs or /flightplan/main (with or without /api/v1)"}}},
        {"api_call", "action", "POST or PUT an operator route (transport, outputs, cg, schedule, spots, overlays, mux ...). Never settings, sessions, failover or channel definitions", {{"method", "POST or PUT"}, {"path", "route path"}, {"body", "JSON object"}}},
        {"outputs", "read", "Streaming outputs of every channel with state, bitrate and errors", {{"channel", "blank = all"}}},
        {"output_control", "action", "Start, stop or restart one output of a channel", {{"channel", "required"}, {"name", "output name"}, {"op", "start | stop | restart"}}},
        {"facebook_status", "read", "Facebook Live: channel, state, uptime, bitrate, last error (the stream key is never shown)", {}},
        {"facebook_go_live", "action", "Start the Facebook Live stream (settings must have a channel and key)", {}},
        {"facebook_end", "action", "End the Facebook Live stream", {}},
        {"pad_list", "read", "The remote pad's buttons (node, channel, call) and theme", {}},
        {"pad_press", "action", "Press a remote pad button by id (calls this station or a remote node)", {{"id", "button id"}}},
        {"spot_generate", "action", "Generate one spot from a recipe now (headlines, weather_am, sports_local ...; music is composed automatically)", {{"recipe", "recipe key"}}},
        {"system_stats", "read", "CPU, memory, network and per-channel fps / bitrate", {}},
        {"readiness", "read", "Readiness of the schedule and media for the coming hours", {}},
        {"flight_plan", "read", "Time debt and corrections of a channel's current interval", {{"channel", "required"}}},
    };
    QJsonArray a;
    for (const C& c : table) {
        QJsonObject o; o["name"] = c.name; o["risk"] = c.risk; o["desc"] = c.desc;
        QJsonObject p; for (const auto& kv : c.params) p[kv.first] = kv.second; o["params"] = p;
        a.append(o);
    }
    return a;
}

QJsonValue redactSecrets(const QJsonValue& v) {
    static const QRegularExpression secretKey(QStringLiteral("(api[_-]?key|token|passw(or)?d|secret|passphrase|stream[_-]?key|authorization|credential|cookie)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression queryKey(QStringLiteral("([?&](token|key|password|passphrase|secret|apikey|api_key)=)[^&#\\s\"]+"), QRegularExpression::CaseInsensitiveOption);
    if (v.isObject()) {
        QJsonObject in = v.toObject(), out;
        for (auto it = in.begin(); it != in.end(); ++it) {
            const QString k = it.key();
            if (it.value().isString() && !k.startsWith("has", Qt::CaseInsensitive) && secretKey.match(k).hasMatch() && !it.value().toString().isEmpty()) out[k] = "(hidden)";
            else out[k] = redactSecrets(it.value());
        }
        return out;
    }
    if (v.isArray()) { QJsonArray a; for (const QJsonValue& x : v.toArray()) a.append(redactSecrets(x)); return a; }
    if (v.isString()) { QString s = livestream::redactUrl(v.toString()); if (s.contains('?')) s.replace(queryKey, "\\1(hidden)"); return s; }
    return v;
}

static QString normApiPath(const QString& path) {
    QString p = path.trimmed(); if (p.startsWith("/api/v1")) p = p.mid(7);
    if (!p.startsWith('/')) p = "/" + p;
    return p;
}

bool agentApiAllowed(const QString& method, const QString& path, QString* why) {
    auto no = [why](const QString& w) { if (why) *why = w; return false; };
    const QString m = method.trimmed().toUpper(); QString p = normApiPath(path);
    const QString bare = p.section('?', 0, 0), low = bare.toLower();
    if (bare.contains("..") || bare.contains("//") || low.contains("%2e") || low.contains("%2f") || low.contains("\\")) return no("bad path");
    static const QStringList neverAny = {"/settings", "/remote/nodes", "/sessions/export", "/preview", "/agents", "/api/op", "/starnodes/enrol"};
    for (const QString& n : neverAny) if (low == n || low.startsWith(n + "/")) return no("this route carries credentials or is the agent itself");
    if (low.startsWith("/sessions/") && low.endsWith("/export")) return no("session exports carry station state");
    if (m == "GET" || m == "HEAD") return true;
    if (m == "DELETE") return no("deleting is done in the dashboard, not by an agent");
    if (m != "POST" && m != "PUT") return no("method must be GET, POST or PUT");
    static const QStringList neverWrite = {"/settings", "/sessions", "/failover", "/starnodes", "/agents", "/remote/nodes", "/station", "/alerts", "/ai", "/render/nodes"};
    for (const QString& n : neverWrite) if (low == n || low.startsWith(n + "/")) return no("changes to " + n + " are made in the dashboard");
    const QStringList seg = low.split('/', Qt::SkipEmptyParts);
    if (seg.size() == 2 && seg[0] == "channels") return no("a channel definition change restarts its engine: use the dashboard");
    if (seg.size() >= 3 && seg[0] == "remote" && seg[1] == "pad") { if (seg.size() == 4 && seg[3] == "press") return true; return no("only pad presses; the pad layout is edited in the dashboard"); }
    static const QStringList areas = {"channels", "schedule", "spots", "live", "overlays", "outputs", "mux", "library", "inputs", "flightplan", "programming", "ingest", "publish", "render", "media", "readiness", "onair", "cg", "flight"};
    if (seg.isEmpty() || !areas.contains(seg[0])) return no("not an operator area");
    return true;
}

QString routeGroup(const QString& path) {
    QString p = path.trimmed(); if (p.startsWith("/api/v1/")) p = p.mid(7); else if (p.startsWith("/api/")) p = "/station" + p.mid(4);
    const QStringList seg = p.split('/', Qt::SkipEmptyParts); return seg.isEmpty() ? QStringLiteral("root") : seg[0];
}

bool operatorCommandIsAction(const QString& name) {
    for (const QJsonValue& v : operatorCommands()) if (v.toObject().value("name").toString() == name) return v.toObject().value("risk").toString() == "action";
    return false;
}

} // namespace suites
