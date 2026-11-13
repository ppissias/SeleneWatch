#pragma once

#include "frame.h"

// Centre and radius of the painted lunar disk. Detection has no special
// knowledge of this test scene and examines the whole frame.
struct LunarDisk
{
    float centerX{0};
    float centerY{0};
    float radius{0};

    static LunarDisk centered(int width, int height)
    {
        const int shortSide = width < height ? width : height;
        return LunarDisk{
            static_cast<float>(width) * 0.5f,
            static_cast<float>(height) * 0.5f,
            static_cast<float>(shortSide) * 0.38f};
    }

    [[nodiscard]] bool contains(int x, int y) const
    {
        const float dx = static_cast<float>(x) - centerX;
        const float dy = static_cast<float>(y) - centerY;
        return dx * dx + dy * dy <= radius * radius;
    }
};

// Marker and flash painted onto that disk. Tests use the same numbers.
// Five pixels: below Feature-9's default minimum patch of eight. This is a
// visible pacing marker, not a second flash in the synthetic calibration scene.
inline constexpr int kMarkerRadius = 1;
inline constexpr int kMarkerStepPx = 3;
inline constexpr float kMarkerYFraction = 0.45f;
inline constexpr int kFlashRadius = 8;
inline constexpr int kFlashGray = 255;
inline constexpr int kFlashFrames = 3;
inline constexpr float kFlashCenterYOffset = 0.45f;

// Fills one synthetic lunar image, including the moving marker.
// flash paints the injected spot. Timestamps are left to the caller.
void paintSyntheticFrame(Frame& frame, int width, int height, uint64_t index, bool flash);
