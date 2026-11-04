#include "drivewatcher.h"
#include <QDir>
#include <QFileInfo>
#include <QDebug>

DriveWatcher::DriveWatcher(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &DriveWatcher::onFileChanged);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &DriveWatcher::onDirectoryChanged);
}

DriveWatcher::~DriveWatcher()
{
    // QFileSystemWatcher is parented, will be deleted automatically
}

bool DriveWatcher::watchPath(const QString& path, const DriveWatchConfig& config)
{
    QFileInfo info(path);
    if (!info.exists()) {
        qWarning() << "Path does not exist:" << path;
        return false;
    }
    
    if (m_configs.contains(path)) {
        qWarning() << "Path already being watched:" << path;
        return false;
    }
    
    m_configs[path] = config;
    
    if (info.isDir()) {
        if (config.watchDirs) {
            m_watcher->addPath(path);
            m_pathToWatchedPath[path] = path;
        }
        
        if (config.recursive) {
            addDirectoryRecursive(path, path, config);
        }
    } else if (info.isFile() && config.watchFiles) {
        m_watcher->addPath(path);
        m_pathToWatchedPath[path] = path;
    }
    
    return true;
}

void DriveWatcher::unwatchPath(const QString& path)
{
    if (!m_configs.contains(path)) {
        return;
    }
    
    // Remove all paths associated with this watched path
    QStringList pathsToRemove;
    for (auto it = m_pathToWatchedPath.begin(); it != m_pathToWatchedPath.end(); ++it) {
        if (it.value() == path) {
            pathsToRemove.append(it.key());
        }
    }
    
    for (const QString& p : pathsToRemove) {
        m_watcher->removePath(p);
        m_pathToWatchedPath.remove(p);
    }
    
    m_configs.remove(path);
}

QStringList DriveWatcher::getWatchedPaths() const
{
    return m_configs.keys();
}

bool DriveWatcher::isWatching(const QString& path) const
{
    return m_configs.contains(path);
}

void DriveWatcher::onFileChanged(const QString& path)
{
    QString watchedPath = m_pathToWatchedPath.value(path);
    if (watchedPath.isEmpty()) {
        return;
    }
    
    DriveWatchConfig config = m_configs.value(watchedPath);
    if (!shouldProcessPath(path, config)) {
        return;
    }
    
    emit fileChanged(path, watchedPath);
}

void DriveWatcher::onDirectoryChanged(const QString& path)
{
    QString watchedPath = m_pathToWatchedPath.value(path);
    if (watchedPath.isEmpty()) {
        return;
    }
    
    DriveWatchConfig config = m_configs.value(watchedPath);
    if (!shouldProcessPath(path, config)) {
        return;
    }
    
    emit directoryChanged(path, watchedPath);
    
    // Check for new files in directory
    if (config.watchFiles) {
        QDir dir(path);
        QStringList files = dir.entryList(QDir::Files);
        for (const QString& file : files) {
            QString filePath = dir.absoluteFilePath(file);
            if (!m_pathToWatchedPath.contains(filePath)) {
                m_watcher->addPath(filePath);
                m_pathToWatchedPath[filePath] = watchedPath;
                emit fileAdded(filePath, watchedPath);
            }
        }
    }
}

bool DriveWatcher::shouldProcessPath(const QString& filePath, const DriveWatchConfig& config)
{
    // Check exclude patterns
    for (const QString& pattern : config.excludePatterns) {
        if (filePath.contains(pattern)) {
            return false;
        }
    }
    
    // Check include patterns (if any specified)
    if (!config.includePatterns.isEmpty()) {
        bool matches = false;
        for (const QString& pattern : config.includePatterns) {
            if (filePath.contains(pattern)) {
                matches = true;
                break;
            }
        }
        if (!matches) {
            return false;
        }
    }
    
    return true;
}

void DriveWatcher::addDirectoryRecursive(const QString& dirPath, const QString& watchedPath, const DriveWatchConfig& config)
{
    QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }
    
    if (config.watchDirs) {
        m_watcher->addPath(dirPath);
        m_pathToWatchedPath[dirPath] = watchedPath;
    }
    
    if (config.watchFiles) {
        QStringList files = dir.entryList(QDir::Files);
        for (const QString& file : files) {
            QString filePath = dir.absoluteFilePath(file);
            if (shouldProcessPath(filePath, config)) {
                m_watcher->addPath(filePath);
                m_pathToWatchedPath[filePath] = watchedPath;
            }
        }
    }
    
    if (config.recursive) {
        QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& subdir : subdirs) {
            QString subdirPath = dir.absoluteFilePath(subdir);
            addDirectoryRecursive(subdirPath, watchedPath, config);
        }
    }
}

