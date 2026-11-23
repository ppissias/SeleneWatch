#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"

#include <QtCore/QRect>
#include <QtCore/QString>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

struct StackFilterDecision
{
    bool kept{true};
    double referenceQuality{0.0};
    bool overridden{false};
};

// Pure quality-filter decision shared by Re-sharpen preview and video export.
// Applies tone.qualityFilterMode / minQualityRatio / keepBestPercent, then any
// manual per-stack overrides, guaranteeing at least the single sharpest stack
// remains kept when qualities is non-empty.
[[nodiscard]] std::vector<StackFilterDecision> evaluateStackQualityFilter(
    const std::vector<double>& qualities,
    const VideoToneSettings& tone,
    const std::vector<std::optional<bool>>& manualOverrides = {},
    int localWindowRadius = 5);

struct TimelapseExportResult
{
    bool ok{false};
    QString mp4Path;
    QString gifPath;
    QString serPath;
    int stacksUsed{0};
    int stacksDropped{0};
    QString error;
};

// Reads sorted *.fits stacks from fitsDir, applies quality filtering, overlap
// cropping, clear-reference selection, robust affine (or optional patchy-cloud)
// brightness levelling, and session-wide stretch/gamma (for video export only),
// then writes timelapse.mp4 / .gif into outputDir.
[[nodiscard]] TimelapseExportResult exportTimelapseFromFits(const QString& fitsDir,
    const QString& outputDir,
    const VideoToneSettings& tone,
    const QRect& commonOverlap = {},
    std::function<void(int done, int total)> progress = {},
    std::atomic<bool>* cancel = nullptr,
    const std::map<QString, bool>& stackOverrides = {});

// Re-sharpens every stack in <sessionDir>/stacks/ into a non-destructive
// <sessionDir>/sharpened_<profile>_<UTC>/ folder and builds its MP4/GIF.
// When rebuildVideoOnly is true, reads sourceFitsDir (default <sessionDir>/sharpened)
// directly without re-running FITS sharpening and writes the rebuilt video into
// a non-destructive video_<UTC>/ folder so earlier videos are never overwritten.
[[nodiscard]] TimelapseExportResult resharpenTimelapseSession(const QString& sessionDir,
    const QString& sourceFitsDir,
    const SharpeningSettings& sharpening,
    const VideoToneSettings& tone,
    bool rebuildVideoOnly,
    QString& outTargetDir,
    std::function<void(int done, int total, const QString& stage)> progress = {},
    std::atomic<bool>* cancel = nullptr,
    const std::map<QString, bool>& stackOverrides = {});
