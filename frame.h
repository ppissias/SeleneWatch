#pragma once

#include "blip.h"

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
// Some cameras shift 12-bit data into the top bits (ZWO's RAW16, FireCapture's
// SER files); others leave it in the low bits.
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
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Mono8;

    // Bits of each sample that carry data: 8 for Mono8 and RGB888, up to 16
    // for Mono16. A 12-bit camera's frame is Mono16 with 12 here.
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

    // A replayed SER explicitly marked as test data by its writer. Its
    // individual injection details remain in the original clip's sidecar.
    bool testSource{false};

    // The samples as delivered, row by row from the top.
    std::vector<uint8_t> data;
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

// The right shift that leaves the top 8 significant bits of a Mono16 sample.
// The preview and the Feature-4 detector read 16-bit samples this way.
[[nodiscard]] constexpr int shiftToEightBits(const Frame& frame)
{
    if (frame.alignment == BitAlignment::High)
    {
        return 8;
    }
    return frame.significantBits > 8 ? frame.significantBits - 8 : 0;
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
