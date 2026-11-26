#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse/video_exporter.h"

#include <QtWidgets/QDialog>

#include <atomic>
#include <map>
#include <optional>
#include <thread>
#include <vector>

class PreviewWidget;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QToolButton;

// Post-capture time-lapse re-processing dialog (Session -> Re-sharpen
// time-lapse...). Lets the observer compare synchronized Before/After previews
// while adjusting optional stack re-alignment, linear sharpening, cloud
// brightness levelling, quality filtering with per-stack overrides, and
// MP4/GIF/SER video assembly on a background worker thread without modifying
// the original raw FITS stacks.
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

protected:
    void closeEvent(QCloseEvent* event) override;

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
    void updateQualityFilterControls();
    void refreshQualityFilter();
    void stepDroppedStack(int direction);
    void onStackTableItemChanged(QTableWidgetItem* item);
    [[nodiscard]] std::map<QString, bool> currentStackOverrides() const;
    [[nodiscard]] StackRealignMode currentRealignMode() const;
    [[nodiscard]] bool isRealignActive() const;
    [[nodiscard]] int effectiveRealignRefIndex() const;
    [[nodiscard]] const FitsImage* ensureRealignReferenceLoaded(int refIdx);
    void updateRealignControls();
    void invalidateRealignCache();

    QString timelapsesRoot_;
    QString sessionDir_;
    QString lastOutputDir_;
    std::vector<QString> stackFiles_;
    std::vector<double> stackQualities_;
    std::vector<std::optional<bool>> stackManualOverrides_;
    std::vector<StackFilterDecision> filterDecisions_;
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
    std::array<double, 3> cachedBeforeChannelBlack_{-1.0, -1.0, -1.0};
    bool cachedBeforeAutoStretch_{false};
    QRect cachedBeforeCrop_;

    std::optional<QRect> realignBox_;
    int realignRefIndex_{-1};
    int realignRefRawIdx_{-1};
    std::optional<FitsImage> realignRefRaw_;
    bool cachedAlignedValid_{false};
    int cachedAlignedStackIdx_{-1};
    int cachedAlignedRefIdx_{-1};
    StackRealignMode cachedAlignedMode_{StackRealignMode::Off};
    bool cachedAlignedAllowRot_{true};
    std::optional<QRect> cachedAlignedBox_;
    SessionAlignResult cachedAlignedResult_;

    QTimer* previewTimer_{nullptr};
    QLabel* sessionLabel_{nullptr};
    QPushButton* browseBtn_{nullptr};
    QComboBox* stackCombo_{nullptr};
    QComboBox* profileCombo_{nullptr};
    QPushButton* saveProfileBtn_{nullptr};
    QPushButton* deleteProfileBtn_{nullptr};

    QComboBox* realignMode_{nullptr};
    QWidget* realignBoxRow_{nullptr};
    QToolButton* drawFeatureBoxBtn_{nullptr};
    QToolButton* clearFeatureBoxBtn_{nullptr};
    QWidget* realignRefRow_{nullptr};
    QToolButton* setRealignRefBtn_{nullptr};
    QWidget* realignOptsBox_{nullptr};
    QCheckBox* correctRotation_{nullptr};
    QCheckBox* saveAlignedStacks_{nullptr};
    QLabel* realignStatus_{nullptr};

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
    QComboBox* qualityMode_{nullptr};
    QDoubleSpinBox* minQualityPct_{nullptr};
    QDoubleSpinBox* keepBestPct_{nullptr};
    QLabel* qualitySummary_{nullptr};
    QToolButton* prevDroppedBtn_{nullptr};
    QToolButton* nextDroppedBtn_{nullptr};
    QToolButton* inspectStacksBtn_{nullptr};
    QToolButton* resetOverridesBtn_{nullptr};
    QWidget* stackListBox_{nullptr};
    QTableWidget* stackTable_{nullptr};
    bool updatingTable_{false};
    QCheckBox* cropOverlap_{nullptr};
    QSpinBox* fps_{nullptr};
    QCheckBox* writeMp4_{nullptr};
    QCheckBox* writeGif_{nullptr};
    QCheckBox* writeSer_{nullptr};
    QCheckBox* stretchSer_{nullptr};
    QWidget* stretchSerRow_{nullptr};
    QCheckBox* rebuildVideoOnly_{nullptr};

    QToolButton* zoomFitBtn_{nullptr};
    QToolButton* zoom100Btn_{nullptr};
    QToolButton* zoom200Btn_{nullptr};
    QToolButton* zoom400Btn_{nullptr};
    QLabel* zoomReadout_{nullptr};
    QGroupBox* afterBox_{nullptr};
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
