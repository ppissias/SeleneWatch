#include "recording/frame_timing.h"

#include <algorithm>
#include <cmath>

void FrameTimingMeter::add(const Frame& frame)
{
    if (started_)
    {
        const double utcMs = std::chrono::duration<double, std::milli>(frame.timestamp - lastUtc_).count();
        const double steadyMs = std::chrono::duration<double, std::milli>(frame.steadyTimestamp - lastSteady_).count();
        const double interval = hostClock_ ? steadyMs : utcMs;
        // Welford's running mean and variance.
        ++count_;
        const double delta = interval - mean_;
        mean_ += delta / static_cast<double>(count_);
        sumSquares_ += delta * (interval - mean_);
        min_ = count_ == 1 ? interval : std::min(min_, interval);
        max_ = count_ == 1 ? interval : std::max(max_, interval);
        if (hostClock_) largestStep_ = std::max(largestStep_, std::abs(utcMs - steadyMs));
    }
    started_ = true;
    lastUtc_ = frame.timestamp;
    lastSteady_ = frame.steadyTimestamp;
}

FrameTiming FrameTimingMeter::result() const
{
    FrameTiming timing;
    timing.hostClock = hostClock_;
    timing.intervals = count_;
    if (count_ == 0) return timing;
    timing.meanMs = mean_;
    timing.sdMs = count_ > 1 ? std::sqrt(sumSquares_ / static_cast<double>(count_ - 1)) : 0;
    timing.minMs = min_;
    timing.maxMs = max_;
    if (hostClock_) timing.largestClockStepMs = largestStep_;
    return timing;
}

FrameTiming measureFrameTiming(const std::vector<FramePtr>& frames, bool hostClock)
{
    FrameTimingMeter meter(hostClock);
    for (const FramePtr& frame : frames) meter.add(*frame);
    return meter.result();
}

QString timingText(const FrameTiming& timing)
{
    const QString clock = timing.hostClock ? "host monotonic clock" : "frame times";
    if (timing.intervals == 0) return QString("clock=\"%1\" intervals=0").arg(clock);
    QString text = QString("clock=\"%1\" intervals=%2 mean_ms=%3 sd_ms=%4 min_ms=%5 max_ms=%6")
        .arg(clock).arg(timing.intervals).arg(timing.meanMs, 0, 'f', 3).arg(timing.sdMs, 0, 'f', 3)
        .arg(timing.minMs, 0, 'f', 3).arg(timing.maxMs, 0, 'f', 3);
    if (timing.largestClockStepMs)
        text += QString(" largest_utc_minus_monotonic_step_ms=%1").arg(*timing.largestClockStepMs, 0, 'f', 3);
    return text;
}
