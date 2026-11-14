#pragma once

#include "blip.h"
#include "memory_budget.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

enum class PixelFormat
{
    Mono8,

    // One little-endian uint16_t per pixel. Also holds 10-, 12-, and 14-bit
    // data, exactly as the source delivered it; significantBits and
    // alignment say where the data sits in the word.
    Mono16,

    RGB888
};

// Where the significant bits of a Mono16 sample sit in its 16-bit word.
// A verified source may put 12-bit data in high or low bits. Unverified camera
// packing uses all 16 delivered bits; sampled zeros alone do not prove padding.
enum class BitAlignment
{
    Low,
    High
};

// The colour filter of a raw colour sensor, named by its top-left 2x2 cell.
// None for a mono sensor and for data that is already RGB.
enum class BayerPattern
{
    None,
    RGGB,
    GRBG,
    GBRG,
    BGGR
};

// Represents a single image frame acquired from a camera or test source.
//
// A frame from a pool keeps whatever its previous user left in every field,
// so a source sets all of them for each frame it fills.
struct Frame
{
    explicit Frame(MemoryBudget budget = {}) : data(BudgetAllocator<uint8_t>(std::move(budget))) {}
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Mono8;

    // Interpreted sample width: 8 for Mono8 and RGB888, up to 16
    // for Mono16. Sources with verified packing can declare 12 high/low;
    // an unverified camera conservatively interprets all 16 delivered bits.
    // Its separate sensor ADC depth and packing policy are in StreamInfo.
    int significantBits = 8;
    BitAlignment alignment = BitAlignment::Low;
    BayerPattern bayer = BayerPattern::None;

    uint64_t frameIndex = 0;

    std::chrono::system_clock::time_point timestamp{};
    std::chrono::steady_clock::time_point steadyTimestamp{};

    // Present only on frames to which the Session applied a test blip.
    std::optional<BlipMark> blip;

    // The synthetic source's original fixed flash, kept as a pipeline check.
    bool syntheticFlash{false};

    // An explicitly marked TEST source or replayed TEST SER. For a replay,
    // individual injection details remain in the original clip's sidecar.
    bool testSource{false};
    // SDK cumulative loss observed when this camera frame was delivered.
    // Not a hardware exposure counter; absent for recordings/synthetic data,
    // or when telemetry failed (the live source then ends visibly).
    std::optional<uint64_t> sourceDroppedFrames;

    // The samples as delivered, row by row from the top.
    BudgetVector<uint8_t> data;
};

// A published frame. A source fills a frame while it is the only owner.
// Once the frame is held as a FramePtr, nobody writes it again, so the
// ring, the clips, and the preview can share it without copying pixels.
using FramePtr = std::shared_ptr<const Frame>;

[[nodiscard]] constexpr int bytesPerPixel(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::Mono8:
        return 1;
    case PixelFormat::Mono16:
        return 2;
    case PixelFormat::RGB888:
        return 3;
    }
    return 0;
}

// The bytes a frame of this size and format holds.
[[nodiscard]] constexpr size_t frameBytes(int width, int height, PixelFormat format)
{
    return static_cast<size_t>(width) * static_cast<size_t>(height)
        * static_cast<size_t>(bytesPerPixel(format));
}

[[nodiscard]] constexpr const char* pixelFormatName(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::Mono8:
        return "Mono8";
    case PixelFormat::Mono16:
        return "Mono16";
    case PixelFormat::RGB888:
        return "RGB888";
    }
    return "unknown";
}

[[nodiscard]] constexpr const char* bayerPatternName(BayerPattern pattern)
{
    switch (pattern)
    {
    case BayerPattern::None:
        return "none";
    case BayerPattern::RGGB:
        return "RGGB";
    case BayerPattern::GRBG:
        return "GRBG";
    case BayerPattern::GBRG:
        return "GBRG";
    case BayerPattern::BGGR:
        return "BGGR";
    }
    return "unknown";
}
