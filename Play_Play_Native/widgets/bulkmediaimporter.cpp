#include "bulkmediaimporter.h"
#include "services/metadataservice.h"
#include "database/mediadb.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QDebug>
#include <QApplication>
#include <QDateTime>
#include <QtConcurrent>

BulkMediaImporter::BulkMediaImporter(QWidget* parent)
    : QWidget(parent)
    , m_selectedType(MediaType::Video)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    // Type selector
    QHBoxLayout* typeLayout = new QHBoxLayout();
    typeLayout->addWidget(new QLabel("Media Type:"));
    m_typeCombo = new QComboBox();
    m_typeCombo->addItem("Video", static_cast<int>(MediaType::Video));
    m_typeCombo->addItem("Image", static_cast<int>(MediaType::Image));
    m_typeCombo->addItem("Ad", static_cast<int>(MediaType::Ad));
    m_typeCombo->addItem("Program", static_cast<int>(MediaType::Program));
    m_typeCombo->addItem("Station ID", static_cast<int>(MediaType::StationId));
    m_typeCombo->addItem("Bumper", static_cast<int>(MediaType::Bumper));
    m_typeCombo->addItem("Promo", static_cast<int>(MediaType::Promo));
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &BulkMediaImporter::onTypeChanged);
    typeLayout->addWidget(m_typeCombo);
    typeLayout->addStretch();
    layout->addLayout(typeLayout);
    
    // File selection buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    m_selectFilesButton = new QPushButton("Select Files...");
    m_selectFolderButton = new QPushButton("Select Folder...");
    connect(m_selectFilesButton, &QPushButton::clicked, this, &BulkMediaImporter::onSelectFiles);
    connect(m_selectFolderButton, &QPushButton::clicked, this, &BulkMediaImporter::onSelectFolder);
    buttonLayout->addWidget(m_selectFilesButton);
    buttonLayout->addWidget(m_selectFolderButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);
    
    // File list
    m_fileList = new QListWidget();
    layout->addWidget(m_fileList);
    
    // Progress
    m_progressBar = new QProgressBar();
    m_progressBar->setVisible(false);
    layout->addWidget(m_progressBar);
    
    // Status
    m_statusLabel = new QLabel("Ready");
    layout->addWidget(m_statusLabel);
    
    // Action buttons
    QHBoxLayout* actionLayout = new QHBoxLayout();
    m_importButton = new QPushButton("Import");
    m_cancelButton = new QPushButton("Cancel");
    connect(m_importButton, &QPushButton::clicked, this, &BulkMediaImporter::onImport);
    connect(m_cancelButton, &QPushButton::clicked, this, &BulkMediaImporter::onCancel);
    actionLayout->addStretch();
    actionLayout->addWidget(m_importButton);
    actionLayout->addWidget(m_cancelButton);
    layout->addLayout(actionLayout);
}

void BulkMediaImporter::onSelectFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        "Select Media Files",
        "",
        "Media Files (*.mp4 *.mov *.avi *.mkv *.mp3 *.wav *.jpg *.jpeg *.png *.gif)"
    );
    
    if (!files.isEmpty()) {
        m_selectedFiles = files;
        m_fileList->clear();
        for (const QString& file : files) {
            m_fileList->addItem(QFileInfo(file).fileName());
        }
        m_statusLabel->setText(QString("%1 file(s) selected").arg(files.size()));
    }
}

void BulkMediaImporter::onSelectFolder()
{
    QString folder = QFileDialog::getExistingDirectory(this, "Select Folder");
    if (!folder.isEmpty()) {
        FolderScanner scanner;
        QFuture<QList<ScannedFile>> future = scanner.scanFolder(folder);
        QList<ScannedFile> files = future.result();
        
        m_selectedFiles.clear();
        for (const ScannedFile& file : files) {
            m_selectedFiles.append(file.path);
        }
        
        m_fileList->clear();
        for (const ScannedFile& file : files) {
            m_fileList->addItem(file.name);
        }
        m_statusLabel->setText(QString("%1 file(s) found").arg(files.size()));
    }
}

void BulkMediaImporter::onImport()
{
    if (m_selectedFiles.isEmpty()) {
        return;
    }
    
    m_progressBar->setVisible(true);
    m_progressBar->setMaximum(m_selectedFiles.size());
    m_progressBar->setValue(0);
    
    processFiles(m_selectedFiles);
}

void BulkMediaImporter::onCancel()
{
    emit importCancelled();
}

void BulkMediaImporter::onTypeChanged(int index)
{
    m_selectedType = static_cast<MediaType>(m_typeCombo->itemData(index).toInt());
}

void BulkMediaImporter::processFiles(const QStringList& filePaths)
{
    QJsonArray importedItems;
    MetadataService metadataService;
    MediaDb* mediaDb = new MediaDb();
    
    for (int i = 0; i < filePaths.size(); ++i) {
        const QString& filePath = filePaths[i];
        QFileInfo fileInfo(filePath);
        
        // Detect type from filename
        MediaType detectedType = detectTypeFromFilename(fileInfo.fileName());
        if (detectedType == MediaType::Video) {
            detectedType = m_selectedType; // Use selected type as fallback
        }
        
        // Extract metadata
        QFuture<MediaMetadata*> future = metadataService.extractMetadata(filePath);
        MediaMetadata* metadata = future.result();
        
        // Create media item
        QJsonObject item;
        item["id"] = QString("import_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(i);
        item["title"] = fileInfo.baseName();
        item["duration"] = metadata ? metadata->duration : 0.0;
        item["src"] = filePath;
        item["type"] = MediaDetection::mediaTypeToString(detectedType);
        item["filePath"] = filePath;
        item["filename"] = fileInfo.fileName();
        
        // Save to database
        if (mediaDb) {
            // Note: Would need saveMedia method in MediaDb
        }
        
        importedItems.append(item);
        
        m_progressBar->setValue(i + 1);
        QApplication::processEvents();
        
        if (metadata) {
            delete metadata;
        }
    }
    
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Imported %1 item(s)").arg(importedItems.size()));
    
    emit importComplete(importedItems);
    
    delete mediaDb;
}

MediaType BulkMediaImporter::detectTypeFromFilename(const QString& filename)
{
    QString lower = filename.toLower();
    
    if (lower.contains("_ad") || lower.contains("ad_") || lower.contains("commercial") || lower.contains("spot")) {
        return MediaType::Ad;
    }
    if (lower.contains("_id") || lower.contains("id_") || lower.contains("station") || lower.contains("ident")) {
        return MediaType::StationId;
    }
    if (lower.contains("bump") || lower.contains("_bmp") || lower.contains("bmp_")) {
        return MediaType::Bumper;
    }
    if (lower.contains("promo") || lower.contains("teaser") || lower.contains("trailer")) {
        return MediaType::Promo;
    }
    if (lower.contains("program") || lower.contains("episode") || lower.contains("show") || lower.contains("_ep")) {
        return MediaType::Program;
    }
    
    QString ext = QFileInfo(filename).suffix().toLower();
    if (QStringList({"jpg", "jpeg", "png", "gif", "webp", "bmp"}).contains(ext)) {
        return MediaType::Image;
    }
    
    return m_selectedType;
}

