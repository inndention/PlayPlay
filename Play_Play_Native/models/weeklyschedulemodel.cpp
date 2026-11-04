#include "weeklyschedulemodel.h"
#include <QDebug>

WeeklyScheduleModel::WeeklyScheduleModel(QObject* parent)
    : QAbstractTableModel(parent)
    , m_mediaDb(new MediaDb())
{
    m_mediaDb->initialize();
    loadSchedule();
}

int WeeklyScheduleModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return 24; // 00:00 to 23:00
}

int WeeklyScheduleModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return 7; // Monday to Sunday
}

QVariant WeeklyScheduleModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    int hour = index.row();
    int day = index.column();

    // Construct ID: "day-hour" (e.g. "0-14")
    QString slotId = QString("%1-%2").arg(day).arg(hour);
    
    // Check cache
    const HourSlot* slot = nullptr;
    // We need to find the slot in the cache values list or map
    // Since WeeklySchedule uses lists, we iterate or use a helper if available.
    // However, our types.h defined WeeklySchedule struct has helper: findSlot
    
    // Since m_scheduleCache is not a pointer, we need to implement look up.
    // We can't call const method on it if findSlot is not const. 
    // Let's iterate manually or optimize later.
    for (const auto& s : m_scheduleCache.slotValues) {
        if (s.id == slotId) {
            slot = &s;
            break;
        }
    }

    if (!slot) {
        // Return default "Empty" values
        if (role == StatusRole) return Empty;
        if (role == ItemCountRole) return 0;
        if (role == DurationRole) return 0.0;
        if (role == SlotIdRole) return slotId;
        if (role == IsLoopingRole) return false;
        return QVariant();
    }

    switch (role) {
    case StatusRole:
        return calculateStatus(*slot);
    case ItemCountRole:
        return slot->items.size();
    case DurationRole:
        return calculateDuration(*slot);
    case SlotIdRole:
        return slot->id;
    case IsLoopingRole:
        return slot->isLooping;
    case ColorRole: // Color based on status
        {
            SlotStatus status = calculateStatus(*slot);
            if (status == Empty) return "#2D2D30"; // Dark Grey
            if (status == Under) return "#D4AF37"; // Yellow/Gold warning
            if (status == Full) return "#2E7D32"; // Green success
            if (status == Over) return "#C62828";  // Red warning
            return "#2D2D30";
        }
    }

    return QVariant();
}

QHash<int, QByteArray> WeeklyScheduleModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[StatusRole] = "status";
    roles[ItemCountRole] = "itemCount";
    roles[DurationRole] = "duration";
    roles[SlotIdRole] = "slotId";
    roles[IsLoopingRole] = "isLooping";
    roles[ColorRole] = "color";
    return roles;
}

QVariant WeeklyScheduleModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Horizontal) {
        return getDayName(section);
    } else {
        return QString("%1:00").arg(section, 2, 10, QChar('0'));
    }
}

void WeeklyScheduleModel::loadSchedule()
{
    beginResetModel();
    m_scheduleCache = m_mediaDb->getAllSlots();
    endResetModel();
}

void WeeklyScheduleModel::copyDay(int sourceDay, int destDay)
{
    if (sourceDay < 0 || sourceDay > 6 || destDay < 0 || destDay > 6 || sourceDay == destDay)
        return;

    // Strategy:
    // 1. Get all slots for sourceDay
    // 2. Clear all slots for destDay in DB
    // 3. Create new slots for destDay with copied content
    
    beginResetModel(); // Full reset for simplicity
    
    // Find source slots
    QList<HourSlot> sourceSlots;
    for (const auto& s : m_scheduleCache.slotValues) {
        if (s.dayOfWeek == sourceDay) {
            sourceSlots.append(s);
        }
    }
    
    // Save as dest slots
    for (auto slot : sourceSlots) {
        slot.dayOfWeek = destDay;
        slot.id = QString("%1-%2").arg(destDay).arg(slot.hour); // New ID
        m_mediaDb->saveScheduleSlot(slot);
    }
    
    m_scheduleCache = m_mediaDb->getAllSlots(); // Reload
    endResetModel();
}

void WeeklyScheduleModel::clearDay(int day)
{
    beginResetModel();
    for (int h = 0; h < 24; ++h) {
        QString id = QString("%1-%2").arg(day).arg(h);
        m_mediaDb->deleteScheduleSlot(id);
    }
    m_scheduleCache = m_mediaDb->getAllSlots();
    endResetModel();
}

bool WeeklyScheduleModel::hasData(int day, int hour) const
{
    QString id = QString("%1-%2").arg(day).arg(hour);
    for (const auto& s : m_scheduleCache.slotValues) {
        if (s.id == id) return true;
    }
    return false;
}

QString WeeklyScheduleModel::getDayName(int dayIndex) const
{
    switch (dayIndex) {
        case 0: return "Monday";
        case 1: return "Tuesday";
        case 2: return "Wednesday";
        case 3: return "Thursday";
        case 4: return "Friday";
        case 5: return "Saturday";
        case 6: return "Sunday";
        default: return "";
    }
}

WeeklyScheduleModel::SlotStatus WeeklyScheduleModel::calculateStatus(const HourSlot& slot) const
{
    if (slot.items.isEmpty()) return Empty;
    
    double totalDuration = calculateDuration(slot);
    
    // Tolerance: 59m to 61m considered "Full"
    // 3600s = 1h
    if (totalDuration < 3540) return Under; // < 59m
    if (totalDuration > 3660) return Over;  // > 61m
    return Full;
}

double WeeklyScheduleModel::calculateDuration(const HourSlot& slot) const
{
    double total = 0;
    for (const auto& item : slot.items) {
        total += item.media.duration; // Use media duration
        // Ideally should account for timeIn/Out but keeping simple for now
    }
    return total;
}
