#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QDockWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void newFile();
    void openFile();
    void saveFile();
    void about();
    void showSettings();
    void showMediaLibrary();
    void showPlaylistEditor();
    void showScheduler();
    void showCGController();
    void showBulkImporter();

private:
    void createMenus();
    void createToolBars();
    void createStatusBar();
    void createDockWindows();
    void readSettings();
    void writeSettings();
    
    QTabWidget* m_tabWidget;
    QDockWidget* m_mediaLibraryDock;
    QDockWidget* m_playlistDock;
    QDockWidget* m_schedulerDock;
    
    QMenu* m_fileMenu;
    QMenu* m_editMenu;
    QMenu* m_viewMenu;
    QMenu* m_toolsMenu;
    QMenu* m_helpMenu;
    
    QToolBar* m_fileToolBar;
    QToolBar* m_editToolBar;
    
    QAction* m_newAct;
    QAction* m_openAct;
    QAction* m_saveAct;
    QAction* m_exitAct;
    QAction* m_settingsAct;
    QAction* m_aboutAct;
};

#endif // MAINWINDOW_H




