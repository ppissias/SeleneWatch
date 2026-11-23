#pragma once

#include "core/frame.h"
#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"

#include <QtCore/QRect>
#include <QtGui/QImage>

#include <atomic>
#include <chrono>
#include <optional>
#include <vector>

// Lightweight raw frame cropped on the source thread without debayering or
// float32 conversion; converted on the stacker worker thread for the <=200 kept frames.
struct RawBurstFrame
{
    uint64_t frameIndex{0};
    std::chrono::system_clock::time_point timestamp{};
    int width{0};
    int height{0};
    int rawWidth{0};
    int rawHeight{0};
    int innerOffsetX{0};
    int innerOffsetY{0};
    PixelFormat format{PixelFormat::Mono8};
    int significantBits{8};
    BitAlignment alignment{BitAlignment::Low};
    BayerPattern bayer{BayerPattern::None};
    double rawMaxAdu{255.0};
    std::vector<uint8_t> data;
    double sharpness{0};
    double targetBrightness{0};
    // saveRawSer only: a full-frame copy owned by the time-lapse and charged to
    // the session RAM budget (never the camera's pooled frame); evicted with the
    // heap entry and released once its burst SER is written.
    FramePtr rawFullFrame;
};

// Single frame extracted, optionally debayered/cropped, and scored for burst stacking.
struct PreparedBurstFrame
{
    uint64_t frameIndex{0};
    std::chrono::system_clock::time_point timestamp{};
    int width{0};
    int height{0};
    int channels{1};
    double rawMaxAdu{255.0};
    std::vector<float> pixels; // Planar [W*H*channels], normalized [0, 1]
    double sharpness{0};
    double targetBrightness{0};
};

struct BurstStackResult
{
    FitsImage stack;
    QRect validOverlap;
    bool ok{false};
    QString error;
};

struct SessionAlignResult
{
    FitsImage aligned;
    double shiftX{0};
    double shiftY{0};
    double rotationDeg{0};
    QRect validOverlap;
    bool usedFallback{false};
};

// Fast source-thread capture: copies cropped raw bytes (preserving Bayer phase
// with a small margin) and computes brightness-normalized sharpness without
// debayering or full-frame float conversion.
[[nodiscard]] RawBurstFrame captureRawBurstFrame(const Frame& raw,
    const std::optional<QRect>& cropRoi = {},
    const std::optional<QRect>& qualityRoi = {});

// Converts raw camera/file Frame into normalized linear float (debayering colour
// sensors in v1), crops to cropRoi if set, and computes brightness-normalized
// sharpness and target foreground brightness on qualityRoi.
[[nodiscard]] PreparedBurstFrame prepareBurstFrame(const Frame& raw,
    const std::optional<QRect>& cropRoi = {},
    const std::optional<QRect>& qualityRoi = {});

// Two-pass apodized subpixel phase-correlation alignment and sigma-clipped
// floating-point averaging over kept burst frames (<= 200).
[[nodiscard]] BurstStackResult stackBurst(const std::vector<PreparedBurstFrame>& kept,
    const std::optional<QRect>& alignRoi = {},
    std::atomic<bool>* cancel = nullptr);
[[nodiscard]] BurstStackResult stackBurst(const std::vector<RawBurstFrame>& kept,
    const std::optional<QRect>& alignRoi = {},
    std::atomic<bool>* cancel = nullptr);

// Aligns a finished stack to the session reference stack:
// - Planet: intensity centroid / limb centering (robust against surface rotation)
// - Moon: 2D rigid translation + rotation (robust against Alt-Az field rotation),
//   prioritizing lunarAnchor when specified.
[[nodiscard]] SessionAlignResult alignStackToReference(const FitsImage& stack,
    const FitsImage& reference,
    TimelapsePreset preset,
    const std::optional<QRect>& lunarAnchor = {});

// Applies linear float denoise + thresholded unsharp masking without gamma/stretch.
[[nodiscard]] FitsImage applyLinearSharpening(const FitsImage& unsharpened,
    const SharpeningSettings& settings);

// Summary of a stack's luminance contrast inside the valid overlap, used to pick
// a clear reference stack even when sensor noise inflates qualityScore under cloud.
struct StackBrightnessStats
{
    double p05{0.0};
    double p98{0.0};
    [[nodiscard]] double contrastSpan() const { return std::max(0.0, p98 - p05); }
};

// Linear brightness match mapping a stack onto the reference stack's scale:
// matched = input * gain + offset (in linear [0, 1] units before stretch/gamma).
struct BrightnessMatch
{
    double gain{1.0};
    double offset{0.0};
};

// Measures foreground target brightness, contrast span, and auto-stretch black/white quantiles.
[[nodiscard]] double measureTargetBrightness(const FitsImage& image);
[[nodiscard]] StackBrightnessStats measureStackBrightnessStats(const FitsImage& image,
    const QRect& crop = {});
[[nodiscard]] int selectClearReferenceStack(const std::vector<double>& qualityScores,
    const std::vector<double>& contrastSpans);
[[nodiscard]] BrightnessMatch computeBrightnessMatch(const FitsImage& image,
    const FitsImage& reference,
    const QRect& crop = {},
    bool alignedPixels = true);
void computeAutoStretchLevels(const FitsImage& image, double& outBlack, double& outWhite,
    const QRect& crop = {});

// Tone-maps a linear float FitsImage into an 8-bit QImage (Format_RGB888 or
// Format_Grayscale8) for live thumbnails or FFmpeg video export.
// When reference is non-null and levelling != Off, equalizes brightness (and
// optionally low-frequency uneven clouds) to reference inside crop before
// applying black/white stretch and gamma.
[[nodiscard]] QImage toneMapFitsToImage(const FitsImage& image,
    double black, double white, double gamma, double brightnessScale = 1.0,
    const QRect& crop = {}, int maxLongSide = 0,
    const FitsImage* reference = nullptr,
    BrightnessLevellingMode levelling = BrightnessLevellingMode::Off,
    bool alignedPixels = true);
