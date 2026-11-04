#include "mainwindow.h"
#include "Play_Play_Native/widgets/medialibrary.h"
#include "Play_Play_Native/widgets/playlisteditor.h"
#include "Play_Play_Native/widgets/unifiedscheduler.h"
#include "Play_Play_Native/widgets/cgcontroller.h"
#include "Play_Play_Native/widgets/settingspanel.h"
#include "Play_Play_Native/widgets/bulkmediaimporter.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tabWidget(new QTabWidget(this))
{
    setCentralWidget(m_tabWidget);
    setWindowTitle("PLAY PLAY - TV Playout Automation System");
    resize(1600, 1000);
    
    createMenus();
    createToolBars();
    createStatusBar();
    createDockWindows();
    
    readSettings();
}

MainWindow::~MainWindow()
{
    writeSettings();
}

void MainWindow::createMenus()
{
    m_fileMenu = menuBar()->addMenu(tr("&File"));
    m_newAct = m_fileMenu->addAction(tr("&New"), this, &MainWindow::newFile);
    m_newAct->setShortcut(QKeySequence::New);
    m_openAct = m_fileMenu->addAction(tr("&Open"), this, &MainWindow::openFile);
    m_openAct->setShortcut(QKeySequence::Open);
    m_saveAct = m_fileMenu->addAction(tr("&Save"), this, &MainWindow::saveFile);
    m_saveAct->setShortcut(QKeySequence::Save);
    m_fileMenu->addSeparator();
    m_exitAct = m_fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    m_exitAct->setShortcut(QKeySequence::Quit);
    
    m_editMenu = menuBar()->addMenu(tr("&Edit"));
    m_settingsAct = m_editMenu->addAction(tr("&Settings"), this, &MainWindow::showSettings);
    m_settingsAct->setShortcut(QKeySequence::Preferences);
    
    m_viewMenu = menuBar()->addMenu(tr("&View"));
    m_viewMenu->addAction(tr("&Media Library"), this, &MainWindow::showMediaLibrary);
    m_viewMenu->addAction(tr("&Playlist Editor"), this, &MainWindow::showPlaylistEditor);
    m_viewMenu->addAction(tr("&Scheduler"), this, &MainWindow::showScheduler);
    m_viewMenu->addAction(tr("&CG Controller"), this, &MainWindow::showCGController);
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(tr("&Bulk Media Importer"), this, &MainWindow::showBulkImporter);
    
    m_toolsMenu = menuBar()->addMenu(tr("&Tools"));
    
    m_helpMenu = menuBar()->addMenu(tr("&Help"));
    m_aboutAct = m_helpMenu->addAction(tr("&About"), this, &MainWindow::about);
}

void MainWindow::createToolBars()
{
    m_fileToolBar = addToolBar(tr("File"));
    m_fileToolBar->addAction(m_newAct);
    m_fileToolBar->addAction(m_openAct);
    m_fileToolBar->addAction(m_saveAct);
    
    m_editToolBar = addToolBar(tr("Edit"));
    m_editToolBar->addAction(m_settingsAct);
}

void MainWindow::createStatusBar()
{
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::createDockWindows()
{
    // Media Library Dock
    m_mediaLibraryDock = new QDockWidget(tr("Media Library"), this);
    m_mediaLibraryDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    MediaLibrary* mediaLibrary = new MediaLibrary(m_mediaLibraryDock);
    m_mediaLibraryDock->setWidget(mediaLibrary);
    addDockWidget(Qt::LeftDockWidgetArea, m_mediaLibraryDock);
    
    // Playlist Dock
    m_playlistDock = new QDockWidget(tr("Playlist Editor"), this);
    m_playlistDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    PlaylistEditor* playlistEditor = new PlaylistEditor(m_playlistDock);
    m_playlistDock->setWidget(playlistEditor);
    addDockWidget(Qt::RightDockWidgetArea, m_playlistDock);
    
    // Scheduler Dock
    m_schedulerDock = new QDockWidget(tr("Scheduler"), this);
    m_schedulerDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    UnifiedScheduler* scheduler = new UnifiedScheduler(m_schedulerDock);
    m_schedulerDock->setWidget(scheduler);
    addDockWidget(Qt::BottomDockWidgetArea, m_schedulerDock);
}

void MainWindow::newFile()
{
    // TODO: Implement new file
    statusBar()->showMessage(tr("New file"), 2000);
}

void MainWindow::openFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), "", tr("All Files (*)"));
    if (!fileName.isEmpty()) {
        // TODO: Implement file opening
        statusBar()->showMessage(tr("Opened: %1").arg(fileName), 2000);
    }
}

void MainWindow::saveFile()
{
    // TODO: Implement file saving
    statusBar()->showMessage(tr("File saved"), 2000);
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About PLAY PLAY"),
        tr("PLAY PLAY - TV Playout Automation System\n\n"
           "Version 1.0.0\n\n"
           "Professional broadcast automation suite built with Qt6."));
}

void MainWindow::showSettings()
{
    SettingsPanel* settings = new SettingsPanel(this);
    settings->setAttribute(Qt::WA_DeleteOnClose);
    settings->show();
}

void MainWindow::showMediaLibrary()
{
    m_mediaLibraryDock->show();
    m_mediaLibraryDock->raise();
}

void MainWindow::showPlaylistEditor()
{
    m_playlistDock->show();
    m_playlistDock->raise();
}

void MainWindow::showScheduler()
{
    m_schedulerDock->show();
    m_schedulerDock->raise();
}

void MainWindow::showCGController()
{
    CGController* cgController = new CGController(this);
    cgController->setAttribute(Qt::WA_DeleteOnClose);
    cgController->show();
}

void MainWindow::showBulkImporter()
{
    BulkMediaImporter* importer = new BulkMediaImporter(this);
    importer->setWindowTitle(tr("Bulk Media Importer"));
    importer->setWindowFlags(Qt::Window);
    importer->resize(800, 600);
    importer->show();
    
    connect(importer, &BulkMediaImporter::importComplete, this, [this](const QJsonArray& items) {
        statusBar()->showMessage(tr("Imported %1 media item(s)").arg(items.size()), 5000);
    });
    
    connect(importer, &BulkMediaImporter::importCancelled, this, [this]() {
        statusBar()->showMessage(tr("Import cancelled"), 3000);
    });
}

void MainWindow::readSettings()
{
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}

void MainWindow::writeSettings()
{
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
}
