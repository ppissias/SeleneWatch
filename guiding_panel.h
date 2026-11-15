#pragma once

#include "guiding/lock_tracker.h"

#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;

// Shows the feature lock: its state, the drift estimate, the latest match,
// and why a lock was refused or lost. It holds no lock state; the window
// passes each status the guider publishes and forwards the two requests.
class GuidingPanel final : public QWidget
{
    Q_OBJECT
public:
    // The side of the box locked around a selected pixel, in frame pixels.
    static constexpr int kDefaultLockSide = 64;

    explicit GuidingPanel(QWidget* parent = nullptr);
    void setStatus(const LockStatus& status);
    // Lock at selection needs a running stream and a selected pixel.
    void setAvailable(bool running, bool pixelSelected);
    void setLogFailed(bool failed);

signals:
    void lockAtSelectionRequested();
    void unlockRequested();

private:
    QLabel* state_;
    QLabel* drift_;
    QLabel* match_;
    QLabel* region_;
    QLabel* message_;
    QLabel* logProblem_;
    QPushButton* lockButton_;
    QPushButton* unlockButton_;
    bool locked_{false};
};
