#include "playout/uiscale.h"
#include "playout/nativetools.h"
#include <functional>
#include <memory>
#include <QFileInfo>
#include <QApplication>
#include <QSplashScreen>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QElapsedTimer>
#include <memory>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QDateTime>
#include "mainwindow.h"
#include "services/logger.h"
#include "playout/timeauthority.h"
#include "database/mediadb.h"
#include "database/asrunlogger.h"
#include "playout/controlservice.h"
#include "playout/supervisorclient.h"
#include "playout/supervisorapi.h"
#include "playout/meterticker.h"
#include <QQuickWindow>
#include <QTimer>
#include "playout/engineadapter.h"
#include "Play_Play_Native/models/medialibrarymodel.h"
#include "Play_Play_Native/models/activitylogmodel.h"
#include "Play_Play_Native/models/playlistmodel.h"
#include "Play_Play_Native/models/schedulemodel.h"
#include "Play_Play_Native/models/weeklyschedulemodel.h"
#include "Play_Play_Native/models/systemmonitormodel.h"
#include "services/copilotservice.h"
#include "services/settingsmanager.h"
#include "services/errorbus.h"
#include "services/errorhandler.h"
#include "services/persistentlogger.h"
#include "services/metadataservice.h"
#include "services/mediadetection.h"
#include "services/precisiontimer.h"
#include "services/folderscanner.h"
#include "services/drivewatcher.h"
#include "utils/assethasher.h"
// DEP-21: the ffplayout HTTP and OBS WebSocket app clients are legacy (no QML page uses them);
// cmake/lane-inhouse-legacy.cmake sets PLAYPLAY_WITH_LEGACY_CLIENTS to 0/1 (option, default ON). Undefined keeps them.
#ifndef PLAYPLAY_WITH_LEGACY_CLIENTS
#define PLAYPLAY_WITH_LEGACY_CLIENTS 1
#endif
#if PLAYPLAY_WITH_LEGACY_CLIENTS
#include "integration/obsscheduler.h"
#include "integration/obscommandexecutor.h"
#include "integration/ffplayoutapi.h"
#endif
#include "utils/platform.h"
#include "playout/core/loopwatchdog.h"
#include <QDir>
#include <QStandardPaths>
#include <QQmlContext>
#include <QFile>


// Start-up splash (branding/splash.png): the artwork with a real progress bar and the current step drawn over its bottom-left corner.
class BrandSplash : public QSplashScreen {
public:
    explicit BrandSplash(const QPixmap& pm) : QSplashScreen(pm, Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint) { m_clock.start(); }
    void setProgress(int pct, const QString& text) { m_pct = pct; m_text = text; repaint(); QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents); }
    qint64 shownMs() const { return m_clock.elapsed(); }
protected:
    void drawContents(QPainter* p) override {
        const double w = width(), h = height();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF track(0.043 * w, 0.886 * h, 0.392 * w, qMax(3.0, 0.0085 * h));
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(26, 34, 48)); p->drawRoundedRect(track, track.height() / 2, track.height() / 2);
        p->setBrush(QColor(0, 200, 255)); p->drawRoundedRect(QRectF(track.left(), track.top(), track.width() * qBound(0, m_pct, 100) / 100.0, track.height()), track.height() / 2, track.height() / 2);
        QFont f("Cascadia Mono"); f.setStyleHint(QFont::Monospace); f.setPixelSize(qMax(10, int(0.0175 * h)));
        p->setFont(f); p->setPen(QColor(176, 188, 202));
        p->drawText(QPointF(0.043 * w, 0.935 * h), m_text);
    }
private:
    int m_pct = 0; QString m_text; QElapsedTimer m_clock;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Fusion");

    // Ensure Qt can find platform/plugins in both dev and packaged layouts.
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList libPaths = QCoreApplication::libraryPaths();
    auto addPath = [&libPaths](const QString& path) {
        if (!path.isEmpty() && QDir(path).exists() && !libPaths.contains(path)) {
            libPaths.prepend(path);
        }
    };
    addPath(appDir);
    addPath(appDir + "/plugins");
    addPath(appDir + "/../plugins");
    QCoreApplication::setLibraryPaths(libPaths);
    
    // Set application properties
    app.setApplicationName("PLAY PLAY");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("PlayPlay");
    {   // STB-4: GUI-thread watchdog ("Not Responding" at 5 s); JSON lines and dumps next to playplay_*.log; never aborts
        LoopWatchdogOptions wo;
        wo.name = "app";
        wo.logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
        wo.stallMs = 500; wo.hangMs = 5000; wo.dumpAfterMs = 15000; wo.abortAfterMs = 0;
        LoopWatchdog::install(wo);
    }
    {   // the window / taskbar icon (PNG sizes from the resources: Qt's .ico reader is a plugin the build does not stage)
        QIcon icon;
        for (int n : {16, 32, 48, 64, 128, 256}) icon.addFile(QString(":/branding/icon-%1.png").arg(n));
        app.setWindowIcon(icon);
    }
    // Splash: skipped for automation (--screenshot) and with --no-splash.
    std::unique_ptr<BrandSplash> splash;
    if (!app.arguments().contains("--screenshot") && !app.arguments().contains("--no-splash")) {
        const QPixmap pm(":/branding/splash.png");
        if (!pm.isNull()) { splash.reset(new BrandSplash(pm.scaledToWidth(1120, Qt::SmoothTransformation))); splash->show(); splash->setProgress(4, "Starting..."); }
    }
    auto step = [&splash](int pct, const QString& text) { if (splash) splash->setProgress(pct, text); };
    
    // Initialize logger
    Logger::instance()->initialize();
    Logger::instance()->info("Starting PLAY PLAY - TV Playout Automation System");
    
    // Initialize settings manager
    SettingsManager::instance()->loadAll();
    step(15, "Loading settings...");
    
    // Initialize error handling
    ErrorBus::instance();
    ErrorHandler::instance();
    PersistentLogger::instance();
    TimerConfig timerCfg{20, 20, 100, false, 1000, false};
    PrecisionTimer* precisionTimer = new PrecisionTimer(timerCfg, &app);
    MetadataService* metadataService = new MetadataService(&app);
    MediaDetection* mediaDetection = new MediaDetection(&app);
    FolderScanner* folderScanner = new FolderScanner(&app);
    DriveWatcher* driveWatcher = new DriveWatcher(&app);
    AssetHasher* assetHasher = new AssetHasher(&app);
#if PLAYPLAY_WITH_LEGACY_CLIENTS
    OBSScheduler* obsScheduler = new OBSScheduler(&app);
    OBSCommandExecutor* obsCommandExecutor = new OBSCommandExecutor(&app);
    FFPlayoutAPI* ffplayoutApi = new FFPlayoutAPI(SettingsManager::instance()->getFFPlayoutApiUrl(), 1, &app);
    Logger::instance()->info("Legacy ffplayout/OBS app clients constructed (PLAYPLAY_WITH_LEGACY_CLIENTS=1)");
#else
    Logger::instance()->info("Legacy ffplayout/OBS app clients not built (PLAYPLAY_WITH_LEGACY_CLIENTS=0)");
#endif

    // Initialize Time Authority
    TimeAuthority* timeAuth = getTimeAuthority();
    timeAuth->setFrameRate(30); // Default to 30 fps
    
    step(30, "Opening the media library...");
    // Initialize Media Database (non-critical - can continue without it)
    MediaDb* mediaDb = nullptr;
    try {
        mediaDb = new MediaDb();
        if (!mediaDb->initialize()) {
            Logger::instance()->warning("Failed to initialize media database - continuing without it");
        }
    } catch (const std::exception& e) {
        Logger::instance()->warning(QString("Media database initialization exception: %1").arg(e.what()));
    }
    
    step(42, "Opening the as-run log...");
    // Initialize As-Run Logger (non-critical)
    AsRunLogger* asRunLogger = nullptr;
    try {
        asRunLogger = getAsRunLogger();
        if (!asRunLogger->initialize()) {
            Logger::instance()->warning("Failed to initialize as-run logger - continuing without it");
        }
    } catch (const std::exception& e) {
        Logger::instance()->warning(QString("As-run logger initialization exception: %1").arg(e.what()));
    }
    
    step(52, "Initializing playout engine...");
    // Initialize Control Service (non-critical for UI)
    ControlService* controlService = nullptr;
    try {
        controlService = getControlService();
        if (!controlService->initialize()) {
            Logger::instance()->warning("Failed to initialize control service - UI will work without playout");
        }
    } catch (const std::exception& e) {
        Logger::instance()->warning(QString("Control service initialization exception: %1").arg(e.what()));
    }
    
    step(64, "Loading the interface...");
    // Try to use QML first, fallback to Widgets
    QQmlApplicationEngine* engine = nullptr;
    try {
        engine = new QQmlApplicationEngine();
        QQuickStyle::setStyle("Fusion");
        
        // Register QML types
        qmlRegisterType<MediaLibraryModel>("PlayPlay.Models", 1, 0, "MediaLibraryModel");
        qmlRegisterType<PlaylistModel>("PlayPlay.Models", 1, 0, "PlaylistModel");
        qmlRegisterType<ScheduleModel>("PlayPlay.Models", 1, 0, "ScheduleModel");
        qmlRegisterType<WeeklyScheduleModel>("PlayPlay.Models", 1, 0, "WeeklyScheduleModel");
        
        // Register meta types for QML
        qRegisterMetaType<MediaItem>("MediaItem");
        qRegisterMetaType<PlaylistItem>("PlaylistItem");
        qRegisterMetaType<AsRunEntry>("AsRunEntry");
        
        // Create and expose models to QML context
        MediaLibraryModel* mediaLibraryModel = new MediaLibraryModel(&app);
        PlaylistModel* playlistModel = new PlaylistModel(&app);
        ScheduleModel* scheduleModel = new ScheduleModel(&app);
        WeeklyScheduleModel* weeklyScheduleModel = new WeeklyScheduleModel(&app);
        SystemMonitorModel* systemMonitor = new SystemMonitorModel(&app);
        ActivityLogModel* activityLogModel = new ActivityLogModel(&app);

        // Load starting playlist slot
        playlistModel->loadSlot(0, 0);
        
        engine->rootContext()->setContextProperty("mediaLibraryModel", mediaLibraryModel);
        engine->rootContext()->setContextProperty("playlistModel", playlistModel);
        engine->rootContext()->setContextProperty("scheduleModel", scheduleModel);
        engine->rootContext()->setContextProperty("weeklyScheduleModel", weeklyScheduleModel);
        engine->rootContext()->setContextProperty("systemMonitor", systemMonitor);
        engine->rootContext()->setContextProperty("activityLogModel", activityLogModel);
        engine->rootContext()->setContextProperty("controlService", getControlService());
        engine->rootContext()->setContextProperty("copilotService", getCopilotService());
        engine->rootContext()->setContextProperty("settingsManager", SettingsManager::instance());
        engine->rootContext()->setContextProperty("errorBus", ErrorBus::instance());
        engine->rootContext()->setContextProperty("errorHandler", ErrorHandler::instance());
        engine->rootContext()->setContextProperty("persistentLogger", PersistentLogger::instance());
        engine->rootContext()->setContextProperty("precisionTimer", precisionTimer);
        engine->rootContext()->setContextProperty("metadataService", metadataService);
        engine->rootContext()->setContextProperty("mediaDetection", mediaDetection);
        engine->rootContext()->setContextProperty("folderScanner", folderScanner);
        engine->rootContext()->setContextProperty("driveWatcher", driveWatcher);
        engine->rootContext()->setContextProperty("assetHasher", assetHasher);
#if PLAYPLAY_WITH_LEGACY_CLIENTS
        engine->rootContext()->setContextProperty("obsScheduler", obsScheduler);
        engine->rootContext()->setContextProperty("obsCommandExecutor", obsCommandExecutor);
        engine->rootContext()->setContextProperty("ffplayoutApi", ffplayoutApi);
#endif

        // Supervisor connection: the headless automation host runs playout; the
        // app is a client. Launch one next to us if none is running.
        SupervisorClient* supervisor = new SupervisorClient(&app);
        setSupervisorClientForAdapters(supervisor);
        SupervisorClient::Options so;
        so.dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/supervisor";
        // Attach to another Supervisor (a test or remote-control instance) instead of the default one:
        // --supervisor-port <rest port> --supervisor-ipc <ipc name>. Such a Supervisor is never launched by the app.
        {
            const QStringList sa = app.arguments();
            const int pi = sa.indexOf("--supervisor-port"), ii = sa.indexOf("--supervisor-ipc");
            if (pi > 0 && pi + 1 < sa.size()) { so.restPort = sa[pi + 1].toInt(); so.launchIfMissing = false; }
            if (ii > 0 && ii + 1 < sa.size()) { so.ipcName = sa[ii + 1]; so.launchIfMissing = false; }
#ifdef PLAYPLAY_DEV_TREE
            // A development build (cmake -DPLAYPLAY_DEV_TREE=ON) never launches a Supervisor and never attaches to the live one
            // (2026-10-26: a build-seg app without --supervisor-port launched a build-seg Supervisor on the live identity).
            if (pi <= 0 || ii <= 0 || so.restPort == 7700 || so.ipcName == QLatin1String("PlayPlaySupervisor")) {
                qCritical("PlayPlay (dev tree): run it with --supervisor-port <test port> --supervisor-ipc <test name>; it never launches or joins the live Supervisor.");
                return 3;
            }
            so.launchIfMissing = false;
#endif
        }
        step(78, "Connecting to the Supervisor...");
        supervisor->start(so);
        engine->rootContext()->setContextProperty("supervisor", supervisor);
        SupervisorApi* api = new SupervisorApi(&app);
        api->setBaseUrl(supervisor->restUrl());
        // Paths given on the command line (Explorer drops them on the exe / a shortcut, "Send to") go
        // into the media library once the Supervisor answers.
        {
            QVariantList pendingImports;
            const QStringList argv = app.arguments();
            for (int i = 1; i < argv.size(); ++i) {
                const QString a = argv[i];
                if (a.startsWith("--")) { if (a == "--screenshot" || a == "--page" || a == "--delay" || a == "--zoom" || a == "--supervisor-port" || a == "--supervisor-ipc") ++i; continue; }
                if (QFileInfo::exists(a)) pendingImports << QDir::toNativeSeparators(QFileInfo(a).absoluteFilePath());
            }
            if (!pendingImports.isEmpty()) {
                auto tryImport = std::make_shared<std::function<void(int)>>();
                *tryImport = [api, supervisor, pendingImports, tryImport](int attempt) {
                    if (!supervisor->isConnected()) {
                        if (attempt < 30) QTimer::singleShot(1000, [tryImport, attempt]() { (*tryImport)(attempt + 1); });
                        else qWarning() << "[App] import skipped: Supervisor not reachable";
                        return;
                    }
                    QVariantMap body; body["files"] = pendingImports; body["type"] = "program";
                    api->post("/media/scan", body);
                    qInfo() << "[App] importing" << pendingImports.size() << "path(s) from the command line";
                };
                QTimer::singleShot(1500, [tryImport]() { (*tryImport)(0); });
            }
        }
        engine->rootContext()->setContextProperty("api", api);
        // PA-5 (W18): one shared peak-hold timer for every VuMeter / AudioStrip, running only while a hold decays.
        engine->rootContext()->setContextProperty("meterTicker", new MeterTicker(&app));
        engine->rootContext()->setContextProperty("nativeTools", new NativeTools(&app));
        // Whole-interface zoom ("much larger text"): scales the window root item, popups and menus included.
        UiScale* uiScale = new UiScale(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ui.ini", &app);
        { const QStringList za = app.arguments(); const int zi = za.indexOf("--zoom"); if (zi > 0 && zi + 1 < za.size()) { uiScale->setPersist(false); uiScale->setZoom(za[zi + 1].toDouble()); } }
        engine->rootContext()->setContextProperty("uiScale", uiScale);
        engine->addImageProvider("preview", new PreviewImageProvider(supervisor));

        getControlService()->initialize();
        QJsonObject engineConfig;
        engineConfig["channelId"] = "main";
        getControlService()->registerEngine("main", "supervisor", engineConfig);
        getControlService()->setSkipHandler([supervisor]() { supervisor->skip(); });
        getControlService()->start();

        // Add QML import path for local resources
        engine->addImportPath("qrc:/qml");
        
        // Add QML import path for deployed modules
        engine->addImportPath(appDir + "/qml");
        
        // Load main QML file
        const QUrl url(QStringLiteral("qrc:/qml/main.qml"));
        
        // Check if resource exists
        if (!QFile::exists(":/qml/main.qml")) {
            Logger::instance()->error("CRITICAL: QML resource file NOT FOUND: qrc:/qml/main.qml");
            QMessageBox::critical(nullptr, "Resource Error", "QML resources missing! Falling back to legacy Widgets UI.");
            MainWindow* window = new MainWindow();
            window->show();
            return app.exec();
        }
        
        // Capture QML load errors
        QString capturedErrors;
        QObject::connect(engine, &QQmlApplicationEngine::warnings, [&](const QList<QQmlError> &warnings) {
            for (const QQmlError &error : warnings) {
                QString entry = QString("[%1:%2] %3\n")
                    .arg(error.url().toString().section('/', -1))
                    .arg(error.line())
                    .arg(error.description());
                capturedErrors += entry;
                Logger::instance()->warning("QML Error: " + entry);
            }
        });

        QObject::connect(engine, &QQmlApplicationEngine::objectCreated,
                         &app, [url](QObject *obj, const QUrl &objUrl) {
            if (!obj && url == objUrl) {
                Logger::instance()->error(QString("Failed to load QML file: %1").arg(objUrl.toString()));
            }
        }, Qt::QueuedConnection);
        
        step(90, "Starting the interface...");
        engine->load(url);
        
        // If QML fails to load, fallback to MainWindow with alert showing detailed errors
        if (engine->rootObjects().isEmpty() && app.arguments().contains("--screenshot")) { qCritical() << "QML failed to load:" << capturedErrors; return 2; }
        if (engine->rootObjects().isEmpty()) {
            QString errorMsg = "QML engine failed to load any root objects.\n\nCaptured Errors:\n" + 
                               (capturedErrors.isEmpty() ? "No specific errors reported." : capturedErrors);
            Logger::instance()->error(errorMsg);
            QMessageBox::critical(nullptr, "UI Load Error", errorMsg);
            
            delete engine;
            MainWindow* window = new MainWindow();
            window->show();
            return app.exec();
        }
        
        Logger::instance()->info("QML application loaded successfully. Modern UI should be visible.");
        if (auto* qw = qobject_cast<QQuickWindow*>(engine->rootObjects().first())) {
            uiScale->attach(qw);
            if (splash) {   // close the splash once the first frame is on screen (and it has been up long enough to read)
                step(100, "Ready");
                BrandSplash* sp = splash.get();
                QObject::connect(qw, &QQuickWindow::frameSwapped, sp, [sp]() { QTimer::singleShot(qMax<qint64>(0, 1300 - sp->shownMs()), sp, [sp]() { sp->close(); }); }, Qt::SingleShotConnection);
            }
        }
        QObject::connect(&app, &QApplication::aboutToQuit, &app, [&engine, supervisor, api]() {
            api->shutdown();                                 // QJSValue callbacks before the engine goes
            delete engine; engine = nullptr;                 // QML first: it binds to supervisor/api
            getControlService()->shutdown();                // adapters hold the supervisor client pointer
            setSupervisorClientForAdapters(nullptr);
            supervisor->stop();
        });
        // --screenshot <file> [--page <name>] [--delay ms]: grab the window and exit (UI verification).
        const QStringList args = app.arguments();
        const int shotIdx = args.indexOf("--screenshot");
        if (shotIdx > 0 && shotIdx + 1 < args.size()) {
            const QString file = args[shotIdx + 1];
            const int pageIdx = args.indexOf("--page");
            const QString page = pageIdx > 0 && pageIdx + 1 < args.size() ? args[pageIdx + 1] : QString();
            const int delayIdx = args.indexOf("--delay");
            const int delay = delayIdx > 0 && delayIdx + 1 < args.size() ? args[delayIdx + 1].toInt() : 4000;
            QObject* root = engine->rootObjects().first();
            if (!page.isEmpty()) QMetaObject::invokeMethod(root, "navTo", Q_ARG(QVariant, page));
            QTimer::singleShot(delay, &app, [root, file, &app, &engine, api]() {
                if (auto* win = qobject_cast<QQuickWindow*>(root)) { const QImage img = win->grabWindow(); img.save(file); qInfo() << "[App] screenshot" << file << img.size(); }
                api->shutdown();
                delete engine; engine = nullptr;      // tear QML down while the supervisor client still exists
                app.quit();
            });
        }
    } catch (const std::exception& e) {
        Logger::instance()->error(QString("Exception during QML initialization: %1").arg(e.what()));
        Logger::instance()->info("Falling back to MainWindow (Widgets mode)");
        if (engine) delete engine;
        MainWindow window;
        window.show();
        return app.exec();
    } catch (...) {
        Logger::instance()->error("Unknown exception during QML initialization");
        Logger::instance()->info("Falling back to MainWindow (Widgets mode)");
        if (engine) delete engine;
        MainWindow window;
        window.show();
        return app.exec();
    }
    
    return app.exec();
}
