#include "mediadetection.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QDebug>

const QStringList MediaDetection::ID_PROMO_KEYWORDS = {
    "id", "star news", "wgsr", "wmdv", "wyat", "promo", "legal", "station", "network"
};

MediaDetection::MediaDetection(QObject* parent)
    : QObject(parent)
{
}

MediaType MediaDetection::detectMediaType(const QString& filename, double durationSeconds)
{
    QString lowerFilename = filename.toLower();
    
    // Check for ID/promo keywords first
    for (const QString& keyword : ID_PROMO_KEYWORDS) {
        if (lowerFilename.contains(keyword)) {
            // Distinguish between ID and promo
            if (keyword == "id" || keyword == "legal" || keyword == "station" || keyword == "network") {
                return MediaType::StationId;
            }
            if (keyword == "promo") {
                return MediaType::Promo;
            }
            // Station call signs are usually IDs
            if (lowerFilename.contains("wgsr") || lowerFilename.contains("wmdv") || 
                lowerFilename.contains("wyat") || lowerFilename.contains("star news")) {
                return MediaType::StationId;
            }
        }
    }
    
    // Check for bumper keywords
    if (lowerFilename.contains("bumper") || lowerFilename.contains("bump")) {
        return MediaType::Bumper;
    }
    
    // Duration-based detection
    if (durationSeconds > 0) {
        // Under 3 minutes (180 seconds) → likely an ad
        if (durationSeconds < 180) {
            return MediaType::Ad;
        }
        // Under 5 minutes → could be a promo or bumper
        if (durationSeconds < 300) {
            return MediaType::Promo;
        }
    }
    
    // Default to program for longer content
    return MediaType::Program;
}

QString MediaDetection::detectClientFromFilename(const QString& filename, const QJsonArray& clients)
{
    QString lowerFilename = filename.toLower();
    
    // Try to match client names
    for (const QJsonValue& clientVal : clients) {
        QJsonObject client = clientVal.toObject();
        QString clientName = client["name"].toString().toLower();
        
        // Exact match or contained in filename
        if (lowerFilename.contains(clientName)) {
            return client["id"].toString();
        }
        
        // Try matching without spaces/special chars
        QString normalizedClient = clientName;
        normalizedClient.remove(QRegularExpression("[^a-z0-9]"));
        QString normalizedFilename = lowerFilename;
        normalizedFilename.remove(QRegularExpression("[^a-z0-9]"));
        
        if (normalizedFilename.contains(normalizedClient) && normalizedClient.length() >= 3) {
            return client["id"].toString();
        }
    }
    
    return QString();
}

BumperSubtype MediaDetection::detectBumperSubtype(const QString& filename)
{
    QString lower = filename.toLower();
    
    if (lower.contains("breaking") || lower.contains("alert") || lower.contains("urgent")) {
        return BumperSubtype::Breaking;
    }
    
    if (lower.contains("program") || lower.contains("show") || lower.contains("series")) {
        return BumperSubtype::Program;
    }
    
    return BumperSubtype::Generic;
}

QString MediaDetection::detectProgramSeries(const QString& filename)
{
    // Try to extract series name from filename
    // Common patterns: "Series Name - Episode Title", "SeriesName_Episode", etc.
    QRegularExpression seriesPattern(R"((.+?)[\s_-]+(?:ep|episode|pt|part)\s*\d+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = seriesPattern.match(filename);
    
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }
    
    // Try pattern: "Series Name S01E01"
    QRegularExpression seasonPattern(R"((.+?)\s+[sS]\d+[eE]\d+)", QRegularExpression::CaseInsensitiveOption);
    match = seasonPattern.match(filename);
    
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }
    
    return QString();
}

QString MediaDetection::mediaTypeToString(MediaType type)
{
    switch (type) {
        case MediaType::Video: return "video";
        case MediaType::Image: return "image";
        case MediaType::Live: return "live";
        case MediaType::StationId: return "station_id";
        case MediaType::Bumper: return "bumper";
        case MediaType::Program: return "program";
        case MediaType::PaidProgram: return "paid_program";
        case MediaType::Ad: return "ad";
        case MediaType::Promo: return "promo";
        case MediaType::Gap: return "gap";
        default: return "video";
    }
}

MediaType MediaDetection::stringToMediaType(const QString& str)
{
    QString lower = str.toLower();
    if (lower == "video") return MediaType::Video;
    if (lower == "image") return MediaType::Image;
    if (lower == "live") return MediaType::Live;
    if (lower == "station_id") return MediaType::StationId;
    if (lower == "bumper") return MediaType::Bumper;
    if (lower == "program") return MediaType::Program;
    if (lower == "paid_program") return MediaType::PaidProgram;
    if (lower == "ad") return MediaType::Ad;
    if (lower == "promo") return MediaType::Promo;
    if (lower == "gap") return MediaType::Gap;
    return MediaType::Video;
}

bool MediaDetection::containsKeyword(const QString& filename, const QStringList& keywords)
{
    QString lower = filename.toLower();
    for (const QString& keyword : keywords) {
        if (lower.contains(keyword)) {
            return true;
        }
    }
    return false;
}

