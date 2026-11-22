#include "guiding/lock_tracker.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace {

double seconds(std::chrono::system_clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : 0.5 * (values[middle - 1] + values[middle]);
}

} // namespace

const char* lockStateName(LockState state)
{
    switch (state)
    {
    case LockState::Off:
        return "off";
    case LockState::Locked:
        return "locked";
    case LockState::Searching:
        return "lock lost, searching";
    case LockState::Lost:
        return "lock lost";
    }
    return "unknown";
}

bool LockTracker::lock(const Frame& frame, LockRegion region)
{
    std::string refusal;
    std::optional<FeatureLock> taken = FeatureLock::take(frame, region, refusal);
    lock_ = std::move(taken);
    clearEstimate();
    candidate_.reset();
    restart_ = false;
    status_ = {};
    status_.frameIndex = frame.frameIndex;
    status_.frameTime = frame.timestamp;
    if (!lock_)
    {
        status_.message = "Cannot lock: " + refusal;
        return false;
    }
    status_.state = LockState::Locked;
    status_.region = lock_->region();
    status_.binning = lock_->binning();
    status_.quality = lock_->quality();
    lastStep_ = frame.timestamp;
    lastValid_ = frame.timestamp;
    return true;
}

void LockTracker::unlock(std::string reason)
{
    const uint64_t frameIndex = status_.frameIndex;
    const auto frameTime = status_.frameTime;
    lock_.reset();
    clearEstimate();
    candidate_.reset();
    lastStep_.reset();
    status_ = {};
    status_.frameIndex = frameIndex;
    status_.frameTime = frameTime;
    status_.message = std::move(reason);
}

void LockTracker::discontinuity()
{
    restart_ = true;
}

bool LockTracker::step(const Frame& frame)
{
    if (!active()) return false;
    if (!lock_->sameLayout(frame))
    {
        unlock("The frame size, format, or colour pattern changed. Lock again.");
        return false;
    }

    const auto now = frame.timestamp;
    // A restarted or looped stream, or a clock that went back, starts a fresh
    // estimate and fresh timers. The search stays centred where the feature
    // was last found.
    if (restart_ || (lastStep_ && now < *lastStep_))
    {
        restart_ = false;
        clearEstimate();
        candidate_.reset();
        lastStep_.reset();
        lastValid_ = now;
        searchingSince_ = now;
    }
    if (lastStep_ && seconds(now - *lastStep_) < kMeasureIntervalSeconds) return false;
    lastStep_ = now;
    status_.frameIndex = frame.frameIndex;
    status_.frameTime = now;

    const bool searching = status_.state == LockState::Searching;
    const LockMatch match = lock_->find(frame, status_.foundDx, status_.foundDy, searching ? kSearchRadius : kTrackRadius);
    status_.lastMatch = match;

    if (!searching)
    {
        if (match.valid)
        {
            status_.foundDx = match.dx;
            status_.foundDy = match.dy;
            lastValid_ = now;
            offsets_.push_back({now, match.dx, match.dy});
        }
        else if (seconds(now - lastValid_) > kLockLostSeconds)
        {
            status_.state = LockState::Searching;
            status_.message = std::format("No valid match for {:.0f} s ({}). Searching within {} pixels.",
                kLockLostSeconds, match.reason, kSearchRadius);
            searchingSince_ = now;
            clearEstimate();
            candidate_.reset();
        }
    }
    else if (match.valid)
    {
        // One match while searching could be a look-alike. A second one at
        // the same place resumes the lock; until then the search moves there.
        if (candidate_ && std::hypot(match.dx - candidate_->dx, match.dy - candidate_->dy) <= kReacquirePixels)
        {
            status_.state = LockState::Locked;
            status_.message.clear();
            lastValid_ = now;
            offsets_.push_back({now, match.dx, match.dy});
            candidate_.reset();
        }
        else
        {
            candidate_ = match;
        }
        status_.foundDx = match.dx;
        status_.foundDy = match.dy;
    }
    else
    {
        candidate_.reset();
        if (seconds(now - searchingSince_) > searchSeconds_)
        {
            status_.state = LockState::Lost;
            status_.message = std::format("The feature was not found within {} pixels for {:.0f} s. Lock again.",
                kSearchRadius, searchSeconds_);
        }
    }
    updateEstimate(now);
    return true;
}

void LockTracker::setSearchSeconds(double seconds)
{
    searchSeconds_ = std::clamp(seconds, kMinSearchSeconds, kMaxSearchSeconds);
}

void LockTracker::updateEstimate(std::chrono::system_clock::time_point now)
{
    while (!offsets_.empty() && seconds(now - offsets_.front().time) > kEstimateSeconds) offsets_.pop_front();
    status_.estimateSamples = static_cast<int>(offsets_.size());
    status_.estimateValid = status_.state == LockState::Locked && status_.estimateSamples >= kMinEstimateSamples;
    if (!status_.estimateValid) return;
    std::vector<double> dx, dy;
    for (const Offset& offset : offsets_)
    {
        dx.push_back(offset.dx);
        dy.push_back(offset.dy);
    }
    status_.estimateDx = median(std::move(dx));
    status_.estimateDy = median(std::move(dy));
}

void LockTracker::clearEstimate()
{
    offsets_.clear();
    status_.estimateValid = false;
    status_.estimateSamples = 0;
}
