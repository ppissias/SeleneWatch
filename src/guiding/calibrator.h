#pragma once

#include "guiding/guide_step.h"

#include <chrono>
#include <optional>
#include <string>

// Measures which way and how fast the image moves for each pulse direction.
//
// With the lock held and guiding off: settle and take a fresh estimate;
// pulse west in steps until the feature has moved kTargetPixels; take an
// estimate; pulse east for the same time and take another. The field's own
// drift is the part the two legs share, so it is removed from the west
// vector. Then north: first take up the declination backlash (pulse north
// until the feature starts to move), then measure a north leg the same way,
// less the drift, and return south.
//
// Fails, with the reason, when the feature does not move, when the two axes
// are far from perpendicular, when the lock is lost, when the mount changes
// pier side, or when the mount may not be pulsed. A plain state machine
// stepped with observations: no thread and no clock of its own.
class Calibrator
{
public:
    static constexpr int kStepMs = 500;
    static constexpr double kTargetPixels = 25;
    static constexpr double kMinMovePixels = 8;
    static constexpr int kMaxLegMs = 30000;
    static constexpr double kBacklashPixels = 4;
    static constexpr int kMaxBacklashMs = 10000;
    static constexpr double kSettleSeconds = 1.0;
    static constexpr int kFreshSamples = 8;
    static constexpr double kMaxSkewDegrees = 20;
    // A pulse the mount still reports this long after it should have ended is a fault.
    static constexpr double kPulseOverrunSeconds = 10;

    enum class Phase { WestStart, West, WestEnd, East, EastEnd, BacklashStart, Backlash, NorthStart, North, NorthEnd, South, Done, Failed };

    GuideDecision step(const GuideObservation& observation);

    [[nodiscard]] bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
    [[nodiscard]] bool failed() const { return phase_ == Phase::Failed; }
    [[nodiscard]] const std::string& failure() const { return failure_; }
    [[nodiscard]] const std::optional<GuideCalibration>& result() const { return result_; }
    // A short description of the current phase, for the window.
    [[nodiscard]] std::string progress() const;

private:
    struct Point
    {
        ImageVector position;
        std::chrono::steady_clock::time_point time;
    };

    void fail(std::string reason);
    // True while the last pulse is still running.
    bool pulseRunning(const GuideObservation& observation);
    // Settles, restarts the estimate, then returns a fresh estimate.
    std::optional<Point> fresh(const GuideObservation& observation, GuideDecision& decision);
    GuideDecision send(GuideDirection direction, int milliseconds, const GuideObservation& observation);
    [[nodiscard]] ImageVector driftSince(const Point& from, std::chrono::steady_clock::time_point now) const;

    Phase phase_{Phase::WestStart};
    std::string failure_;
    std::optional<GuideCalibration> result_;
    std::optional<PierSide> pierSide_;

    std::optional<std::chrono::steady_clock::time_point> pulseEnd_;
    std::chrono::steady_clock::time_point settleUntil_{};
    bool awaitingFresh_{false};

    Point westStart_{}, westEnd_{}, backlashStart_{}, northStart_{};
    int legMs_{0}, westMs_{0}, eastMs_{0}, backlashMs_{0}, northMs_{0}, southMs_{0};
    ImageVector west_, drift_;
};
