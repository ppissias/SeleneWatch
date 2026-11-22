#pragma once

#include "guiding/guide_step.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Measures which way and how fast the image moves for each pulse direction.
//
// With the lock held and guiding off: first measure the field's own unpulsed
// drift (reusing LockTracker's live fit if already long enough, otherwise
// watching unpulsed for kDriftMeasureSeconds); then pulse west until the
// drift-corrected motion reaches kTargetPixels, return east, take up
// declination backlash, pulse north less drift, and return south. Measuring
// drift first prevents fast Dec drift from cutting the west leg short and
// keeps RA backlash on the east return out of the drift estimate (Feature-43).
//
// Fails, with the reason, when the feature does not move, when the two axes
// are far from perpendicular, when the lock is lost, when the mount changes
// pier side, or when the mount may not be pulsed. A plain state machine
// stepped with observations: no thread and no clock of its own.
class Calibrator
{
public:
    static constexpr double kDriftMeasureSeconds = 10.0;
    static constexpr int kMinDriftSamples = 5;
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

    enum class Phase { Drift, WestStart, West, WestEnd, East, EastEnd, BacklashStart, Backlash, NorthStart, North, NorthEnd, South, Done, Failed };

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

    Phase phase_{Phase::Drift};
    std::string failure_;
    std::optional<GuideCalibration> result_;
    std::optional<PierSide> pierSide_;

    std::optional<std::chrono::steady_clock::time_point> pulseEnd_;
    std::chrono::steady_clock::time_point settleUntil_{};
    bool awaitingFresh_{false};
    std::vector<Point> driftSamples_;

    Point westStart_{}, westEnd_{}, backlashStart_{}, northStart_{};
    int legMs_{0}, westMs_{0}, eastMs_{0}, backlashMs_{0}, northMs_{0}, southMs_{0};
    ImageVector west_, drift_;
};
