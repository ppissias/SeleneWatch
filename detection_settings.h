#pragma once

#include <array>
#include <cmath>

// A rectangle of the frame, in frame pixels, that the detector does not test.
struct MaskRect
{
    int x{0}, y{0}, width{0}, height{0};
    bool operator==(const MaskRect&) const = default;
};

// The areas the detector leaves out (Feature-11, Sky mode): the ground,
// trees, street lights. A fixed array keeps the settings cheap to copy,
// since every measurement carries the settings that made it.
struct DetectionMask
{
    static constexpr int kMaxRects = 16;
    std::array<MaskRect, kMaxRects> rects{};
    int count{0};

    [[nodiscard]] bool empty() const { return count == 0; }
    [[nodiscard]] bool valid() const
    {
        if (count < 0 || count > kMaxRects) return false;
        for (int i = 0; i < count; ++i)
            if (rects[i].x < 0 || rects[i].y < 0 || rects[i].width < 1 || rects[i].height < 1) return false;
        return true;
    }
    bool operator==(const DetectionMask&) const = default;
};

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

    [[nodiscard]] bool valid() const
    {
        return std::isfinite(sigma) && sigma > 0 && minPixels >= 1 && maxPixels >= minPixels
            && std::isfinite(maxPatchBoxRatio) && maxPatchBoxRatio >= 1.0
            && std::isfinite(referenceTimeConstantSeconds) && referenceTimeConstantSeconds > 0
            && std::isfinite(minNoiseAdu) && minNoiseAdu > 0 && minNoiseAdu <= 65535.0
            && std::isfinite(brighteningShare) && brighteningShare >= 0 && brighteningShare <= 1
            && std::isfinite(releaseSeconds) && releaseSeconds >= 0 && mask.valid()
            && maxDarkPixels >= 1 && std::isfinite(maxDarkBoxRatio) && maxDarkBoxRatio >= 1.0
            && std::isfinite(darkeningShare) && darkeningShare >= 0 && darkeningShare <= 1;
    }
    bool operator==(const DetectionSettings&) const = default;
};
