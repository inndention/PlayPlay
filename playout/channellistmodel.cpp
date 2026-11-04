#include "channellistmodel.h"
#include <QSet>

ChannelListModel::ChannelListModel(QObject* parent) : QAbstractListModel(parent) {}

int ChannelListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant ChannelListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size() || role != ChannelDataRole) return {};
    return m_rows[index.row()].variant();
}

QHash<int, QByteArray> ChannelListModel::roleNames() const { return {{ChannelDataRole, QByteArrayLiteral("channelData")}}; }

int ChannelListModel::indexOf(const QString& id) const {
    for (int i = 0; i < m_rows.size(); ++i) if (m_rows[i].id == id) return i;
    return -1;
}

QStringList ChannelListModel::ids() const {
    QStringList l; l.reserve(m_rows.size());
    for (const Row& r : m_rows) l << r.id;
    return l;
}

QJsonObject ChannelListModel::channelJson(const QString& id) const { const int i = indexOf(id); return i < 0 ? QJsonObject() : m_rows[i].json; }
QVariantMap ChannelListModel::channel(const QString& id) const { const int i = indexOf(id); return i < 0 ? QVariantMap() : m_rows[i].variant(); }
QVariantMap ChannelListModel::get(int row) const { return row < 0 || row >= m_rows.size() ? QVariantMap() : m_rows[row].variant(); }

QVariantList ChannelListModel::toVariantList() const {
    if (!m_listValid) {
        m_list.clear(); m_list.reserve(m_rows.size());
        for (const Row& r : m_rows) m_list << QVariant(r.variant());   // implicitly shared: no second conversion
        m_listValid = true;
    }
    return m_list;
}

int ChannelListModel::diff(const QJsonObject& a, const QJsonObject& b) {
    if (a == b) return 0;
    static const QLatin1String kProfile("profile"), kProcess("process"), kExecutor("executor"), kEngine("engine"), kEngineState("engineState");
    int bits = 0;
    if (a.value(kProfile) != b.value(kProfile)) bits |= Profile;
    if (a.value(kProcess) != b.value(kProcess)) bits |= Process;
    if (a.value(kExecutor) != b.value(kExecutor)) bits |= Executor;
    if (a.value(kEngine) != b.value(kEngine) || a.value(kEngineState) != b.value(kEngineState)) bits |= Engine;
    // Anything else (exists, hasPreview, keys a later Supervisor adds): one more pass over both key sets.
    auto other = [&](const QJsonObject& x, const QJsonObject& y) {
        for (auto it = x.begin(); it != x.end(); ++it) {
            const QString& k = it.key();
            if (k == kProfile || k == kProcess || k == kExecutor || k == kEngine || k == kEngineState) continue;
            if (it.value() != y.value(k)) return true;
        }
        return false;
    };
    if (other(a, b) || other(b, a)) bits |= Other;
    return bits ? bits : Other;   // a == b said they differ somewhere
}

int ChannelListModel::update(int row, const QJsonObject& json) {
    Row& r = m_rows[row];
    const int bits = diff(r.json, json);
    if (!bits) return 0;
    r.json = json; r.mapValid = false; r.map.clear();
    m_listValid = false;
    ++m_stats.updated;
    const QModelIndex ix = index(row);
    emit dataChanged(ix, ix, {ChannelDataRole});
    return bits;
}

void ChannelListModel::resetTo(const QJsonArray& channels, QHash<QString, int>* changes) {
    // Only for lists whose ids repeat: rows cannot be matched one to one, so the delegates are rebuilt once.
    beginResetModel();
    for (const Row& r : m_rows) (*changes)[r.id] |= Removed;
    m_rows.clear();
    for (const QJsonValue& v : channels) { m_rows.append(makeRow(v.toObject())); (*changes)[m_rows.last().id] |= Inserted; }
    m_listValid = false;
    endResetModel();
    ++m_stats.resets;
}

QHash<QString, int> ChannelListModel::sync(const QJsonArray& channels) {
    ++m_stats.syncs;
    QHash<QString, int> changes;
    const int before = count();
    QStringList want; want.reserve(channels.size());
    QSet<QString> wantSet;
    for (const QJsonValue& v : channels) { const QString id = idOf(v.toObject()); want << id; wantSet.insert(id); }
    QSet<QString> haveSet;
    for (const Row& r : m_rows) haveSet.insert(r.id);
    if (wantSet.size() != want.size() || haveSet.size() != m_rows.size()) {
        resetTo(channels, &changes);
    } else {
        // 1. Rows whose channel is gone, bottom up.
        for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
            if (wantSet.contains(m_rows[i].id)) continue;
            // m_listValid drops before each end*Rows(): their signals go out synchronously, and a handler reading
            // supervisor.channels there must not get the cached list from before the change.
            beginRemoveRows(QModelIndex(), i, i);
            changes[m_rows[i].id] |= Removed;
            m_rows.removeAt(i);
            m_listValid = false;
            endRemoveRows();
            ++m_stats.removed;
        }
        // 2. Walk the wanted order: keep, move up from further down, or insert; then update the row in place.
        for (int i = 0; i < want.size(); ++i) {
            const QString& id = want[i];
            const QJsonObject json = channels[i].toObject();
            if (i < m_rows.size() && m_rows[i].id == id) {
                if (const int bits = update(i, json)) changes[id] |= bits;
                continue;
            }
            int j = -1;
            for (int k = i + 1; k < m_rows.size(); ++k) if (m_rows[k].id == id) { j = k; break; }
            if (j >= 0) {
                beginMoveRows(QModelIndex(), j, j, QModelIndex(), i);
                m_rows.move(j, i);
                m_listValid = false;
                endMoveRows();
                ++m_stats.moved;
                changes[id] |= Moved;
                if (const int bits = update(i, json)) changes[id] |= bits;
            } else {
                beginInsertRows(QModelIndex(), i, i);
                m_rows.insert(i, makeRow(json));
                m_listValid = false;
                endInsertRows();
                ++m_stats.inserted;
                changes[id] |= Inserted;
            }
        }
    }
    if (changes.isEmpty()) ++m_stats.unchanged;
    if (count() != before) emit countChanged();
    return changes;
}

int ChannelListModel::upsert(const QJsonObject& channel) {
    const QString id = idOf(channel);
    const int i = indexOf(id);
    if (i >= 0) return update(i, channel);
    const int at = count();
    beginInsertRows(QModelIndex(), at, at);
    m_rows.append(makeRow(channel));
    m_listValid = false;
    endInsertRows();
    ++m_stats.inserted;
    emit countChanged();
    return Inserted;
}
