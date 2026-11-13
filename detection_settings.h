#pragma once

#include <cmath>

// The observer's four trigger controls. A copy travels with each candidate;
// later edits never change the settings recorded on an earlier trigger.
struct DetectionSettings
{
    double sigma{6.0};
    int minPixels{8};
    int maxPixels{400};
    bool oneFrame{true};

    [[nodiscard]] bool valid() const
    {
        return std::isfinite(sigma) && sigma > 0 && minPixels >= 1 && maxPixels >= minPixels;
    }
    bool operator==(const DetectionSettings&) const = default;
};

inline constexpr double kReferenceTimeConstantSeconds = 2.0;
inline constexpr double kMaxPatchBoxRatio = 4.0;
