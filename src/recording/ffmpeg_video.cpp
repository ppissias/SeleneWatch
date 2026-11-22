#include "recording/ffmpeg_video.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSize>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

namespace {

constexpr qsizetype kMaxStderrBytes = 32 * 1024;
const QString kFfmpegTimedOut = QStringLiteral("FFmpeg did not finish within the time limit");

void drainStderr(QProcess& proc, QByteArray& buf)
{
    const QByteArray chunk = proc.readAllStandardError();
    if (chunk.isEmpty()) return;
    if (buf.size() < kMaxStderrBytes)
    {
        const qsizetype room = kMaxStderrBytes - buf.size();
        buf.append(chunk.constData(), std::min(chunk.size(), room));
    }
}


// Runs FFmpeg with raw RGB24 frames on stdin. Stops it when cancel is set or
// the deadline passes (QDeadlineTimer::Forever for no time limit).
bool runFfmpegPipeStream(const QString& ffmpegExe, const QStringList& args,
    int frameCount, const FrameProducer& produceFrame, int w, int h,
    QString& error, std::atomic<bool>* cancel,
    const QDeadlineTimer& deadline = QDeadlineTimer(QDeadlineTimer::Forever))
{
    QProcess proc;
    proc.setProgram(ffmpegExe);
    proc.setArguments(args);
    proc.start(QIODevice::ReadWrite);
    if (!proc.waitForStarted(5000))
    {
        error = QString("Could not start FFmpeg (%1): %2").arg(ffmpegExe, proc.errorString());
        return false;
    }

    // True (after killing FFmpeg and setting error) when the run must end now.
    const auto mustAbort = [&] {
        if (cancel && cancel->load())
            error = "cancelled";
        else if (deadline.hasExpired())
            error = kFfmpegTimedOut;
        else
            return false;
        proc.kill();
        proc.waitForFinished(2000);
        return true;
    };

    QByteArray stderrBuf;
    const int rowBytes = w * 3;
    for (int i = 0; i < frameCount; ++i)
    {
        drainStderr(proc, stderrBuf);
        if (mustAbort()) return false;
        QString frameErr;
        const QImage img = produceFrame(i, frameErr);
        if (img.isNull())
        {
            proc.kill();
            proc.waitForFinished(2000);
            error = frameErr.isEmpty() ? QString("frame %1 could not be produced").arg(i) : frameErr;
            return false;
        }
        const QImage rgb = (img.format() == QImage::Format_RGB888 && img.width() == w && img.height() == h)
            ? img
            : img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
        for (int y = 0; y < h; ++y)
        {
            const char* row = reinterpret_cast<const char*>(rgb.constScanLine(y));
            qint64 written = 0;
            while (written < rowBytes)
            {
                const qint64 n = proc.write(row + written, rowBytes - written);
                if (n < 0)
                {
                    drainStderr(proc, stderrBuf);
                    error = QString("FFmpeg pipe write failed: %1").arg(QString::fromUtf8(stderrBuf).trimmed());
                    proc.kill();
                    proc.waitForFinished(2000);
                    return false;
                }
                written += n;
            }
        }
        while (proc.bytesToWrite() > 4 * 1024 * 1024)
        {
            drainStderr(proc, stderrBuf);
            if (mustAbort()) return false;
            if (!proc.waitForBytesWritten(2000) && proc.state() != QProcess::Running)
                break;
        }
    }

    proc.closeWriteChannel();
    while (!proc.waitForFinished(500))
    {
        drainStderr(proc, stderrBuf);
        if (mustAbort()) return false;
    }
    drainStderr(proc, stderrBuf);

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
    {
        error = QString("FFmpeg failed (code %1): %2")
            .arg(proc.exitCode()).arg(QString::fromUtf8(stderrBuf).trimmed());
        return false;
    }
    return true;
}

bool moveTempFileIntoPlace(const QString& tmpPath, const QString& targetPath, QString& error)
{
    QFile::remove(targetPath);
    if (QFile::rename(tmpPath, targetPath)) return true;
    error = QString("Could not move %1 into place").arg(targetPath);
    QFile::remove(tmpPath);
    return false;
}

// Computes even dimensions (minimum 2x2) fitting within maxLongEdge without upscaling.
QSize boundedEvenSize(int srcW, int srcH, int maxLongEdge)
{
    const int longSide = std::max(srcW, srcH);
    int w = srcW;
    int h = srcH;
    if (maxLongEdge > 0 && longSide > maxLongEdge)
    {
        const double scale = static_cast<double>(maxLongEdge) / static_cast<double>(longSide);
        w = static_cast<int>(std::lround(srcW * scale));
        h = static_cast<int>(std::lround(srcH * scale));
    }
    return QSize(std::max(2, w & ~1), std::max(2, h & ~1));
}

} // namespace

bool encodeStreamWithFfmpeg(int frameCount, const FrameProducer& produceFrame,
    int srcW, int srcH,
    const QString& mp4Path,
    const QString& gifPath,
    int fps,
    QString& error,
    std::atomic<bool>* cancel)
{
    if (frameCount <= 0 || srcW <= 0 || srcH <= 0)
    {
        error = "no frames to encode";
        return false;
    }
    if (mp4Path.isEmpty() && gifPath.isEmpty()) return true;

    const QString ffmpegExe = findFfmpegExecutable();
    const int evenW = std::max(2, srcW & ~1);
    const int evenH = std::max(2, srcH & ~1);
    const int safeFps = std::clamp(fps, 1, 60);

    if (!mp4Path.isEmpty())
    {
        const QFileInfo info(mp4Path);
        QDir().mkpath(info.absolutePath());
        const QString tmp = info.absolutePath() + "/." + info.fileName() + ".tmp.mp4";
        QFile::remove(tmp);

        const qint64 pixelsPerSec = static_cast<qint64>(evenW) * evenH * safeFps;
        const int targetKbps = static_cast<int>(std::clamp<qint64>(pixelsPerSec / 1800, 3000, 35000));
        const QStringList args{
            "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo",
            "-pixel_format", "rgb24",
            "-video_size", QString("%1x%2").arg(evenW).arg(evenH),
            "-framerate", QString::number(safeFps),
            "-i", "pipe:0",
            "-c:v", "libopenh264",
            "-profile:v", "high",
            "-coder", "cabac",
            "-rc_mode", "quality",
            "-b:v", QString("%1k").arg(targetKbps),
            "-allow_skip_frames", "0",
            "-pix_fmt", "yuv420p",
            "-movflags", "+faststart",
            tmp
        };
        if (!runFfmpegPipeStream(ffmpegExe, args, frameCount, produceFrame, evenW, evenH, error, cancel))
        {
            QFile::remove(tmp);
            return false;
        }
        QFile::remove(mp4Path);
        if (!QFile::rename(tmp, mp4Path))
        {
            error = QString("Could not move %1 into place").arg(mp4Path);
            QFile::remove(tmp);
            return false;
        }
    }

    if (!gifPath.isEmpty())
    {
        const QFileInfo info(gifPath);
        QDir().mkpath(info.absolutePath());
        const QString tmp = info.absolutePath() + "/." + info.fileName() + ".tmp.gif";
        QFile::remove(tmp);

        const QString statsMode = frameCount > 1 ? "diff" : "full";
        const QString filter = QString(
            "scale='min(800,iw)':-2:flags=lanczos,split[s0][s1];"
            "[s0]palettegen=stats_mode=%1[p];[s1][p]paletteuse=dither=floyd_steinberg").arg(statsMode);
        const QStringList args{
            "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo",
            "-pixel_format", "rgb24",
            "-video_size", QString("%1x%2").arg(evenW).arg(evenH),
            "-framerate", QString::number(safeFps),
            "-i", "pipe:0",
            "-vf", filter,
            "-loop", "0",
            tmp
        };
        if (!runFfmpegPipeStream(ffmpegExe, args, frameCount, produceFrame, evenW, evenH, error, cancel))
        {
            QFile::remove(tmp);
            return false;
        }
        QFile::remove(gifPath);
        if (!QFile::rename(tmp, gifPath))
        {
            error = QString("Could not move %1 into place").arg(gifPath);
            QFile::remove(tmp);
            return false;
        }
    }

    return true;
}

Mp4Stream::Mp4Stream() = default;

Mp4Stream::~Mp4Stream()
{
    QString ignored;
    close(ignored);
}

bool Mp4Stream::isOpen() const
{
    return process_ != nullptr;
}

bool Mp4Stream::open(const QString& path, int width, int height, int fps, QString& error)
{
    if (process_) return true;
    width_ = std::max(2, width & ~1);
    height_ = std::max(2, height & ~1);
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int safeFps = std::clamp(fps, 1, 60);
    const qint64 pixelsPerSec = static_cast<qint64>(width_) * height_ * safeFps;
    const int targetKbps = static_cast<int>(std::clamp<qint64>(pixelsPerSec / 1800, 3000, 35000));
    const QStringList args{
        "-y", "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo",
        "-pixel_format", "rgb24",
        "-video_size", QString("%1x%2").arg(width_).arg(height_),
        "-framerate", QString::number(safeFps),
        "-i", "pipe:0",
        "-c:v", "libopenh264",
        "-profile:v", "high",
        "-rc_mode", "quality",
        "-b:v", QString("%1k").arg(targetKbps),
        "-allow_skip_frames", "0",
        "-g", "10",
        "-pix_fmt", "yuv420p",
        "-movflags", "+frag_keyframe+empty_moov+default_base_moof",
        "-flush_packets", "1",
        path
    };
    process_ = std::make_unique<QProcess>();
    process_->setProgram(findFfmpegExecutable());
    process_->setArguments(args);
    process_->start(QIODevice::ReadWrite);
    if (!process_->waitForStarted(5000))
    {
        error = QString("Could not start FFmpeg (%1): %2").arg(process_->program(), process_->errorString());
        process_.reset();
        return false;
    }
    stderr_.clear();
    return true;
}

bool Mp4Stream::write(const QImage& frame, QString& error)
{
    if (!process_)
    {
        error = "the video is not open";
        return false;
    }
    drainStderr(*process_, stderr_);
    if (process_->state() != QProcess::Running)
    {
        error = QString("FFmpeg stopped: %1").arg(QString::fromUtf8(stderr_).trimmed());
        process_.reset();
        return false;
    }
    const QImage rgb = (frame.format() == QImage::Format_RGB888 && frame.width() == width_ && frame.height() == height_)
        ? frame
        : frame.scaled(width_, height_, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    const qint64 rowBytes = static_cast<qint64>(width_) * 3;
    for (int y = 0; y < height_; ++y)
    {
        const char* row = reinterpret_cast<const char*>(rgb.constScanLine(y));
        qint64 written = 0;
        while (written < rowBytes)
        {
            const qint64 n = process_->write(row + written, rowBytes - written);
            if (n < 0)
            {
                drainStderr(*process_, stderr_);
                error = QString("FFmpeg pipe write failed: %1").arg(QString::fromUtf8(stderr_).trimmed());
                process_->kill();
                process_->waitForFinished(2000);
                process_.reset();
                return false;
            }
            written += n;
        }
    }
    // The frame reaches FFmpeg now, not with the next one 30 s later.
    while (process_->bytesToWrite() > 0)
    {
        drainStderr(*process_, stderr_);
        if (!process_->waitForBytesWritten(5000) && process_->state() != QProcess::Running) break;
    }
    return true;
}

bool Mp4Stream::close(QString& error)
{
    if (!process_) return true;
    process_->closeWriteChannel();
    const QDeadlineTimer deadline(60'000);
    while (!process_->waitForFinished(500))
    {
        drainStderr(*process_, stderr_);
        if (deadline.hasExpired())
        {
            process_->kill();
            process_->waitForFinished(2000);
            process_.reset();
            error = kFfmpegTimedOut;
            return false;
        }
    }
    drainStderr(*process_, stderr_);
    const bool ok = process_->exitStatus() == QProcess::NormalExit && process_->exitCode() == 0;
    if (!ok) error = QString("FFmpeg failed (code %1): %2").arg(process_->exitCode()).arg(QString::fromUtf8(stderr_).trimmed());
    process_.reset();
    return ok;
}

QString findFfmpegExecutable()
{
    const QString env = qEnvironmentVariable("SELENEWATCH_FFMPEG_EXE");
    if (!env.isEmpty() && QFileInfo::exists(env)) return env;

    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
    if (QFileInfo::exists(beside)) return beside;

#ifdef SELENEWATCH_FFMPEG_BIN_HINT
    const QString hint = QDir(QStringLiteral(SELENEWATCH_FFMPEG_BIN_HINT)).filePath("ffmpeg.exe");
    if (QFileInfo::exists(hint)) return hint;
#endif
    return "ffmpeg";
}

bool encodeImagesWithFfmpeg(const std::vector<QImage>& frames,
    const QString& mp4Path,
    const QString& gifPath,
    int fps,
    QString& error,
    std::atomic<bool>* cancel)
{
    if (frames.empty())
    {
        error = "no frames to encode";
        return false;
    }
    return encodeStreamWithFfmpeg(
        static_cast<int>(frames.size()),
        [&frames](int i, QString&) -> QImage { return frames[static_cast<size_t>(i)]; },
        frames.front().width(),
        frames.front().height(),
        mp4Path, gifPath, fps, error, cancel);
}

bool encodeEventPreviewGif(const std::vector<QImage>& frames,
    const QString& gifPath,
    const EventPreviewGifOptions& options,
    QString& error,
    int* outWidth,
    int* outHeight,
    qint64* outBytes,
    std::atomic<bool>* cancel)
{
    if (frames.empty())
    {
        error = "no frames to encode";
        return false;
    }
    if (gifPath.isEmpty())
    {
        error = "no GIF output path";
        return false;
    }

    const QString ffmpegExe = findFfmpegExecutable();
    const QFileInfo info(gifPath);
    QDir().mkpath(info.absolutePath());
    const QString tmp = info.absolutePath() + "/." + info.fileName() + ".tmp.gif";

    const int srcW = std::max(2, frames.front().width());
    const int srcH = std::max(2, frames.front().height());
    const int startEdge = std::max(32, std::min(options.maxLongEdge > 0 ? options.maxLongEdge : 512,
        std::max(srcW, srcH)));
    const int safeFps = std::clamp(options.fps, 1, 30);
    const QDeadlineTimer deadline(options.timeoutMs > 0 ? options.timeoutMs : 30000);

    struct GifTier
    {
        double edgeFraction;
        int maxColors;
        const char* dither;
    };
    constexpr GifTier kTiers[] = {
        {1.00, 128, "bayer:bayer_scale=3"},
        {0.75, 96,  "bayer:bayer_scale=3"},
        {0.56, 64,  "bayer:bayer_scale=4"},
        {0.38, 48,  "none"},
        {0.25, 32,  "none"},
    };

    QSize finalSize;
    qint64 finalBytes = 0;
    for (size_t t = 0; t < std::size(kTiers); ++t)
    {
        const auto& tier = kTiers[t];
        const int targetEdge = std::max(32, static_cast<int>(std::lround(startEdge * tier.edgeFraction)));
        const QSize sz = boundedEvenSize(srcW, srcH, targetEdge);

        std::vector<QImage> scaled;
        scaled.reserve(frames.size());
        for (const QImage& img : frames)
        {
            QImage rgb = (img.width() == sz.width() && img.height() == sz.height())
                ? img
                : img.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (rgb.format() != QImage::Format_RGB888)
                rgb = std::move(rgb).convertToFormat(QImage::Format_RGB888);
            scaled.push_back(std::move(rgb));
        }

        const QString filter = QString(
            "split[s0][s1];[s0]palettegen=max_colors=%1:stats_mode=full[p];"
            "[s1][p]paletteuse=dither=%2").arg(tier.maxColors).arg(QLatin1String(tier.dither));
        const QStringList args{
            "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo",
            "-pixel_format", "rgb24",
            "-video_size", QString("%1x%2").arg(sz.width()).arg(sz.height()),
            "-framerate", QString::number(safeFps),
            "-i", "pipe:0",
            "-vf", filter,
            "-loop", "0",
            tmp
        };

        QFile::remove(tmp);
        if (!runFfmpegPipeStream(ffmpegExe, args, static_cast<int>(scaled.size()),
                [&scaled](int i, QString&) -> QImage { return scaled[static_cast<size_t>(i)]; },
                sz.width(), sz.height(), error, cancel, deadline))
        {
            if (error == kFfmpegTimedOut)
                error = QString("FFmpeg did not finish within %1 s").arg(options.timeoutMs / 1000.0, 0, 'g', 3);
            QFile::remove(tmp);
            return false;
        }

        finalSize = sz;
        finalBytes = QFileInfo(tmp).size();
        if (options.maxBytes <= 0 || finalBytes <= options.maxBytes)
            break;
    }

    if (!moveTempFileIntoPlace(tmp, gifPath, error))
        return false;

    if (outWidth) *outWidth = finalSize.width();
    if (outHeight) *outHeight = finalSize.height();
    if (outBytes) *outBytes = finalBytes;
    return true;
}
