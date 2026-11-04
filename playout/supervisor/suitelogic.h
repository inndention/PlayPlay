#ifndef SUITELOGIC_H
#define SUITELOGIC_H

#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>

/**
 * suitelogic — the pure rules behind the render / Star node / spot studio / publisher / agent suites
 * (no network, no store), so they can be unit-tested (tests/test_suites.cpp).
 *
 * Ported rules and where they come from in PlayPlay OBS:
 *   chooseRenderNode     mesh_render.choose_idle_node: opt-in, not primary, not paused, queue <= maxBusy
 *   stationPathAllowed   stations.ALLOW: the only PlayPlay calls the HUB may make on another station
 *   fleetCommandAllowed  star-node fleet.js ALLOWED_READ_COMMANDS
 *   classifyDraft        the publishing lane rules (absolute tier, agent drafts need sources, AI label)
 *   socialBlurb          social._blurb (headline + first sentences + tagline)
 *   balanceScenes        guided spot timing: scene durations always add up to the spot length
 */
namespace suites {

// ---------------------------------------------------------------- render
/// Busy count of a node from its health block: queue.running + queue.queued.
int nodeBusy(const QJsonObject& health);
/// True when a node may take dispatched render work right now (enabled, opted in, not primary, healthy, not paused, not too busy).
bool nodeEligible(const QJsonObject& node, QString* why = nullptr);
/// Least-busy eligible node (ties by name); empty object when none.
QJsonObject chooseRenderNode(const QJsonArray& nodes, const QString& kind = QString());
/// Segment plan for a split render: [{index, startSec, durSec, worker}], workers assigned round robin.
QJsonArray planSegments(double durationSec, int segments, const QStringList& workers);
/// ffmpeg output arguments (after the input) for a preset; ok=false for an unknown preset.
QStringList presetArgs(const QString& preset, bool* ok = nullptr);
/// File extension a preset writes.
QString presetExt(const QString& preset);
/// Parse "out_time_us=" / "out_time_ms=" / "out_time=" lines of ffmpeg -progress output; -1 when absent.
double progressSeconds(const QByteArray& chunk);

// ---------------------------------------------------------------- Star nodes / stations
bool stationPathAllowed(const QString& method, const QString& path);
QStringList stationAllowPatterns();          // human-readable list for the UI
QStringList fleetReadCommands();
bool fleetCommandAllowed(const QString& command);
QStringList starNodeActions();
QStringList restartableServices();
bool starNodeActionAllowed(const QString& action);

// ---------------------------------------------------------------- publisher
/// "absolute" when the text touches obituaries, crime, injury/death, minors or allegations; else "default".
QString draftTier(const QString& text);
/// True for an https URL whose host is public (not localhost, not a private/tailnet/link-local address).
bool publicHttpsUrl(const QString& url);
/// Classify a new or revised draft: returns {status, tier, why, aiText} for the publishing lane.
/// mode = off|review|auto (publisher config); source = manual|spot_job|spot_studio|agent_draft.
QJsonObject classifyDraft(const QJsonObject& item, const QString& mode);
/// Allowed forward moves of the publishing ladder.
bool canTransition(const QString& from, const QString& to);
/// Outgoing copy with the AI label appended when ai says AI made any part of it.
QString withAiLabel(const QString& text, const QJsonObject& ai, const QString& label);
/// Post body: headline + first sentence or two of the narration (<= ~220 chars) + tagline.
QString socialBlurb(const QString& headline, const QString& narration, const QString& tagline);
/// Status group filters used by the queue view: open, waiting, needs_attention, done.
bool statusInGroup(const QString& status, const QString& group);

// ---------------------------------------------------------------- spot studio
QStringList sentences(const QString& text);
/// Scenes for a spot length, end card included: 6 → 2, 15 → 3, 30 → 5, 60 → 8.
int sceneCountFor(int lengthSec);
/// Scale scene durations so they sum exactly to lengthSec (each >= 0.8 s, rounded to 0.1 s; the last absorbs the rest).
QJsonArray balanceScenes(const QJsonArray& scenes, double lengthSec);
/// Deterministic draft: {concepts[], scenes[], endCard{}, questions[]} from {brief, lengthSec, kind, brand{name,cta,url}, tone, client}.
QJsonObject draftScript(const QJsonObject& opts, int concept = 1);
/// Narration read for a script: every scene's vo then the end card vo.
QString narrationOf(const QJsonArray& scenes, const QJsonObject& endCard);

// ---------------------------------------------------------------- AI models (OpenRouter)
/// The model every OpenRouter caller uses unless a setting names a current one.
QString defaultAiModel();
/// Legacy families that are never offered or used: gpt-4o / gpt-4* / gpt-3.5, claude-3 / claude-2 / claude-instant,
/// gemini-1.x / gemini-2.0 / gemini-pro (1.0), palm. A legacy (or empty) id resolves to defaultAiModel().
bool isLegacyModel(const QString& id);
QString resolveAiModel(const QString& requested);
/// Shape the OpenRouter /models `data` array: text-in/text-out models only, legacy families always removed, models older
/// than maxAgeDays (0 = no age limit) removed, newest first: [{id, name, created, context, promptPerMtok, completionPerMtok, vision}].
QJsonArray filterModelCatalog(const QJsonArray& data, qint64 nowSec, int maxAgeDays);
/// A small current list used when OpenRouter cannot be reached and nothing is cached.
QJsonArray builtinModelCatalog();

// ---------------------------------------------------------------- agents
/// The operator command table: [{name, risk: read|action, desc, params{name: description}}].
QJsonArray operatorCommands();
bool operatorCommandIsAction(const QString& name);
/// Hide credentials in any JSON an agent can read: string values under keys that look like credentials (api keys, tokens, passwords, secrets,
/// passphrases, stream keys, authorization, cookies) become "(hidden)"; the last path segment of rtmp(s) URLs is masked; has* flags stay.
QJsonValue redactSecrets(const QJsonValue& v);
/// May an agent reach this /api/v1 path with this method through api_get / api_call? GET: everything except routes that carry credentials
/// (settings, remote node list, session exports). POST/PUT: only operator areas (channels' transport / outputs / cg ..., schedule, spots, live,
/// overlays, outputs, mux, library, inputs, flightplan, programming, ingest, publish, render, media, remote pad presses); never a channel
/// definition (PUT /channels/{id}), settings, sessions, failover, star nodes, agents, remote nodes. DELETE: never. `why` says why not.
bool agentApiAllowed(const QString& method, const QString& path, QString* why = nullptr);
/// "channels", "spots", ...: the area of a route path (/api/v1/... or station-compatible /api/...), for the capability list.
QString routeGroup(const QString& path);

} // namespace suites

#endif // SUITELOGIC_H
