#pragma once

#include "detection/detection_mask.h"

#include <cmath>

// Observer-configurable thresholds, patch geometry limits, background-learning
// constants, and watch-specific options copied by FlashDetector before each
// frame and recorded on every trigger.
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

    // Feature-13 and Feature-45: the detector looks for pixels that fall by
    // at least K times their noise, and for nothing that rises. On only in
    // the shadows' watch. Dark patches have their own size and shape limits:
    // a plane or the ISS against the sunlit Moon is far larger than a flash.
    // Minimum patch and persistence are shared.
    bool darkening{false};
    int maxDarkPixels{20000};
    double maxDarkBoxRatio{10.0};
    // With darkening, a frame where at least this share of the unmasked
    // pixels falls by K at once fires as a large darkening. Zero is off.
    double darkeningShare{0};

    // Feature-31, for flashes on the earthlit side: pixels whose learned
    // brightness is above this share of full scale (the sunlit side, the
    // glare by the terminator) are not tested. No flash can be seen there,
    // and seeing on sunlit terrain only makes false triggers. Zero is off.
    double ignoreAboveShare{0};

    // Feature-50: opt-in sub-pixel shift stabilization inside the detector
    // (off by default; always off on fixed-camera Sky watches). Raw saved
    // frames remain untouched.
    bool stabilizeDetection{false};

    // Feature-45: the patch limits of the change the detector looks for, the
    // dark ones with darkening.
    [[nodiscard]] int patchMaxPixels() const { return darkening ? maxDarkPixels : maxPixels; }
    [[nodiscard]] double patchMaxBoxRatio() const { return darkening ? maxDarkBoxRatio : maxPatchBoxRatio; }

    [[nodiscard]] bool valid() const
    {
        return std::isfinite(sigma) && sigma > 0 && minPixels >= 1 && maxPixels >= 1 && patchMaxPixels() >= minPixels
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
