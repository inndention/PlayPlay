#ifndef FILERESOLVER_H
#define FILERESOLVER_H

#include <QString>
#include <QUrl>

class FileResolver
{
public:
    static QString resolveFileUrl(const QString& filePath);
    static QUrl resolveMediaUrl(const QString& filePath);
};

#endif // FILERESOLVER_H

