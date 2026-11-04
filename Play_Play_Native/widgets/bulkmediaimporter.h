#ifndef BULKMEDIAIMPORTER_H
#define BULKMEDIAIMPORTER_H

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include <QProgressBar>
#include <QLabel>
#include <QFileDialog>
#include "services/folderscanner.h"
#include "services/mediadetection.h"

class BulkMediaImporter : public QWidget
{
    Q_OBJECT
    
public:
    explicit BulkMediaImporter(QWidget* parent = nullptr);
    
signals:
    void importComplete(const QJsonArray& items);
    void importCancelled();
    
private slots:
    void onSelectFiles();
    void onSelectFolder();
    void onImport();
    void onCancel();
    void onTypeChanged(int index);
    
private:
    void processFiles(const QStringList& filePaths);
    MediaType detectTypeFromFilename(const QString& filename);
    
    QListWidget* m_fileList;
    QComboBox* m_typeCombo;
    QPushButton* m_selectFilesButton;
    QPushButton* m_selectFolderButton;
    QPushButton* m_importButton;
    QPushButton* m_cancelButton;
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    
    QStringList m_selectedFiles;
    MediaType m_selectedType;
};

#endif // BULKMEDIAIMPORTER_H

