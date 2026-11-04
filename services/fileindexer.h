#ifndef FILEINDEXER_H
#define FILEINDEXER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QTimer>
#include "core/types.h"

class FileIndexer : public QObject
{
    Q_OBJECT

public:
    explicit FileIndexer(QObject* parent = nullptr);

    void startIndexing(const QString& directory);
    void stopIndexing();
    bool isIndexing() const;

signals:
    void indexingStarted(const QString& directory);
    void indexingProgress(int current, int total, const QString& currentFile);
    void indexingFinished(const QString& directory, int indexedCount);
    void newMediaFound(const MediaItem& mediaItem);
    void error(const QString& message);

private slots:
    void processNextFile();

private:
    QList<QString> findMediaFiles(const QString& directory);
    MediaItem extractMediaMetadata(const QString& filePath);

    QString m_currentDirectory;
    bool m_isIndexing;
    QList<QString> m_filesToIndex;
    int m_indexedCount;
    QStringList m_mediaExtensions;
};

#endif // FILEINDEXER_H

