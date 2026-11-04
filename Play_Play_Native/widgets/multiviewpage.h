#ifndef MULTIVIEWPAGE_H
#define MULTIVIEWPAGE_H

#include <QWidget>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QMap>

class MultiviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit MultiviewPage(QWidget *parent = nullptr);

private slots:
    void onTake(const QString& previewId);
    void onCut(const QString& previewId);
    void onFullscreen();

private:
    QGroupBox* createPreviewWidget(const QString& title, const QString& id);
    
    QMap<QString, QGroupBox*> m_previewWidgets;
    QMap<QString, QLabel*> m_statusLabels;
};

#endif // MULTIVIEWPAGE_H

