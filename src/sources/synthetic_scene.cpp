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

namespace {

struct SkyStar
{
    float xFrac;
    float yFrac;
    float amplitude;
    float sigma;
};

const std::array<SkyStar, 36>& skyStars()
{
    static const std::array<SkyStar, 36> list = [] {
        std::array<SkyStar, 36> out{};
        uint32_t state = 20261119u;
        auto next = [&] {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        };
        for (size_t i = 0; i < out.size(); ++i)
        {
            out[i] = SkyStar{
                0.04f + next() * 0.92f,
                0.04f + next() * 0.72f,
                55.0f + next() * 135.0f,
                0.75f + next() * 0.65f};
        }
        return out;
    }();
    return list;
}

// Fast zero-mean pseudo-Gaussian sensor noise (std dev ~2.0 ADU) per pixel/frame.
int skyPixelNoise(int x, int y, uint64_t index)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u
        + static_cast<uint32_t>(y) * 668265263u
        + static_cast<uint32_t>(index) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    const int a = static_cast<int>(h & 7u);
    const int b = static_cast<int>((h >> 3) & 7u);
    const int c = static_cast<int>((h >> 6) & 7u);
    // Sum of 3 uniforms on [0,7] -> range [0,21], mean 10.5, std dev ~3.97; divide by 2 -> std dev ~2.0.
    return (a + b + c - 10) / 2;
}

} // namespace

void paintSyntheticSkyFrame(Frame& frame, int width, int height, uint64_t index, bool flash)
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

    // Horizon contour (trees and roofline along bottom ~18% of frame).
    std::vector<int> horizon(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x)
    {
        const float xf = static_cast<float>(x) / static_cast<float>(std::max(1, width));
        const float ridge = 0.82f + 0.035f * std::sin(xf * 9.5f)
            + 0.02f * std::cos(xf * 23.0f) + 0.015f * std::sin(xf * 51.0f);
        // A small flat roof notch on the left side.
        const float roof = (xf > 0.12f && xf < 0.24f) ? 0.79f : ridge;
        horizon[static_cast<size_t>(x)] = std::clamp(static_cast<int>(height * std::min(ridge, roof)), 0, height);
    }

    for (int y = 0; y < height; ++y)
    {
        uint8_t* row = &frame.data[static_cast<size_t>(y) * width];
        const int skyBase = 18 + (10 * y) / std::max(1, height);
        for (int x = 0; x < width; ++x)
        {
            const int base = (y >= horizon[static_cast<size_t>(x)]) ? 7 : skyBase;
            const int val = base + skyPixelNoise(x, y, index);
            row[x] = static_cast<uint8_t>(std::clamp(val, 0, 255));
        }
    }

    // Steady stars in the unmasked sky.
    for (const SkyStar& star : skyStars())
    {
        const float cx = star.xFrac * width;
        const float cy = star.yFrac * height;
        const int r = static_cast<int>(std::ceil(star.sigma * 3.0f));
        const int x0 = std::max(0, static_cast<int>(std::floor(cx)) - r);
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(cx)) + r);
        const int y0 = std::max(0, static_cast<int>(std::floor(cy)) - r);
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(cy)) + r);
        const float invTwoSigSq = 0.5f / (star.sigma * star.sigma);
        for (int y = y0; y <= y1; ++y)
        {
            if (y >= horizon[static_cast<size_t>(std::clamp(static_cast<int>(cx), 0, width - 1))]) continue;
            uint8_t* row = &frame.data[static_cast<size_t>(y) * width];
            const float dy = y - cy;
            for (int x = x0; x <= x1; ++x)
            {
                const float dx = x - cx;
                const int add = static_cast<int>(std::lround(star.amplitude * std::exp(-(dx * dx + dy * dy) * invTwoSigSq)));
                if (add > 0) row[x] = static_cast<uint8_t>(std::min(255, static_cast<int>(row[x]) + add));
            }
        }
    }

    if (!flash) return;

    // Built-in fixed flash for Sky mode: a bright diagonal meteor streak in the upper sky.
    const int xStart = width / 4;
    const int yStart = height / 4;
    const int segLen = std::max(16, width / 18);
    for (int t = 0; t <= segLen; ++t)
    {
        const int sx = xStart + t;
        const int sy = yStart + t / 2;
        for (int dy = -2; dy <= 2; ++dy)
        {
            const int yy = sy + dy;
            if (yy < 0 || yy >= height) continue;
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int xx = sx + dx;
                if (xx < 0 || xx >= width) continue;
                const float d2 = static_cast<float>(dx * dx + dy * dy);
                const int add = static_cast<int>(std::lround(180.0f * std::exp(-0.5f * d2)));
                uint8_t& px = frame.data[static_cast<size_t>(yy) * width + xx];
                px = static_cast<uint8_t>(std::min(255, static_cast<int>(px) + add));
            }
        }
    }
}
