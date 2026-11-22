#pragma once

#include "guiding/guider.h"

#include <QtWidgets/QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

// The Guiding dock: the feature lock, the mount, the calibration, and
// guiding. It holds no guiding state; the window passes each status the
// guider publishes and forwards the requests. The only values it keeps are
// the observer's choices: the mount kind, the tolerance and how long a lost
// lock is searched for (Feature-32).
class GuidingPanel final : public QWidget
{
    Q_OBJECT
public:
    // The side of the box locked around a selected pixel, in frame pixels.
    static constexpr int kDefaultLockSide = 64;
    enum class MountKind { Simulated, Ascom };

    explicit GuidingPanel(QWidget* parent = nullptr);
    void setStatus(const GuiderStatus& status);
    // Lock at selection needs a running stream and a selected pixel; Draw
    // lock box needs a running stream.
    void setAvailable(bool running, bool pixelSelected);
    // Feature-28: whether the next drag on the preview draws the lock box.
    void setDrawingLock(bool on);
    [[nodiscard]] bool drawingLock() const;
    void setLogFailed(bool failed);
    void setAscomAvailable(bool available);
    void setAscomDriver(const QString& progId);
    // The frame's shorter side, to show the tolerance in pixels.
    void setFrameShortSide(int pixels);

    [[nodiscard]] MountKind mountKind() const;
    void setMountKind(MountKind kind);
    // The tolerance as a percentage of the frame's shorter side.
    [[nodiscard]] double tolerancePercent() const;
    void setTolerancePercent(double percent);
    // Feature-42: higher drift limit before giving up if corrections cannot reduce it.
    [[nodiscard]] double giveUpPercent() const;
    void setGiveUpPercent(double percent);
    // Feature-40: correction aggressiveness (30-100 %) and max pulse duration (ms).
    [[nodiscard]] int aggressivenessPercent() const;
    void setAggressivenessPercent(int percent);
    [[nodiscard]] int maxPulseMs() const;
    void setMaxPulseMs(int milliseconds);
    // Feature-32: the grace period for a lost lock while guiding, seconds.
    [[nodiscard]] double lostAfterSeconds() const;
    void setLostAfterSeconds(double seconds);

signals:
    void lockAtSelectionRequested();
    void drawLockToggled(bool on);
    void unlockRequested();
    void chooseDriverRequested();
    void connectRequested();
    void disconnectRequested();
    void calibrateRequested();
    void guidingRequested(bool on);
    void toleranceChanged();
    void correctionTuningChanged();
    void lostAfterChanged();
    void mountKindChanged();

private:
    void updateTolerance();
    void updateMountControls();

    QLabel* state_;
    QLabel* drift_;
    QLabel* driftSpeed_;
    QLabel* match_;
    QLabel* region_;
    QLabel* message_;
    QLabel* logProblem_;
    QPushButton* drawLock_;
    QPushButton* lockButton_;
    QPushButton* unlockButton_;

    QComboBox* mountKind_;
    QPushButton* chooseButton_;
    QLabel* driver_;
    QPushButton* connectButton_;
    QPushButton* disconnectButton_;
    QLabel* mountState_;

    QPushButton* calibrateButton_;
    QLabel* calibration_;
    QDoubleSpinBox* tolerance_;
    QLabel* toleranceReadout_;
    QDoubleSpinBox* giveUp_;
    QLabel* giveUpReadout_;
    QSpinBox* aggressiveness_;
    QSpinBox* maxPulse_;
    QPushButton* guideButton_;
    QSpinBox* lostAfter_;
    QLabel* guiding_;
    QLabel* guideMessage_;

    bool locked_{false};
    bool connected_{false};
    bool ascomAvailable_{false};
    GuideMode mode_{GuideMode::Idle};
    bool calibrated_{false};
    int frameShortSide_{0};
    std::optional<double> arcsecPerPixel_;
};
