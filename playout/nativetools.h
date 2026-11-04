#pragma once
#include <QObject>
#include <QApplication>
#include <QColorDialog>
#include <QFileDialog>

class NativeTools : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE QString recordingDirectory(const QString& initial) { return QFileDialog::getExistingDirectory(QApplication::activeWindow(), tr("Layered recording session"), initial); }
    Q_INVOKABLE QString recordingOutput(const QString& initial) { return QFileDialog::getSaveFileName(QApplication::activeWindow(), tr("Export recording to a new file"), initial, tr("Matroska video (*.mkv)"), nullptr, QFileDialog::DontConfirmOverwrite); }
    Q_INVOKABLE QString pickScreenColor(const QString& initial) {
        const QColor picked = QColorDialog::getColor(QColor(initial), QApplication::activeWindow(),
            tr("Screen color — use Pick Screen Color to sample the picture"), QColorDialog::DontUseNativeDialog);
        return picked.isValid() ? picked.name(QColor::HexRgb) : QString();
    }
};
