#pragma once

#include "guiding/feature_lock.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

enum class LockState
{
    Off,
    Locked,
    // Valid matches stopped for longer than kLockLostSeconds. The search is
    // wider and the lock resumes when two matches agree.
    Searching,
    // The wide search found nothing for kSearchSeconds. Nothing is measured
    // until the observer locks again.
    Lost
};

[[nodiscard]] const char* lockStateName(LockState state);

// What the lock knows after its latest step. A copy goes to the window.
struct LockStatus
{
    LockState state{LockState::Off};
    LockRegion region;
    int binning{1};
    LockQuality quality;

    // The latest search, valid or not. Empty before the first one.
    std::optional<LockMatch> lastMatch;
    // Where the feature was last found, relative to its lock position.
    double foundDx{0}, foundDy{0};
    // The drift estimate: the median of the valid offsets of the last
    // kEstimateSeconds. Decisions use this, never one measurement.
    bool estimateValid{false};
    double estimateDx{0}, estimateDy{0};
    int estimateSamples{0};

    // The frame of the latest step.
    uint64_t frameIndex{0};
    std::chrono::system_clock::time_point frameTime{};
    // Why the lock was refused, ended, or lost. Empty otherwise.
    std::string message;
};

// The state of one lock, stepped with frames: the template, where the
// feature was last found, the recent offsets and their median, and the
// lost-lock rule. Time is the frames' own time, so a recording played fast
// behaves as it did at the telescope. Plain data: no thread and no clock of
// its own, so a test can step it.
class LockTracker
{
public:
    // Frames closer together than this in source time are skipped.
    static constexpr double kMeasureIntervalSeconds = 0.2;
    static constexpr double kEstimateSeconds = 3.0;
    static constexpr int kMinEstimateSamples = 3;
    static constexpr double kLockLostSeconds = 2.0;
    static constexpr double kSearchSeconds = 60.0;
    // Search radius in frame pixels while locked and while searching.
    static constexpr int kTrackRadius = 24;
    static constexpr int kSearchRadius = 64;
    // Two matches this close resume a lost lock.
    static constexpr double kReacquirePixels = 3.0;

    // Takes the template from frame. False, with the reason in the status,
    // when the region cannot be locked; an earlier lock is then released.
    bool lock(const Frame& frame, LockRegion region);
    void unlock(std::string reason = {});

    // The next frame does not follow the last one (a loop restarting, or a
    // stream started again). The estimate starts afresh; the lock stays.
    void discontinuity();

    // The image has moved on purpose (a guide pulse): only matches from now
    // on count towards the estimate. The lock and its timers stay.
    void restartEstimate() { clearEstimate(); }

    // Searches frame when it is due. True when a search was made.
    bool step(const Frame& frame);

    [[nodiscard]] const LockStatus& status() const { return status_; }
    [[nodiscard]] bool active() const { return lock_.has_value() && status_.state != LockState::Lost; }

private:
    struct Offset
    {
        std::chrono::system_clock::time_point time;
        double dx, dy;
    };

    void updateEstimate(std::chrono::system_clock::time_point now);
    void clearEstimate();

    std::optional<FeatureLock> lock_;
    LockStatus status_;
    std::deque<Offset> offsets_;
    std::optional<std::chrono::system_clock::time_point> lastStep_;
    std::chrono::system_clock::time_point lastValid_{};
    std::chrono::system_clock::time_point searchingSince_{};
    // The first of two agreeing matches while searching.
    std::optional<LockMatch> candidate_;
    bool restart_{false};
};
