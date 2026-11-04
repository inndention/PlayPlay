// Qt Quick Test runner for the ux-buttons lane: every tests/qml/buttons/tst_*.qml.
// The setup hands the QML tests what they cannot read themselves: the app's QML folder (to import the real ui/ components)
// and the shipped key-guard lines of main.qml and OverlaysView.qml (UXB-0), so tst_typingguard.qml tests the code the app
// runs rather than a copy that could drift. The PlayPlay.Buttons module comes from the linked ui-buttons.qrc under the
// qrc:/qml import path, as in the app (tst_pbutton.qml, tst_toggle.qml).
#include <QtQuickTest/quicktest.h>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStringList>
#include <QUrl>

namespace {
QString srcPath(const QString& rel) { return QStringLiteral(PLAYPLAY_SOURCE_DIR "/") + rel; }

// The trimmed lines of a source file that contain one of `needles`, in file order and joined by newlines; empty when the
// file or every line is missing (the QML test fails loudly on that).
QString linesWith(const QString& rel, const QStringList& needles) {
    QFile f(srcPath(rel));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList out;
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    for (const QString& l : lines)
        for (const QString& n : needles)
            if (l.contains(n)) { out << l.trimmed(); break; }
    return out.join(QLatin1Char('\n'));
}
}

class Setup : public QObject {
    Q_OBJECT
public slots:
    void qmlEngineAvailable(QQmlEngine* engine) {
        // PlayPlay.Buttons from the linked ui-buttons.qrc, found the way main.cpp finds it (UXB-3a).
        engine->addImportPath(QStringLiteral("qrc:/qml"));
        QQmlContext* c = engine->rootContext();
        c->setContextProperty("ppQmlDir", QUrl::fromLocalFile(srcPath("Play_Play_Native/qml")).toString());
        c->setContextProperty("ppSpaceGuard", linesWith("Play_Play_Native/qml/main.qml",
            { "function spaceIsTransport(", "Shortcut { sequence: \"Space\"" }));
        c->setContextProperty("ppTypingProperty", linesWith("Play_Play_Native/qml/views/OverlaysView.qml",
            { "readonly property bool typing:" }));
        c->setContextProperty("ppPresetEnabled", linesWith("Play_Play_Native/qml/views/OverlaysView.qml",
            { "enabled: view.visible && !view.typing" }));
    }
};

int main(int argc, char** argv) {
    // Headless and deterministic: offscreen windows activate synchronously (Window and Application shortcuts need an active
    // window), the software renderer needs no GPU, and the window never takes the desktop focus from the live app.
    // Fusion is the app's style (main.cpp). Each default yields to an explicit environment setting.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (!qEnvironmentVariableIsSet("QT_QUICK_BACKEND")) qputenv("QT_QUICK_BACKEND", "software");
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_STYLE")) qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");
#ifdef Q_OS_WIN
    // The offscreen platform's FreeType font database looks next to Qt (no fonts there): use the system fonts.
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR")) {
        const QString fonts = qEnvironmentVariable("WINDIR", QStringLiteral("C:\\Windows")) + QStringLiteral("\\Fonts");
        qputenv("QT_QPA_FONTDIR", fonts.toLocal8Bit());
    }
#endif
    Setup setup;
    return quick_test_main_with_setup(argc, argv, "buttons_qml", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "test_buttons_qml.moc"
