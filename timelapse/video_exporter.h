#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <atomic>
#include <functional>
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

[[nodiscard]] QString findFfmpegExecutable();

// Streams RGB888 frames directly to FFmpeg over stdin (no temporary PNG files).
// Writes MP4 (libopenh264, yuv420p) and/or palette-optimized GIF when paths
// are non-empty.
bool encodeImagesWithFfmpeg(const std::vector<QImage>& frames,
    const QString& mp4Path,
    const QString& gifPath,
    int fps,
    QString& error,
    std::atomic<bool>* cancel = nullptr);

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
// When rebuildVideoOnly is true, uses sourceFitsDir (default <sessionDir>/sharpened)
// directly without re-running FITS sharpening.
[[nodiscard]] TimelapseExportResult resharpenTimelapseSession(const QString& sessionDir,
    const QString& sourceFitsDir,
    const SharpeningSettings& sharpening,
    const VideoToneSettings& tone,
    bool rebuildVideoOnly,
    QString& outTargetDir,
    std::function<void(int done, int total, const QString& stage)> progress = {},
    std::atomic<bool>* cancel = nullptr);
