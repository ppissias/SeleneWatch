#include "synthetic_scene.h"

#include <algorithm>
#include <cmath>

void paintSyntheticFrame(Frame& frame, int width, int height, uint64_t index, bool flash)
{
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::Mono8;
    frame.significantBits = 8;
    frame.alignment = BitAlignment::Low;
    frame.bayer = BayerPattern::None;
    frame.frameIndex = index;

    const size_t targetSize = static_cast<size_t>(width * height);
    if (frame.data.size() != targetSize)
    {
        frame.data.resize(targetSize);
    }

    const LunarDisk disk = LunarDisk::centered(width, height);
    const float radiusSq = disk.radius * disk.radius;

    const int movingX = static_cast<int>((index * kMarkerStepPx) % width);
    const int movingY = static_cast<int>(height * kMarkerYFraction);

    for (int y = 0; y < height; ++y)
    {
        const float dy = y - disk.centerY;
        const float dySq = dy * dy;
        uint8_t* row = &frame.data[static_cast<size_t>(y * width)];

        for (int x = 0; x < width; ++x)
        {
            const float dx = x - disk.centerX;
            const float distSq = dx * dx + dySq;

            if (distSq <= radiusSq)
            {
                // Simulated lunar disk with limb darkening and crater-like variations.
                const float normDist = std::sqrt(distSq) / disk.radius;
                const float limbShading = std::clamp(std::cos(normDist * 1.57079f), 0.0f, 1.0f);

                // Subtle textural details (sinusoidal pattern simulating terrain / craters).
                const float texture = 0.85f + 0.15f * std::sin(x * 0.08f) * std::cos(y * 0.08f)
                    + 0.08f * std::sin((x + y) * 0.03f);

                const float intensity = 170.0f * limbShading * texture;
                row[x] = static_cast<uint8_t>(std::clamp(intensity, 15.0f, 240.0f));
            }
            else
            {
                // Deep sky background with slight sensor noise.
                const uint8_t noise = static_cast<uint8_t>((x * 17 + y * 31 + index) % 7);
                row[x] = 10 + noise;
            }

            const int testDx = x - movingX;
            const int testDy = y - movingY;
            if (testDx * testDx + testDy * testDy <= kMarkerRadius * kMarkerRadius)
            {
                row[x] = 255;
            }
        }
    }

    if (!flash)
    {
        return;
    }

    // Fixed spot, below the moving marker's horizontal track.
    const int flashX = static_cast<int>(disk.centerX);
    const int flashY = static_cast<int>(disk.centerY + disk.radius * kFlashCenterYOffset);
    for (int y = flashY - kFlashRadius; y <= flashY + kFlashRadius; ++y)
    {
        if (y < 0 || y >= height)
        {
            continue;
        }
        uint8_t* row = &frame.data[static_cast<size_t>(y) * static_cast<size_t>(width)];
        for (int x = flashX - kFlashRadius; x <= flashX + kFlashRadius; ++x)
        {
            if (x < 0 || x >= width)
            {
                continue;
            }
            const int dx = x - flashX;
            const int dy = y - flashY;
            if (dx * dx + dy * dy <= kFlashRadius * kFlashRadius)
            {
                row[x] = static_cast<uint8_t>(kFlashGray);
            }
        }
    }
}
