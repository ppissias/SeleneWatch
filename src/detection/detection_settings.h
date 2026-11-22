#pragma once

#include "detection/detection_mask.h"

#include <cmath>

struct DetectionSettings
{
    double sigma{6.0};
    int minPixels{8};
    int maxPixels{400};
    bool oneFrame{true};
    double maxPatchBoxRatio{4.0};
    double referenceTimeConstantSeconds{2.0};
    double minNoiseAdu{0.5};

    // Feature-11, used in Sky mode. Zero and empty leave the detector as it
    // is on the Moon.
    // A frame where at least this share of the unmasked pixels rises by K
    // fires as a large brightening (lightning, a fireball's flare).
    double brighteningShare{0};
    // A patch, or a large brightening, that keeps firing longer than this is
    // learned into the background, so a star drifting past a fixed camera
    // stops firing.
    double releaseSeconds{0};
    DetectionMask mask;

    // Feature-13, off in both modes until it is measured on recordings. A
    // pixel that falls by at least K times its noise is dark. Dark pixels
    // form their own patches, never merged with rising ones, with their own
    // size and shape limits: a plane or the ISS against the sunlit Moon is
    // far larger than a flash. Minimum patch and persistence are shared.
    bool darkening{false};
    int maxDarkPixels{20000};
    double maxDarkBoxRatio{10.0};
    // A frame where at least this share of the unmasked pixels falls by K
    // at once fires as a large darkening. Zero is off.
    double darkeningShare{0};

    // Feature-31, for flashes on the earthlit side: pixels whose learned
    // brightness is above this share of full scale (the sunlit side, the
    // glare by the terminator) are not tested. No flash can be seen there,
    // and seeing on sunlit terrain only makes false triggers. Zero is off.
    double ignoreAboveShare{0};

    [[nodiscard]] bool valid() const
    {
        return std::isfinite(sigma) && sigma > 0 && minPixels >= 1 && maxPixels >= minPixels
            && std::isfinite(maxPatchBoxRatio) && maxPatchBoxRatio >= 1.0
            && std::isfinite(referenceTimeConstantSeconds) && referenceTimeConstantSeconds > 0
            && std::isfinite(minNoiseAdu) && minNoiseAdu > 0 && minNoiseAdu <= 65535.0
            && std::isfinite(brighteningShare) && brighteningShare >= 0 && brighteningShare <= 1
            && std::isfinite(releaseSeconds) && releaseSeconds >= 0 && mask.valid()
            && maxDarkPixels >= 1 && std::isfinite(maxDarkBoxRatio) && maxDarkBoxRatio >= 1.0
            && std::isfinite(darkeningShare) && darkeningShare >= 0 && darkeningShare <= 1
            && std::isfinite(ignoreAboveShare) && ignoreAboveShare >= 0 && ignoreAboveShare <= 1;
    }
    bool operator==(const DetectionSettings&) const = default;
};
