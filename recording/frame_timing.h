#pragma once

#include "frame.h"

#include <QtCore/QString>

#include <cstddef>
#include <optional>
#include <vector>

// How evenly a run of frames came (Feature-14 Part B): the intervals between
// them, from the host's monotonic clock for a camera and from the frame
// times for a file, and, for a camera, the largest step between the UTC and
// monotonic clocks from one frame to the next, which shows a clock adjusted
// in the middle of the run.
struct FrameTiming
{
    bool hostClock{false};
    size_t intervals{0};
    double meanMs{0}, sdMs{0}, minMs{0}, maxMs{0};
    std::optional<double> largestClockStepMs;
};

// Adds frames one by one, for a recording too long to keep.
class FrameTimingMeter
{
public:
    explicit FrameTimingMeter(bool hostClock) : hostClock_(hostClock) {}
    void add(const Frame& frame);
    [[nodiscard]] FrameTiming result() const;

private:
    bool hostClock_;
    bool started_{false};
    std::chrono::system_clock::time_point lastUtc_{};
    std::chrono::steady_clock::time_point lastSteady_{};
    size_t count_{0};
    double mean_{0}, sumSquares_{0}, min_{0}, max_{0}, largestStep_{0};
};

[[nodiscard]] FrameTiming measureFrameTiming(const std::vector<FramePtr>& frames, bool hostClock);
// One line: the clock, the count, mean, standard deviation, least and most.
[[nodiscard]] QString timingText(const FrameTiming& timing);
