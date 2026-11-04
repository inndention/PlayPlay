#ifndef SETTINGSPANEL_H
#define SETTINGSPANEL_H

#include <QDialog>
#include <QTabWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QFileDialog>

class SettingsPanel : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget *parent = nullptr);

private slots:
    void onOk();
    void onApply();
    void onBrowseMediaPath();

private:
    QWidget* createAutomationTab();
    QWidget* createPlaybackTab();
    QWidget* createIntegrationTab();
    QWidget* createStorageTab();
    
    void loadSettings();
    void saveSettings();
    
    QTabWidget* m_tabWidget;
    
    // Automation settings
    QCheckBox* m_alwaysPlayLatest;
    QCheckBox* m_shuffleAds;
    QCheckBox* m_fillGapsWithIds;
    QCheckBox* m_fillGapsWithBumpers;
    QCheckBox* m_autoAdvance;
    QCheckBox* m_loopAtEnd;
    
    // Playback settings
    QSpinBox* m_frameRateSpin;
    QSpinBox* m_toleranceSpin;
    
    // Integration settings
    QLineEdit* m_obsHostEdit;
    QSpinBox* m_obsPortSpin;
    QLineEdit* m_obsPasswordEdit;
    
    // Storage settings
    QLineEdit* m_mediaPathEdit;
    
    QPushButton* m_okButton;
    QPushButton* m_cancelButton;
    QPushButton* m_applyButton;
};

#endif // SETTINGSPANEL_H

