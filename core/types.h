#ifndef TYPES_H
#define TYPES_H

#include <QString>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QVector>
#include <QMap>
#include <QHash>
#include <QUrl>
#include <optional>
#include <unordered_map>
#include <memory>

// ============================================
// CORE TYPE DEFINITIONS
// Ported from TypeScript types
// ============================================

enum class MediaType {
    Video,
    Audio, // NEW
    Image,
    Live,
    StationId,
    Bumper,
    Program,
    PaidProgram,
    Ad,
    Promo,
    Gap
};

enum class BumperSubtype {
    Generic,
    Breaking,
    Program
};

enum class LiveSourceKind {
    GStreamer,
    NDI,
    Capture,
    URL,
    OBSScene
};

struct Segment {
    QString id;
    QString label;
    double startTime; // seconds
    double endTime;   // seconds
    QString color;
};

struct LiveSourceConfig {
    LiveSourceKind kind;
    QString name;
    QString ndiName;
    QString device;
    QUrl url;
    QString obsScene;
    QString pipeline;
};

struct MediaItem {
    QString id;
    QString title;
    double duration; // seconds
    QUrl src;
    MediaType type;
    QString thumbnail;
    QString filePath;
    QString filename;
    QString client; // For ads
    QString programSeries;
    QDate episodeDate;
    QString notes;
    QList<Segment> segments;
    std::optional<LiveSourceConfig> liveSource;
    BumperSubtype bumperSubtype;
    QString bumperProgramId;
    QDateTime addedAt;
    QDateTime dateMade;
    QDateTime lastPlayedAt;
    int playCount;
    int priority;
    bool archived;
    
    // Serialization
    QJsonObject toJson() const;
    static MediaItem fromJson(const QJsonObject& json);

    bool operator==(const MediaItem& other) const {
        return id == other.id;
    }
};

struct PlaylistItem {
    QString id;
    QString mediaId;
    MediaItem media;
    double timeIn;  // seconds
    double timeOut; // seconds
    QDateTime startTime; // Absolute time
    QDateTime endTime;
    bool isFixedTime;
    QDateTime fixedTime;
    bool isResolved;
    QStringList obsCommands;
    QStringList cgOnStart;
    QStringList cgOnEnd;
    QJsonObject actions; // Automation actions
    int priority; // For block scheduling
    
    QJsonObject toJson() const;
    static PlaylistItem fromJson(const QJsonObject& json);

    bool operator==(const PlaylistItem& other) const {
        return id == other.id;
    }
};

struct HourSlot {
    QString id; // "day-hour" format
    int dayOfWeek; // 0-6
    int hour;      // 0-23
    QList<PlaylistItem> items;
    bool isLooping;
    bool randomizeAds;
    bool keepProgramOrder;
    QDateTime updatedAt;
    
    // Default constructor for containers
    HourSlot() : dayOfWeek(0), hour(0), isLooping(false), randomizeAds(true), keepProgramOrder(true) {}
    
    // Copy constructor
    HourSlot(const HourSlot& other) = default;
    
    // Assignment operator
    HourSlot& operator=(const HourSlot& other) = default;
};

// Simple pair structure
struct HourSlotPair {
    QString key;
    HourSlot value;
};

struct WeeklySchedule {
    // Use QVector and provide explicit template instantiation in .cpp file
    QStringList slotKeys; QList<HourSlot> slotValues;
    QDateTime lastModified;
    
    // Helper methods for map-like access
    HourSlot* findSlot(const QString& key);
    const HourSlot* findSlot(const QString& key) const;
    HourSlot& operator[](const QString& key);
    bool contains(const QString& key) const;
    void clear();
    bool empty() const;
    
    QJsonObject toJson() const;
    static WeeklySchedule fromJson(const QJsonObject& json);
};

struct AdClient {
    QString id;
    QString name;
    QString color;
    QString contactName;
    QString contactEmail;
    QString contactPhone;
    QString address;
    QString website;
    QDate contractStart;
    QDate contractEnd;
    double budget;
    double ratePerPlay;
    QString billingCode;
    QString terms;
    QJsonObject rules; // Scheduling rules
    QString status; // "active" | "paused" | "cancelled"
    QString notes;
    
    QJsonObject toJson() const;
    static AdClient fromJson(const QJsonObject& json);
};

struct Campaign {
    QString id;
    QString name;
    QString client;
    QDate startDate;
    QDate endDate;
    int targetPlays;
    int currentPlays;
    QString billingCode;
    QStringList daypartRestrictions;
    int separationMinutes;
    QStringList competitiveExclusions;
    
    QJsonObject toJson() const;
    static Campaign fromJson(const QJsonObject& json);
};

struct PlayEvent {
    qint64 timestamp;      // Unix timestamp when play started
    double duration;       // Actual duration played (seconds)
    bool completed;        // Did it play to completion?
    QString hourSlot;      // Which hour slot it was in
    QString itemId;        // Playlist item id
    QString mediaTitle;    // Title at time of play
    qint64 scheduledStart; // Planned start (ms)
    qint64 actualStart;    // Actual start (ms)
    qint64 actualEnd;      // Actual end (ms)
    int playlistIndex;     // Position in hour
    QString source;         // playout/scheduler/etc
    
    QJsonObject toJson() const;
    static PlayEvent fromJson(const QJsonObject& json);
};

struct MediaStats {
    int playCount;
    double totalPlayTime;  // Total seconds played
    qint64 lastPlayed;     // Unix timestamp
    QList<PlayEvent> playHistory;
    
    QJsonObject toJson() const;
    static MediaStats fromJson(const QJsonObject& json);
};

struct AsPlayedEntry : public PlayEvent {
    QString mediaId;
    
    QJsonObject toJson() const;
    static AsPlayedEntry fromJson(const QJsonObject& json);
};

struct FreePlaylist {
    QString id;
    QString name;
    QList<PlaylistItem> items;
    bool isLocked;         // If false, can play anytime (not time-bound)
    qint64 createdAt;
    QString createdFrom;   // Source hour slot ID if exported
    
    QJsonObject toJson() const;
    static FreePlaylist fromJson(const QJsonObject& json);
};

struct AutomationSettings {
    bool alwaysPlayLatestEpisode;
    bool shuffleAdsInHour;
    bool fillGapsWithIds;
    bool fillGapsWithBumpers;
    int maxFillDuration;
    bool autoAdvance;
    bool loopAtEndOfSchedule;
    QString defaultCGAnimationIn;
    QString defaultCGAnimationOut;
    int defaultCGDuration;
    bool trackPlayHistory;
    int maxHistoryDays;
    QString facebookWebhookUrl;
    bool facebookWebhookEnabled;
    bool facebookGraphEnabled;
    QString facebookGraphPageId;
    QString facebookGraphAccessToken;
    bool facebookPreferGraph;
    QString openaiApiKey;
    QString youtubeAccessToken;
    QString salesReportingEndpoint;
    
    QJsonObject toJson() const;
    static AutomationSettings fromJson(const QJsonObject& json);
};

// Helper functions
QString mediaTypeToString(MediaType type);
MediaType stringToMediaType(const QString& str);
QString bumperSubtypeToString(BumperSubtype subtype);
BumperSubtype stringToBumperSubtype(const QString& str);

#endif // TYPES_H





