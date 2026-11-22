#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"

#include <QtCore/QRect>
#include <QtCore/QString>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

struct TimelapseExportResult
{
    bool ok{false};
    QString mp4Path;
    QString gifPath;
    int stacksUsed{0};
    int stacksDropped{0};
    QString error;
};

// Reads sorted *.fits stacks from fitsDir, applies quality filtering, overlap
// cropping, target-masked multiplicative brightness levelling, and session-wide
// stretch/gamma (for video export only), then writes timelapse.mp4 / .gif into outputDir.
[[nodiscard]] TimelapseExportResult exportTimelapseFromFits(const QString& fitsDir,
    const QString& outputDir,
    const VideoToneSettings& tone,
    const QRect& commonOverlap = {},
    std::function<void(int done, int total)> progress = {},
    std::atomic<bool>* cancel = nullptr);

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
    std::atomic<bool>* cancel = nullptr);
