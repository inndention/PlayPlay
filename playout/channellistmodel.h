#ifndef CHANNELLISTMODEL_H
#define CHANNELLISTMODEL_H

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

/**
 * ChannelListModel — the Supervisor snapshot's channel list as a QML list model (perf-app PA-3, W10).
 *
 * `model: supervisor.channels` was a new JS array on every snapshot (every 2 s, plus the 4 s poll): each Repeater /
 * ListView over it destroyed and rebuilt every delegate, the ProgramImage tiles of the On Air bank included. This model
 * keeps one row per channel, matched by profile.id, and updates it in place:
 *   - an identical list changes nothing (no signal at all);
 *   - a changed channel is one dataChanged for its row;
 *   - added / removed / reordered channels are row inserts / removes / moves.
 * It never resets, except for a list with duplicate ids (rows cannot be matched one to one; counted in stats()).
 *
 * One role, "channelData" (the channel object: profile, process, executor, engine, engineState, hasPreview ...), so the
 * delegates written for the JS array keep reading `modelData` (QML hands a one-role model's value to delegates as
 * modelData). Each row's QVariantMap is converted once per change and cached. `count` and `length` (the same number)
 * let `channels.length` bindings work unchanged when they get this model instead of the array.
 * GUI thread only.
 */
class ChannelListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int length READ count NOTIFY countChanged)

public:
    enum Roles { ChannelDataRole = Qt::UserRole + 1 };
    // What changed in one channel: bits of the value sync() / upsert() report per id.
    enum Part { Inserted = 1, Removed = 2, Moved = 4, Profile = 8, Process = 16, Executor = 32, Engine = 64, Other = 128 };

    struct Stats { quint64 syncs = 0, unchanged = 0, resets = 0, inserted = 0, removed = 0, moved = 0, updated = 0; };

    explicit ChannelListModel(QObject* parent = nullptr);

    // The whole list (a snapshot's "channels"). Returns id -> Part bits for every channel that changed (empty: nothing did).
    QHash<QString, int> sync(const QJsonArray& channels);
    // One channel (a single-channel MSG_UI_STATUS): updated in place, appended when new. Returns its Part bits (0: unchanged).
    int upsert(const QJsonObject& channel);

    int count() const { return int(m_rows.size()); }
    int indexOf(const QString& id) const;
    QStringList ids() const;
    QJsonObject channelJson(const QString& id) const;
    QVariantMap channel(const QString& id) const;   // cached conversion; {} when unknown
    QVariantList toVariantList() const;             // every row's cached map (the old `channels` list), cached
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOfId(const QString& id) const { return indexOf(id); }
    const Stats& stats() const { return m_stats; }

    static QString idOf(const QJsonObject& channel) { return channel.value(QLatin1String("profile")).toObject().value(QLatin1String("id")).toString(); }
    static int diff(const QJsonObject& before, const QJsonObject& after);   // Part bits (Profile .. Other), 0 when equal

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    struct Row {
        QString id;
        QJsonObject json;
        mutable QVariantMap map;
        mutable bool mapValid = false;
        const QVariantMap& variant() const { if (!mapValid) { map = json.toVariantMap(); mapValid = true; } return map; }
    };
    QList<Row> m_rows;
    mutable QVariantList m_list;
    mutable bool m_listValid = false;
    Stats m_stats;

    static Row makeRow(const QJsonObject& json) { Row r; r.id = idOf(json); r.json = json; return r; }
    int update(int row, const QJsonObject& json);   // Part bits; dataChanged when non-zero
    void resetTo(const QJsonArray& channels, QHash<QString, int>* changes);
};

#endif // CHANNELLISTMODEL_H
