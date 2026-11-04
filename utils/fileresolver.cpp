#include "fileresolver.h"
#include <QFileInfo>
#include <QDir>

QString FileResolver::resolveFileUrl(const QString& filePath)
{
    if (filePath.isEmpty()) {
        return QString();
    }
    
    QFileInfo info(filePath);
    if (info.exists()) {
        return QDir::toNativeSeparators(info.absoluteFilePath());
    }
    
    return filePath;
}

QUrl FileResolver::resolveMediaUrl(const QString& filePath)
{
    QString resolved = resolveFileUrl(filePath);
    if (resolved.isEmpty()) {
        return QUrl();
    }
    
    return QUrl::fromLocalFile(resolved);
}

