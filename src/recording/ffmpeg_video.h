#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QProcess;

// Video and animation through the FFmpeg program SeleneWatch ships beside
// itself (LGPL, THIRD_PARTY_LICENSES.md): the event preview GIF, the sky
// time-lapse built while it is taken, and the lunar time-lapse's video. Frames
// go to FFmpeg over its standard input; no temporary image files are written.
// Each function runs on the caller's thread and waits for FFmpeg; an
// Mp4Stream keeps it running between frames.

// SELENEWATCH_FFMPEG_EXE when it is set, else ffmpeg.exe beside SeleneWatch,
// else the build's copy, else "ffmpeg" from PATH.
[[nodiscard]] QString findFfmpegExecutable();

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

// Makes frame index (0 to frameCount - 1) for an encoder, or a null image
// with err set to stop it.
using FrameProducer = std::function<QImage(int index, QString& err)>;

// Encodes frameCount frames of srcW × srcH, made one at a time, into an MP4
// (libopenh264, yuv420p) and/or a palette-optimized GIF, for each path that
// is not empty. Stops when cancel is set.
bool encodeStreamWithFfmpeg(int frameCount, const FrameProducer& produceFrame,
    int srcW, int srcH,
    const QString& mp4Path,
    const QString& gifPath,
    int fps,
    QString& error,
    std::atomic<bool>* cancel);

// The same for frames already in memory.
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
