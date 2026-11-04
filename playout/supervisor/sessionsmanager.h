#ifndef SESSIONSMANAGER_H
#define SESSIONSMANAGER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QList>
#include <QSqlDatabase>

namespace sched { class ScheduleStore; }

/**
 * SessionsManager — portable bundles of station state ("sessions").
 *
 * A session is a JSON bundle (format "playplay-session/1") holding the groups
 *   channels     channel profiles (outputs included; applied only when "outputs" is also requested)
 *   schedule     every hour slot of every channel (weekly template + dated overrides)
 *   media        library records WITHOUT transcript text, plus hash/path/size for resolution
 *   settings     non-local settings keys that do not belong to a group below
 *   cg           settings cg.*            spots        settings spots.*
 *   programming  settings recipes.*, traffic.*, clock.*, programming.*, hour_recipes
 * and a mediaManifest {id: {hash, path, size, title}} so that media references can be
 * resolved on another node by content hash. Local-only settings (tokens, bind address,
 * node identity, failover pairing, remote node list, mesh cookies, Twilio) are never bundled.
 * Credential settings the station itself needs (spots.llm API key, star.agents tokens, onvif.devices
 * passwords: isSecretSettingKey) are left out of every stored / exported / pushed session and out of
 * imported or received ones; only exportLive(includeSecrets) carries them, and the Supervisor offers that
 * to an authenticated failover peer only.
 *
 * Session ids are file names, so they are validated (isValidSessionId: 14-digit UTC stamp, '-', 4 hex);
 * anything else never reaches the file system.
 *
 * Sessions are stored as <dataDir>/sessions/<id>.json plus an index.json; every write to the
 * live station goes through ScheduleStore. applyBundle() with dryRun never writes.
 *
 * The same code path replicates a primary onto its backup: exportLive() builds a bundle of
 * the groups whose "marks" changed since the peer last asked; applyBundle() with
 * ApplyOptions{keepOutputs, keepArmed, mirror} writes it without touching the backup's hardware or arming.
 */
class SessionsManager : public QObject {
    Q_OBJECT
public:
    static constexpr const char* kFormat = "playplay-session/1";

    struct ApplyOptions {
        bool replace = false;        // schedule: delete local slots the bundle does not carry (per channel in the bundle)
        bool keepOutputs = true;     // channels: keep this node's `outputs` (hardware) unless groups contains "outputs"
        bool keepArmed = true;       // channels: keep this node's `armed` flag when the channel already exists
        bool keepLocalMediaPaths = true; // media: a local record with the same id and an existing file keeps its path
        bool mirror = false;         // media: keep the bundle's record ids (replication). false = foreign apply: a record whose content
                                     // hash is already held under another id is not created and its id is remapped
    };

    SessionsManager(sched::ScheduleStore* store, const QString& dataDir, QObject* parent = nullptr);
    ~SessionsManager() override;

    // --- groups + local settings policy -------------------------------------------
    static QStringList allGroups();                          // channels, schedule, media, settings, cg, spots, programming
    static bool isLocalSettingKey(const QString& key);       // never bundled, never applied
    static QString settingGroup(const QString& key);         // cg | spots | programming | settings | "" (local)
    static bool isSecretSettingKey(const QString& key);      // credentials: bundled only by exportLive(includeSecrets)
    static bool isValidSessionId(const QString& id);         // ^[0-9]{14}-[0-9a-f]{4}$ (what newId() makes); the only ids that map to a file

    // --- node identity ---------------------------------------------------------------
    QString nodeId();                                        // settings node.id (uuid, generated once)
    QString nodeName();                                      // settings node.name (default: machine host name)
    QJsonObject nodeInfo();                                  // {id, name, version}

    // --- live state ------------------------------------------------------------------
    /// Snapshot of the live station for `groups` (empty = all). Wire shape: see class comment.
    QJsonObject buildBundle(const QString& name, const QString& notes, const QStringList& groups = QStringList(), bool includeSecrets = false);
    /// Cheap per-group change marks: {"channels":"<n>:<digest>", "schedule":"<n>:<maxUpdatedAt>", ...}.
    QJsonObject marks();
    /// Replication export: bundle of the requested groups whose mark differs from `since` (peer's marks); always carries `marks`.
    QJsonObject exportLive(const QStringList& groups, const QJsonObject& since, bool includeSecrets = false);

    // --- registry ----------------------------------------------------------------------
    QJsonArray list() const;                                 // index rows (no data)
    QJsonObject get(const QString& id, bool withData, bool* found = nullptr) const;
    QJsonObject capture(const QString& name, const QString& notes);
    QJsonObject recapture(const QString& id, bool* found = nullptr);
    bool rename(const QString& id, const QString& name, const QString& notes, bool nameGiven, bool notesGiven);
    bool remove(const QString& id);
    QJsonObject clone(const QString& id, const QString& newName, bool* found = nullptr);
    /// Full session JSON for the wire / a download (index row + groups + manifest).
    QJsonObject exportBundle(const QString& id, bool* found = nullptr) const;
    /// Store a bundle from a file as a NEW session (new id). Accepts a bare bundle or the wire shape.
    QJsonObject importBundle(const QJsonObject& wire, const QString& name);
    /// A peer pushed a session: upsert by id, newer `version` wins; returns {action, id, version}. {error} for a bad id / shape.
    QJsonObject receiveBundle(const QJsonObject& wire);

    // --- apply -------------------------------------------------------------------------
    /// Write a bundle onto the live station. Report:
    /// {dryRun, groups, applied:{group:n}, unresolvedMedia:[{id,title,hash,path,size}], remapped:{srcId:localId}, warnings:[], errors:[], ok,
    ///  createdChannels:[ids new to this node], restartChannels:[ids whose frame format changed]}
    /// An error report (unknown group) has no `applied`: nothing was written.
    QJsonObject applyBundle(const QJsonObject& bundle, const QStringList& groups, bool dryRun, const ApplyOptions& opt = ApplyOptions());
    QJsonObject applySession(const QString& id, const QStringList& groups, bool dryRun, const ApplyOptions& opt, bool* found = nullptr);

    sched::ScheduleStore* store() const { return m_store; }
    QString sessionsDir() const { return m_dir; }

signals:
    void changed();

private:
    sched::ScheduleStore* m_store;
    QString m_dir;
    QList<QJsonObject> m_index;              // index rows: {id, name, notes, version, createdAt, updatedAt, node, summary, origin}
    mutable QSqlDatabase m_ro;               // second connection to the store's SQLite file, SELECT only (settings/slots enumeration + marks)
    QString m_roName;

    bool ensureRo() const;
    QList<QPair<QString, QString>> allSettings() const;        // key, value (every row)
    QJsonArray allSlotsJson(const QString& channelId) const;   // every hour slot of a channel (template + dated)
    QStringList slotIds(const QString& channelId) const;
    QString tableMark(const QString& table, const QString& timeCol) const;  // "<count>:<max time>"
    QString digestMark(const QString& table, const QString& jsonCol, const QString& orderCol) const;  // "<count>:<md5>"

    void loadIndex();
    bool saveIndex() const;
    /// Session file for `id`, or an empty path (every QFile / QFileInfo call on it fails) when the id is not a valid session id.
    QString filePath(const QString& id) const { return isValidSessionId(id) ? m_dir + "/" + id + ".json" : QString(); }
    int indexOf(const QString& id) const;
    bool writeSession(const QJsonObject& wire) const;
    QJsonObject readSession(const QString& id, bool* ok = nullptr) const;
    static QJsonObject rowOf(const QJsonObject& wire);         // index row from a wire object
    static QJsonObject summaryOf(const QJsonObject& bundle);
    static QString newId();
    static QJsonObject mediaForBundle(const QJsonObject& record);   // strips transcriptText
    static QJsonObject withoutSecrets(QJsonObject wire);            // drops isSecretSettingKey rows from the settings groups (+ fixes the summary)
};

#endif // SESSIONSMANAGER_H
