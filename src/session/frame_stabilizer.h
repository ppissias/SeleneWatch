#pragma once

#include "core/frame.h"
#include "detection/flash_detector.h"
#include "guiding/feature_lock.h"

#include <optional>
#include <string>

// Feature-50: source-thread sub-pixel shift estimator and three-tier failure
// state machine for stabilized detection.
//
// Plain stateful helper with no threads, no OpenCV, and no disk I/O so it runs
// on the source thread and can be stepped directly in unit tests.
//
// Estimator priority (Stage 1 benchmark):
//   1. The observer's active Feature Lock region (when provided), reusing
//      FeatureLock::find at full frame rate.
//   2. Otherwise automatic: clipped-limb DiskCentroid for isolated bright disks
//      surrounded by dark sky (planets, full Moon/Sun), or an auto-picked
//      FeatureLock crater patch for extended surfaces.
//
// Failure handling:
//   - Transient loss (<= graceSeconds) or implausible single-frame jump
//     (> maxStepPx): reject the bad shift and hold the last valid (dx, dy).
//   - Sustained loss (> graceSeconds): signal resetDetector so the detector
//     never compares an unshifted frame against a shifted reference, emit one
//     registration_lost log line, and continue unstabilized until
//     reacquireFrames consecutive consistent matches return.
//   - Large accumulated drift (|dx| or |dy| >= recenterMarginPx): emit an
//     in-place integer recenter step (even on raw Bayer mosaics) so the
//     detector keeps its learned interior model with zero blind gap.
//   - Multi-hour slow field rotation and lunar lighting drift: periodic
//     anchor-preserving template refresh for FeatureLock keeps correlation
//     high with zero step discontinuity.
class FrameStabilizer
{
public:
    static constexpr double kDefaultMaxStepPx = 12.0;
    static constexpr double kDefaultGraceSeconds = 0.5;
    static constexpr int kDefaultReacquireFrames = 3;
    static constexpr double kDefaultRecenterMarginPx = 32.0;
    static constexpr double kDefaultTemplateRefreshSeconds = 60.0;

    struct StepResult
    {
        FrameShift shift;
        bool resetDetector{false};
        int recenterStepX{0};
        int recenterStepY{0};
        // Non-empty only on state transitions suitable for session.txt
        // (e.g. "registration_locked method=...", "registration_lost reason=...").
        std::string logEvent;
    };

    void reset();

    // Steps one frame on the source thread. frameIntervalSec is clamped to a
    // sensible positive range if the caller passes 0 or a large gap.
    StepResult step(const Frame& frame, double frameIntervalSec,
        std::optional<LockRegion> preferredLockRegion = std::nullopt);

    [[nodiscard]] StabilizationState state() const { return state_; }
    [[nodiscard]] StabilizationMethod method() const { return method_; }
    [[nodiscard]] StabilizationSource source() const { return source_; }
    [[nodiscard]] LockRegion activeLockRegion() const { return lock_ ? lock_->region() : LockRegion{}; }

    // Tunable limits (defaults match Stage 1 verified parameters; exposed for tests).
    double maxStepPx{kDefaultMaxStepPx};
    double graceSeconds{kDefaultGraceSeconds};
    int reacquireFrames{kDefaultReacquireFrames};
    double recenterMarginPx{kDefaultRecenterMarginPx};
    double templateRefreshSeconds{kDefaultTemplateRefreshSeconds};

private:
    struct Measurement
    {
        bool valid{false};
        double dx{0};
        double dy{0};
        double score{0};
        double elapsedMs{0};
        std::string reason;
    };

    [[nodiscard]] bool sameLayout(const Frame& frame) const;
    bool initEstimator(const Frame& frame, std::optional<LockRegion> preferredLockRegion);
    bool initDiskCentroid(const Frame& frame);
    bool initFeatureLock(const Frame& frame, std::optional<LockRegion> preferredLockRegion);
    Measurement measure(const Frame& frame, double expectedDx, double expectedDy, int searchRadiusPx) const;
    Measurement measureDiskCentroid(const Frame& frame) const;
    bool refreshFeatureLock(const Frame& frame, double currentDx, double currentDy);
    void adoptPreferredRegion(const Frame& frame, LockRegion region, StepResult& out);
    void populateLockBounds(FrameShift& shift) const;

    int width_{0}, height_{0}, bits_{0};
    PixelFormat format_{PixelFormat::Mono8};
    BitAlignment alignment_{BitAlignment::Low};
    BayerPattern bayer_{BayerPattern::None};

    StabilizationState state_{StabilizationState::Off};
    StabilizationMethod method_{StabilizationMethod::None};
    StabilizationSource source_{StabilizationSource::None};
    bool initialized_{false};

    // DiskCentroid anchor
    double refCx_{0}, refCy_{0}, refWeight_{0};

    // FeatureLock anchor
    std::optional<FeatureLock> lock_;
    double anchorDx_{0}, anchorDy_{0};
    bool preferredApplied_{false};
    LockRegion appliedPreferred_{};

    // Running state
    double lastDx_{0}, lastDy_{0};
    double graceElapsedSec_{0};
    double sinceRefreshSec_{0};
    int reacquireStreak_{0};
    double reacquireDx_{0}, reacquireDy_{0};
};
