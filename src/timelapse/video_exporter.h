#pragma once

#include "timelapse/fits_io.h"
#include "timelapse/timelapse_settings.h"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtGui/QImage>

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

[[nodiscard]] QString findFfmpegExecutable();

class QProcess;

// Feature-36: an MP4 written a frame at a time, for a time-lapse built while
// it is taken. FFmpeg runs for the whole stream, on a QProcess owned by the
// one thread that uses this. The file is fragmented MP4 with a keyframe every
// 10 frames, so a stream that is never closed (a crash, a power cut) still
// plays up to its last fragment.
class Mp4Stream
{
public:
    Mp4Stream();
    ~Mp4Stream();
    Mp4Stream(const Mp4Stream&) = delete;
    Mp4Stream& operator=(const Mp4Stream&) = delete;
    // width and height are made even.
    bool open(const QString& path, int width, int height, int fps, QString& error);
    // Scaled to the stream's size when it differs.
    bool write(const QImage& frame, QString& error);
    // Waits for FFmpeg to finish the file; true when it did.
    bool close(QString& error);
    [[nodiscard]] bool isOpen() const;

private:
    std::unique_ptr<QProcess> process_;
    QByteArray stderr_;
    int width_{0}, height_{0};
};

// Streams RGB888 frames directly to FFmpeg over stdin (no temporary PNG files).
// Writes MP4 (libopenh264, yuv420p) and/or palette-optimized GIF when paths
// are non-empty.
bool encodeImagesWithFfmpeg(const std::vector<QImage>& frames,
    const QString& mp4Path,
    const QString& gifPath,
    int fps,
    QString& error,
    std::atomic<bool>* cancel = nullptr);

struct EventPreviewGifOptions
{
    int fps{4};
    int maxLongEdge{512};
    qint64 maxBytes{512 * 1024};
    // Total time allowed for all FFmpeg runs. The GIF is written on the event
    // writer's thread, so a hung FFmpeg must not hold back the clips behind it.
    int timeoutMs{30000};
};

// Encodes up to 10 preview frames into a compact looping animated GIF via FFmpeg,
// downscaling large frames and stepping down resolution/palette tiers if needed so
// the output file stays within options.maxBytes, and killing FFmpeg once
// options.timeoutMs has passed. Returns encoded image dimensions
// and byte count via optional outputs when provided.
bool encodeEventPreviewGif(const std::vector<QImage>& frames,
    const QString& gifPath,
    const EventPreviewGifOptions& options,
    QString& error,
    int* outWidth = nullptr,
    int* outHeight = nullptr,
    qint64* outBytes = nullptr,
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
