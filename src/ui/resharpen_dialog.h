#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse/video_exporter.h"

#include <QtWidgets/QDialog>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

class PreviewWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QToolButton;

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
    void schedulePreviewUpdate();
    void refreshSavedProfiles();
    [[nodiscard]] SharpeningSettings currentSharpening() const;
    [[nodiscard]] VideoToneSettings currentTone() const;
    void setControlsFromSettings(const SharpeningSettings& sharp, const VideoToneSettings& tone);
    void onApplyAll();
    void setBusy(bool busy);
    void syncPreviewView(PreviewWidget* target, bool fitToWindow, double zoom, QPointF pan);
    void updateZoomControls(double effectiveZoom, bool fitToWindow);

    QString timelapsesRoot_;
    QString sessionDir_;
    QString lastOutputDir_;
    std::vector<QString> stackFiles_;
    int bestStackIndex_{-1};
    std::optional<QRect> sessionOverlap_;
    std::optional<FitsImage> loadedRaw_;
    std::optional<FitsImage> loadedSharpened_;
    std::optional<FitsImage> bestRaw_;
    std::optional<FitsImage> bestSharpened_;
    std::optional<FitsImage> cachedBestApplied_;
    SharpeningSettings cachedBestSharpSettings_;
    bool cachedBestAppliedValid_{false};
    std::vector<SavedSharpeningProfile> savedProfiles_;
    bool updating_{false};
    QImage cachedBeforeImg_;
    double cachedBeforeBlack_{-1.0};
    double cachedBeforeWhite_{-1.0};
    double cachedBeforeGamma_{-1.0};
    QRect cachedBeforeCrop_;

    QTimer* previewTimer_{nullptr};
    QLabel* sessionLabel_{nullptr};
    QPushButton* browseBtn_{nullptr};
    QComboBox* stackCombo_{nullptr};
    QComboBox* profileCombo_{nullptr};
    QPushButton* saveProfileBtn_{nullptr};
    QPushButton* deleteProfileBtn_{nullptr};

    QDoubleSpinBox* radius_{nullptr};
    QSlider* radiusSlider_{nullptr};
    QDoubleSpinBox* amount_{nullptr};
    QSlider* amountSlider_{nullptr};
    QDoubleSpinBox* threshold_{nullptr};
    QSlider* thresholdSlider_{nullptr};
    QDoubleSpinBox* denoise_{nullptr};
    QSlider* denoiseSlider_{nullptr};

    QCheckBox* autoStretch_{nullptr};
    QDoubleSpinBox* blackLevel_{nullptr};
    QSlider* blackSlider_{nullptr};
    QDoubleSpinBox* whiteLevel_{nullptr};
    QSlider* whiteSlider_{nullptr};
    QDoubleSpinBox* gamma_{nullptr};
    QSlider* gammaSlider_{nullptr};
    QComboBox* brightnessLevel_{nullptr};
    QDoubleSpinBox* minQualityPct_{nullptr};
    QCheckBox* cropOverlap_{nullptr};
    QSpinBox* fps_{nullptr};
    QCheckBox* writeMp4_{nullptr};
    QCheckBox* writeGif_{nullptr};
    QCheckBox* rebuildVideoOnly_{nullptr};

    QToolButton* zoomFitBtn_{nullptr};
    QToolButton* zoom100Btn_{nullptr};
    QToolButton* zoom200Btn_{nullptr};
    QToolButton* zoom400Btn_{nullptr};
    QLabel* zoomReadout_{nullptr};
    PreviewWidget* beforeView_{nullptr};
    PreviewWidget* afterView_{nullptr};
    bool syncingView_{false};
    QProgressBar* progressBar_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* applyBtn_{nullptr};
    QPushButton* cancelBtn_{nullptr};
    QPushButton* openDirBtn_{nullptr};

    std::atomic<bool> cancelFlag_{false};
    std::jthread worker_;
};
