#pragma once

#include <cmath>
#include <cstddef>

// Capture durations use source time, not playback speed or frame counts.
// The byte ceiling includes shared pixels, detector arrays, and previews.
struct CaptureSettings
{
    double historySeconds{5};
    double postSeconds{3};
    double maxPostSeconds{10};
    size_t memoryBytes{size_t{2} * 1024 * 1024 * 1024};
    [[nodiscard]] bool valid() const
    {
        return std::isfinite(historySeconds) && historySeconds >= 0 && historySeconds <= 600
            && std::isfinite(postSeconds) && postSeconds > 0
            && std::isfinite(maxPostSeconds) && maxPostSeconds >= postSeconds && maxPostSeconds <= 3600
            && memoryBytes > 0;
    }
};
