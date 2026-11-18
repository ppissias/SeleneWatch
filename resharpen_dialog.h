#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse/video_exporter.h"

#include <QtWidgets/QDialog>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

class ResharpenDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ResharpenDialog(QString timelapsesRoot, const QString& initialSessionDir = {},
        QWidget* parent = nullptr);
    ~ResharpenDialog() override;

    void openSession(const QString& sessionDir);

signals:
    void profilesChanged();

private:
    void loadSelectedStack();
    void updatePreview();
    void refreshSavedProfiles();
    [[nodiscard]] SharpeningSettings currentSharpening() const;
    [[nodiscard]] VideoToneSettings currentTone() const;
    void setControlsFromSettings(const SharpeningSettings& sharp, const VideoToneSettings& tone);
    void onApplyAll();
    void setBusy(bool busy);

    QString timelapsesRoot_;
    QString sessionDir_;
    QString lastOutputDir_;
    std::vector<QString> stackFiles_;
    std::optional<FitsImage> loadedRaw_;
    std::vector<SavedSharpeningProfile> savedProfiles_;
    bool updating_{false};

    QLabel* sessionLabel_{nullptr};
    QPushButton* browseBtn_{nullptr};
    QComboBox* stackCombo_{nullptr};
    QComboBox* profileCombo_{nullptr};
    QPushButton* saveProfileBtn_{nullptr};
    QPushButton* deleteProfileBtn_{nullptr};

    QDoubleSpinBox* radius_{nullptr};
    QDoubleSpinBox* amount_{nullptr};
    QDoubleSpinBox* threshold_{nullptr};
    QDoubleSpinBox* denoise_{nullptr};

    QCheckBox* autoStretch_{nullptr};
    QDoubleSpinBox* blackLevel_{nullptr};
    QDoubleSpinBox* whiteLevel_{nullptr};
    QDoubleSpinBox* gamma_{nullptr};
    QCheckBox* brightnessLevel_{nullptr};
    QDoubleSpinBox* minQualityPct_{nullptr};
    QCheckBox* cropOverlap_{nullptr};
    QSpinBox* fps_{nullptr};
    QCheckBox* writeMp4_{nullptr};
    QCheckBox* writeGif_{nullptr};
    QCheckBox* rebuildVideoOnly_{nullptr};

    QLabel* beforeLabel_{nullptr};
    QLabel* afterLabel_{nullptr};
    QProgressBar* progressBar_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* applyBtn_{nullptr};
    QPushButton* cancelBtn_{nullptr};
    QPushButton* openDirBtn_{nullptr};

    std::atomic<bool> cancelFlag_{false};
    std::jthread worker_;
};
