#pragma once

#include "session_end.h"

#include <QtWidgets/QDialog>

#include <chrono>

class QCheckBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QRadioButton;
class QTimeEdit;

// Feature-32: Ends → Change…. The conditions that end a session, each with
// the time it would be met, the first of them, and what the mount does
// then. It edits a copy; the window takes the result on OK.
class SessionEndDialog final : public QDialog
{
public:
    // What the dialog needs to show times and choices.
    struct Context
    {
        ObservingSite site;
        // The Moon condition is offered (the Moon, a Moon time-lapse).
        bool moon{true};
        // The mount choice is shown only with a mount connected.
        bool mountConnected{false};
        bool canSetTracking{true};
        // The running session's start, or now.
        std::chrono::system_clock::time_point start{std::chrono::system_clock::now()};
        bool running{false};
        QTimeZone zone{QTimeZone::systemTimeZone()};
    };

    SessionEndDialog(const SessionEndSettings& settings, MountAtEnd mount, Context context, QWidget* parent = nullptr);

    [[nodiscard]] SessionEndSettings settings() const;
    [[nodiscard]] MountAtEnd mountAtEnd() const;

private:
    void update();

    Context context_;
    MountAtEnd initialMount_;
    QCheckBox* atTime_;
    QTimeEdit* time_;
    QLabel* timeReading_;
    QCheckBox* after_;
    QTimeEdit* duration_;
    QLabel* durationReading_;
    QCheckBox* atDawn_;
    QDoubleSpinBox* sunLimit_;
    QLabel* dawnReading_;
    QCheckBox* moonBelow_;
    QDoubleSpinBox* moonLimit_;
    QLabel* moonReading_;
    QLabel* result_;
    QGroupBox* mount_;
    QRadioButton* stopTracking_;
    QRadioButton* keepTracking_;
    QLabel* keepWarning_;
};
