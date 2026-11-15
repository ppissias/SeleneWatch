#pragma once

#include <cmath>

// The observer's trigger and background-model settings. Each candidate
// retains the settings applied to its trigger frame.
struct DetectionSettings
{
    double sigma{6.0};
    int minPixels{8};
    int maxPixels{400};
    bool oneFrame{true};
    double maxPatchBoxRatio{4.0};
    double referenceTimeConstantSeconds{2.0};
    double minNoiseAdu{0.5};

    [[nodiscard]] bool valid() const
    {
        return std::isfinite(sigma) && sigma > 0 && minPixels >= 1 && maxPixels >= minPixels
            && std::isfinite(maxPatchBoxRatio) && maxPatchBoxRatio >= 1.0
            && std::isfinite(referenceTimeConstantSeconds) && referenceTimeConstantSeconds > 0
            && std::isfinite(minNoiseAdu) && minNoiseAdu > 0 && minNoiseAdu <= 65535.0;
    }
    bool operator==(const DetectionSettings&) const = default;
};
