#include "multiviewpage.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>

MultiviewPage::MultiviewPage(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(
        "QWidget { background-color: #09090b; color: white; }"
        "QLabel { color: white; }"
        "QPushButton { background-color: #3b82f6; color: white; border: none; padding: 8px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #2563eb; }"
        "QGroupBox { border: 1px solid #3b82f6; border-radius: 4px; margin-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
    );
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    
    // Header
    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(tr("Multiview"), this);
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold;");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    
    QPushButton* fullscreenButton = new QPushButton(tr("Fullscreen"), this);
    connect(fullscreenButton, &QPushButton::clicked, this, &MultiviewPage::onFullscreen);
    headerLayout->addWidget(fullscreenButton);
    
    mainLayout->addLayout(headerLayout);
    
    // Multiview grid (2x2)
    QGridLayout* gridLayout = new QGridLayout();
    gridLayout->setSpacing(10);
    
    // Preview 1
    QGroupBox* preview1 = createPreviewWidget(tr("Preview 1"), "preview1");
    gridLayout->addWidget(preview1, 0, 0);
    
    // Preview 2
    QGroupBox* preview2 = createPreviewWidget(tr("Preview 2"), "preview2");
    gridLayout->addWidget(preview2, 0, 1);
    
    // Preview 3
    QGroupBox* preview3 = createPreviewWidget(tr("Preview 3"), "preview3");
    gridLayout->addWidget(preview3, 1, 0);
    
    // Preview 4
    QGroupBox* preview4 = createPreviewWidget(tr("Preview 4"), "preview4");
    gridLayout->addWidget(preview4, 1, 1);
    
    mainLayout->addLayout(gridLayout);
    mainLayout->addStretch();
    
    Logger::instance()->info("Multiview page initialized");
}

QGroupBox* MultiviewPage::createPreviewWidget(const QString& title, const QString& id)
{
    QGroupBox* group = new QGroupBox(title, this);
    group->setMinimumSize(400, 300);
    
    QVBoxLayout* layout = new QVBoxLayout(group);
    
    // Preview area (placeholder)
    QLabel* previewLabel = new QLabel(tr("Preview\n(To be implemented)"), group);
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setStyleSheet(
        "background-color: #0f172a; "
        "border: 2px solid #3b82f6; "
        "border-radius: 4px; "
        "min-height: 250px; "
        "color: #94a3b8;"
    );
    layout->addWidget(previewLabel);
    
    // Controls
    QHBoxLayout* controlLayout = new QHBoxLayout();
    
    QPushButton* takeButton = new QPushButton(tr("Take"), group);
    takeButton->setStyleSheet("background-color: #22c55e;");
    connect(takeButton, &QPushButton::clicked, this, [this, id]() { onTake(id); });
    controlLayout->addWidget(takeButton);
    
    QPushButton* cutButton = new QPushButton(tr("Cut"), group);
    cutButton->setStyleSheet("background-color: #ef4444;");
    connect(cutButton, &QPushButton::clicked, this, [this, id]() { onCut(id); });
    controlLayout->addWidget(cutButton);
    
    controlLayout->addStretch();
    
    QLabel* statusLabel = new QLabel(tr("Ready"), group);
    statusLabel->setStyleSheet("color: #94a3b8;");
    controlLayout->addWidget(statusLabel);
    
    layout->addLayout(controlLayout);
    
    m_previewWidgets[id] = group;
    m_statusLabels[id] = statusLabel;
    
    return group;
}

void MultiviewPage::onTake(const QString& previewId)
{
    Logger::instance()->info(QString("Multiview: Take %1").arg(previewId));
    if (m_statusLabels.contains(previewId)) {
        m_statusLabels[previewId]->setText(tr("On Air"));
        m_statusLabels[previewId]->setStyleSheet("color: #22c55e; font-weight: bold;");
    }
}

void MultiviewPage::onCut(const QString& previewId)
{
    Logger::instance()->info(QString("Multiview: Cut %1").arg(previewId));
    if (m_statusLabels.contains(previewId)) {
        m_statusLabels[previewId]->setText(tr("Ready"));
        m_statusLabels[previewId]->setStyleSheet("color: #94a3b8;");
    }
}

void MultiviewPage::onFullscreen()
{
    Logger::instance()->info("Multiview: Fullscreen");
    // TODO: Open fullscreen multiview window
}

