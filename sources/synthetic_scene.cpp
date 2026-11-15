#include "sources/synthetic_scene.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

// A crater-like feature: a dark floor and a bright rim. Position and radius
// are fractions of the disk radius, from the disk centre.
struct Crater
{
    float x, y, radius;
};

// Fixed pseudo-random craters in a ring around the centre. They keep away
// from the centre, where tests place blips and probes, and from the flash.
const std::array<Crater, 14>& craters()
{
    static const std::array<Crater, 14> list = [] {
        std::array<Crater, 14> out{};
        uint32_t state = 20261115u;
        auto next = [&] {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        };
        size_t count = 0;
        while (count < out.size())
        {
            const Crater crater{next() * 1.7f - 0.85f, next() * 1.7f - 0.85f, 0.04f + next() * 0.08f};
            const float fromCentre = std::hypot(crater.x, crater.y);
            const float fromFlash = std::hypot(crater.x, crater.y - kFlashCenterYOffset);
            if (fromCentre < 0.25f || fromCentre + crater.radius > 0.85f || fromFlash < 0.25f) continue;
            out[count++] = crater;
        }
        return out;
    }();
    return list;
}

} // namespace

void paintSyntheticFrame(Frame& frame, int width, int height, uint64_t index, bool flash, double offsetX, double offsetY)
{
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::Mono8;
    frame.significantBits = 8;
    frame.alignment = BitAlignment::Low;
    frame.bayer = BayerPattern::None;
    frame.frameIndex = index;
    frame.blip.reset();
    frame.syntheticFlash = flash;
    frame.testSource = false;

    const size_t targetSize = static_cast<size_t>(width) * height;
    if (frame.data.size() != targetSize)
    {
        frame.data.resize(targetSize);
    }

    LunarDisk disk = LunarDisk::centered(width, height);
    disk.centerX += static_cast<float>(offsetX);
    disk.centerY += static_cast<float>(offsetY);
    const float radiusSq = disk.radius * disk.radius;
    const float sceneX = static_cast<float>(offsetX);
    const float sceneY = static_cast<float>(offsetY);

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

                // Subtle textural details (sinusoidal pattern simulating terrain), moving with the disk.
                const float u = x - sceneX;
                const float v = y - sceneY;
                const float texture = 0.85f + 0.15f * std::sin(u * 0.08f) * std::cos(v * 0.08f)
                    + 0.08f * std::sin((u + v) * 0.03f);

                const float intensity = 170.0f * limbShading * texture;
                row[x] = static_cast<uint8_t>(std::clamp(intensity, 15.0f, 240.0f));
            }
            else
            {
                // Deep sky background with slight sensor noise.
                const uint8_t noise = static_cast<uint8_t>((x * 17 + y * 31 + index) % 7);
                row[x] = 10 + noise;
            }
        }
    }

    // Craters darken their floors and brighten their rims, inside the disk only.
    for (const Crater& crater : craters())
    {
        const float cx = disk.centerX + crater.x * disk.radius;
        const float cy = disk.centerY + crater.y * disk.radius;
        const float r = crater.radius * disk.radius;
        if (r < 1.0f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - 1.4f * r)));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(cx + 1.4f * r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - 1.4f * r)));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(cy + 1.4f * r)));
        for (int y = y0; y <= y1; ++y)
        {
            uint8_t* row = &frame.data[static_cast<size_t>(y) * static_cast<size_t>(width)];
            for (int x = x0; x <= x1; ++x)
            {
                const float ddx = x - disk.centerX;
                const float ddy = y - disk.centerY;
                if (ddx * ddx + ddy * ddy > radiusSq) continue;
                const float d = std::hypot(x - cx, y - cy) / r;
                const float floor = 0.45f * std::exp(-(d / 0.6f) * (d / 0.6f));
                const float rim = 0.35f * std::exp(-((d - 1.0f) / 0.18f) * ((d - 1.0f) / 0.18f));
                row[x] = static_cast<uint8_t>(std::clamp(std::round(row[x] * (1.0f - floor + rim)), 15.0f, 240.0f));
            }
        }
    }

    // The pacing marker stays fixed in the frame.
    for (int y = std::max(0, movingY - kMarkerRadius); y <= std::min(height - 1, movingY + kMarkerRadius); ++y)
    {
        for (int x = std::max(0, movingX - kMarkerRadius); x <= std::min(width - 1, movingX + kMarkerRadius); ++x)
        {
            const int testDx = x - movingX;
            const int testDy = y - movingY;
            if (testDx * testDx + testDy * testDy <= kMarkerRadius * kMarkerRadius)
                frame.data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = 255;
        }
    }

    if (!flash)
    {
        return;
    }

    // Fixed spot on the disk, below the moving marker's horizontal track.
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
