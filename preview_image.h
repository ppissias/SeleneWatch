#pragma once

#include "frame.h"
#include "display_settings.h"

#include <QtGui/QImage>

#include <array>

// The conversion for the screen. It belongs to the window and returns an
// 8-bit image that owns its pixels. It may shift, stretch, or invert as the
// preview needs. The event writer may use it for clearly labelled display
// PNGs; native SER evidence has its own unchanged-byte write path.
//
// Window levels and automatic percentile/midtones operate in native sample
// units before conversion. Bayer interpolation is display only.
[[nodiscard]] QImage frameToPreviewImage(const Frame& frame, DisplaySettings settings = {}, DisplayLevels* levels = nullptr);

// Distribution of the original samples before any preview conversion.
// Bayer samples and the separate RGB sample values share one distribution.
struct PreviewHistogram
{
    static constexpr int maxBins = 256;
    std::array<uint64_t, maxBins> bins{};
    int binCount{0};
    int rangeMinimum{0}, rangeMaximum{255};
    int observedMinimum{0}, observedMaximum{0};
    int maximumCode{255};
    uint64_t sampleCount{0}, zeroCount{0}, maximumCodeCount{0};
};

[[nodiscard]] PreviewHistogram frameToPreviewHistogram(const Frame& frame, bool fitRange);

// A writable QImage backed by charged pixels. Shared image copies keep one
// allocation until the last copy is destroyed. For display and writer previews.
[[nodiscard]] QImage makeBudgetImage(int width, int height, QImage::Format format, MemoryBudget budget);

// Consistent auto-stretch curve computed from one representative frame and
// reusable across an animated event preview sequence so frame brightness stays steady.
struct AutoStretchCurve
{
    double black{0};
    double white{1};
    double midtones{0.5};
};

[[nodiscard]] AutoStretchCurve computeAutoStretchCurve(const Frame& frame);
[[nodiscard]] QImage renderAutoStretchedFrame(const Frame& frame, const AutoStretchCurve& curve,
    bool invert = false, bool debayer = true);
