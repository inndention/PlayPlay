#include "settingspanel.h"
#include <QLabel>
#include "services/logger.h"
#include "core/types.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QSettings>
#include <QMessageBox>

SettingsPanel::SettingsPanel(QWidget *parent)
    : QDialog(parent)
    , m_tabWidget(new QTabWidget(this))
    , m_okButton(new QPushButton(tr("OK"), this))
    , m_cancelButton(new QPushButton(tr("Cancel"), this))
    , m_applyButton(new QPushButton(tr("Apply"), this))
{
    setWindowTitle(tr("Settings"));
    resize(700, 500);
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Automation tab
    QWidget* automationTab = createAutomationTab();
    m_tabWidget->addTab(automationTab, tr("Automation"));
    
    // Playback tab
    QWidget* playbackTab = createPlaybackTab();
    m_tabWidget->addTab(playbackTab, tr("Playback"));
    
    // Integration tab
    QWidget* integrationTab = createIntegrationTab();
    m_tabWidget->addTab(integrationTab, tr("Integrations"));
    
    // Storage tab
    QWidget* storageTab = createStorageTab();
    m_tabWidget->addTab(storageTab, tr("Storage"));
    
    mainLayout->addWidget(m_tabWidget);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_okButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addWidget(m_applyButton);
    mainLayout->addLayout(buttonLayout);
    
    connect(m_okButton, &QPushButton::clicked, this, &SettingsPanel::onOk);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsPanel::onApply);
    
    loadSettings();
}

QWidget* SettingsPanel::createAutomationTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(tab);
    
    QGroupBox* automationGroup = new QGroupBox(tr("Automation Settings"), tab);
    QVBoxLayout* automationLayout = new QVBoxLayout(automationGroup);
    
    m_alwaysPlayLatest = new QCheckBox(tr("Always play latest episode"), automationGroup);
    m_shuffleAds = new QCheckBox(tr("Shuffle ads in hour"), automationGroup);
    m_fillGapsWithIds = new QCheckBox(tr("Fill gaps with station IDs"), automationGroup);
    m_fillGapsWithBumpers = new QCheckBox(tr("Fill gaps with bumpers"), automationGroup);
    m_autoAdvance = new QCheckBox(tr("Auto-advance playlist"), automationGroup);
    m_loopAtEnd = new QCheckBox(tr("Loop at end of schedule"), automationGroup);
    
    automationLayout->addWidget(m_alwaysPlayLatest);
    automationLayout->addWidget(m_shuffleAds);
    automationLayout->addWidget(m_fillGapsWithIds);
    automationLayout->addWidget(m_fillGapsWithBumpers);
    automationLayout->addWidget(m_autoAdvance);
    automationLayout->addWidget(m_loopAtEnd);
    
    layout->addWidget(automationGroup);
    layout->addStretch();
    
    return tab;
}

QWidget* SettingsPanel::createPlaybackTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(tab);
    
    QGroupBox* playbackGroup = new QGroupBox(tr("Playback Settings"), tab);
    QVBoxLayout* playbackLayout = new QVBoxLayout(playbackGroup);
    
    QHBoxLayout* frameRateLayout = new QHBoxLayout();
    frameRateLayout->addWidget(new QLabel(tr("Frame Rate:"), playbackGroup));
    m_frameRateSpin = new QSpinBox(playbackGroup);
    m_frameRateSpin->setRange(24, 60);
    m_frameRateSpin->setValue(30);
    frameRateLayout->addWidget(m_frameRateSpin);
    frameRateLayout->addStretch();
    playbackLayout->addLayout(frameRateLayout);
    
    QHBoxLayout* toleranceLayout = new QHBoxLayout();
    toleranceLayout->addWidget(new QLabel(tr("Tolerance Frames:"), playbackGroup));
    m_toleranceSpin = new QSpinBox(playbackGroup);
    m_toleranceSpin->setRange(0, 10);
    m_toleranceSpin->setValue(2);
    toleranceLayout->addWidget(m_toleranceSpin);
    toleranceLayout->addStretch();
    playbackLayout->addLayout(toleranceLayout);
    
    layout->addWidget(playbackGroup);
    layout->addStretch();
    
    return tab;
}

QWidget* SettingsPanel::createIntegrationTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(tab);
    
    QGroupBox* obsGroup = new QGroupBox(tr("OBS Studio"), tab);
    QVBoxLayout* obsLayout = new QVBoxLayout(obsGroup);
    
    QHBoxLayout* obsHostLayout = new QHBoxLayout();
    obsHostLayout->addWidget(new QLabel(tr("Host:"), obsGroup));
    m_obsHostEdit = new QLineEdit("localhost", obsGroup);
    obsHostLayout->addWidget(m_obsHostEdit);
    obsLayout->addLayout(obsHostLayout);
    
    QHBoxLayout* obsPortLayout = new QHBoxLayout();
    obsPortLayout->addWidget(new QLabel(tr("Port:"), obsGroup));
    m_obsPortSpin = new QSpinBox(obsGroup);
    m_obsPortSpin->setRange(1, 65535);
    m_obsPortSpin->setValue(4455);
    obsPortLayout->addWidget(m_obsPortSpin);
    obsPortLayout->addStretch();
    obsLayout->addLayout(obsPortLayout);
    
    QHBoxLayout* obsPasswordLayout = new QHBoxLayout();
    obsPasswordLayout->addWidget(new QLabel(tr("Password:"), obsGroup));
    m_obsPasswordEdit = new QLineEdit(obsGroup);
    m_obsPasswordEdit->setEchoMode(QLineEdit::Password);
    obsPasswordLayout->addWidget(m_obsPasswordEdit);
    obsLayout->addLayout(obsPasswordLayout);
    
    layout->addWidget(obsGroup);
    layout->addStretch();
    
    return tab;
}

QWidget* SettingsPanel::createStorageTab()
{
    QWidget* tab = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(tab);
    
    QGroupBox* storageGroup = new QGroupBox(tr("Storage Settings"), tab);
    QVBoxLayout* storageLayout = new QVBoxLayout(storageGroup);
    
    QHBoxLayout* mediaPathLayout = new QHBoxLayout();
    mediaPathLayout->addWidget(new QLabel(tr("Media Path:"), storageGroup));
    m_mediaPathEdit = new QLineEdit(storageGroup);
    mediaPathLayout->addWidget(m_mediaPathEdit);
    QPushButton* browseButton = new QPushButton(tr("Browse..."), storageGroup);
    connect(browseButton, &QPushButton::clicked, this, &SettingsPanel::onBrowseMediaPath);
    mediaPathLayout->addWidget(browseButton);
    storageLayout->addLayout(mediaPathLayout);
    
    layout->addWidget(storageGroup);
    layout->addStretch();
    
    return tab;
}

void SettingsPanel::loadSettings()
{
    QSettings settings;
    
    // Automation settings
    m_alwaysPlayLatest->setChecked(settings.value("automation/alwaysPlayLatest", false).toBool());
    m_shuffleAds->setChecked(settings.value("automation/shuffleAds", true).toBool());
    m_fillGapsWithIds->setChecked(settings.value("automation/fillGapsWithIds", false).toBool());
    m_fillGapsWithBumpers->setChecked(settings.value("automation/fillGapsWithBumpers", false).toBool());
    m_autoAdvance->setChecked(settings.value("automation/autoAdvance", true).toBool());
    m_loopAtEnd->setChecked(settings.value("automation/loopAtEnd", false).toBool());
    
    // Playback settings
    m_frameRateSpin->setValue(settings.value("playback/frameRate", 30).toInt());
    m_toleranceSpin->setValue(settings.value("playback/toleranceFrames", 2).toInt());
    
    // Integration settings
    m_obsHostEdit->setText(settings.value("integration/obs/host", "localhost").toString());
    m_obsPortSpin->setValue(settings.value("integration/obs/port", 4455).toInt());
    m_obsPasswordEdit->setText(settings.value("integration/obs/password", "").toString());
    
    // Storage settings
    m_mediaPathEdit->setText(settings.value("storage/mediaPath", "").toString());
}

void SettingsPanel::saveSettings()
{
    QSettings settings;
    
    // Automation settings
    settings.setValue("automation/alwaysPlayLatest", m_alwaysPlayLatest->isChecked());
    settings.setValue("automation/shuffleAds", m_shuffleAds->isChecked());
    settings.setValue("automation/fillGapsWithIds", m_fillGapsWithIds->isChecked());
    settings.setValue("automation/fillGapsWithBumpers", m_fillGapsWithBumpers->isChecked());
    settings.setValue("automation/autoAdvance", m_autoAdvance->isChecked());
    settings.setValue("automation/loopAtEnd", m_loopAtEnd->isChecked());
    
    // Playback settings
    settings.setValue("playback/frameRate", m_frameRateSpin->value());
    settings.setValue("playback/toleranceFrames", m_toleranceSpin->value());
    
    // Integration settings
    settings.setValue("integration/obs/host", m_obsHostEdit->text());
    settings.setValue("integration/obs/port", m_obsPortSpin->value());
    settings.setValue("integration/obs/password", m_obsPasswordEdit->text());
    
    // Storage settings
    settings.setValue("storage/mediaPath", m_mediaPathEdit->text());
    
    Logger::instance()->info("Settings saved");
}

void SettingsPanel::onOk()
{
    saveSettings();
    accept();
}

void SettingsPanel::onApply()
{
    saveSettings();
}

void SettingsPanel::onBrowseMediaPath()
{
    QString path = QFileDialog::getExistingDirectory(this, tr("Select Media Path"), m_mediaPathEdit->text());
    if (!path.isEmpty()) {
        m_mediaPathEdit->setText(path);
    }
}

