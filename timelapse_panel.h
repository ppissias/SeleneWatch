#pragma once

#include "timelapse/timelapse_settings.h"
#include "timelapse/timelapse_session.h"

#include <QtCore/QDateTime>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <deque>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QToolButton;

class TimelapsePanel : public QWidget
{
    Q_OBJECT
public:
    explicit TimelapsePanel(QWidget* parent = nullptr);

    [[nodiscard]] TimelapseSettings settings() const;
    void setSettings(const TimelapseSettings& settings);
    void refreshProfiles();

    void setStreamInfo(bool running, bool monitoring, bool recording, bool timeLapsing,
        double estimatedFps, int frameWidth, int frameHeight, int bytesPerPixel, bool hasLock,
        bool finalizing = false);

    void setLunarAnchor(const std::optional<QRect>& rect);
    [[nodiscard]] bool selectingAnchor() const;
    void cancelAnchorSelection();

    void updateStatus(const TimelapseStatus& status);
    void onSessionFinished(const TimelapseFinishResult& result);
    void clearRunVisuals();

signals:
    void startRequested();
    void stopRequested();
    void resharpenRequested(const QString& sessionDir);
    void anchorSelectionChanged(bool active);
    void anchorChanged(const std::optional<QRect>& rect);

private:
    void applyPresetDefaults(TimelapsePreset preset);
    void updateEstimates();
    void updateFilmstripFrame();
    void loadFromQSettings();
    void saveToQSettings() const;

    bool updating_{false};
    bool running_{false};
    bool monitoring_{false};
    bool recording_{false};
    bool timeLapsing_{false};
    double estimatedFps_{30.0};
    int frameWidth_{640};
    int frameHeight_{480};
    int bytesPerPixel_{1};
    bool hasLock_{false};
    std::optional<QRect> lunarAnchor_;
    std::vector<SavedSharpeningProfile> savedProfiles_;

    QFormLayout* form_{nullptr};
    QComboBox* preset_{nullptr};
    QDoubleSpinBox* interval_{nullptr};
    QDoubleSpinBox* window_{nullptr};
    QDoubleSpinBox* keepPercent_{nullptr};
    QSpinBox* maxFrames_{nullptr};
    QComboBox* sharpening_{nullptr};
    QCheckBox* cropToLock_{nullptr};
    QToolButton* selectAnchor_{nullptr};
    QPushButton* clearAnchor_{nullptr};
    QLabel* anchorLabel_{nullptr};

    QComboBox* stopKind_{nullptr};
    QDoubleSpinBox* durationMin_{nullptr};
    QDateTimeEdit* utcEnd_{nullptr};
    QSpinBox* targetStacks_{nullptr};

    QCheckBox* writeMp4_{nullptr};
    QCheckBox* writeGif_{nullptr};
    QSpinBox* videoFps_{nullptr};
    QCheckBox* saveRawSer_{nullptr};
    QLabel* estimateLabel_{nullptr};

    QPushButton* startStopButton_{nullptr};
    QPushButton* resharpenButton_{nullptr};

    QProgressBar* progressBar_{nullptr};
    QLabel* timingLabel_{nullptr};
    QLabel* statsLabel_{nullptr};
    QLabel* warningLabel_{nullptr};
    QLabel* latestThumb_{nullptr};
    QLabel* filmstripThumb_{nullptr};
    QLabel* filmstripInfo_{nullptr};
    QCheckBox* loopFilmstrip_{nullptr};

    static constexpr size_t kMaxFilmstripFrames = 200;
    static constexpr int kFilmstripThumbMaxEdge = 256;

    std::deque<QImage> filmstripFrames_;
    size_t filmstripIndex_{0};
    QTimer* filmstripTimer_{nullptr};
    QString lastSessionDir_;
    int lastSeenStacks_{0};
};
