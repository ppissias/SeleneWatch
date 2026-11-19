#include "timelapse/video_exporter.h"
#include "recording/metadata_text.h"
#include "timelapse/stack_processor.h"

#include <QtCore/QCoreApplication>
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

using FrameProducer = std::function<QImage(int index, QString& err)>;

bool runFfmpegPipeStream(const QString& ffmpegExe, const QStringList& args,
    int frameCount, const FrameProducer& produceFrame, int w, int h,
    QString& error, std::atomic<bool>* cancel)
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

    QByteArray stderrBuf;
    const int rowBytes = w * 3;
    for (int i = 0; i < frameCount; ++i)
    {
        drainStderr(proc, stderrBuf);
        if (cancel && cancel->load())
        {
            proc.kill();
            proc.waitForFinished(2000);
            error = "cancelled";
            return false;
        }
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
            if (cancel && cancel->load())
            {
                proc.kill();
                proc.waitForFinished(2000);
                error = "cancelled";
                return false;
            }
            if (!proc.waitForBytesWritten(2000) && proc.state() != QProcess::Running)
                break;
        }
    }

    proc.closeWriteChannel();
    while (!proc.waitForFinished(500))
    {
        drainStderr(proc, stderrBuf);
        if (cancel && cancel->load())
        {
            proc.kill();
            proc.waitForFinished(2000);
            error = "cancelled";
            return false;
        }
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

QStringList sortedFitsFiles(const QString& dirPath)
{
    const QDir dir(dirPath);
    QStringList files = dir.entryList({"*.fits", "*.fit"}, QDir::Files | QDir::Readable, QDir::Name);
    for (QString& f : files) f = dir.filePath(f);
    return files;
}

bool moveTempFileIntoPlace(const QString& tmpPath, const QString& targetPath, QString& error)
{
    QFile::remove(targetPath);
    if (QFile::rename(tmpPath, targetPath)) return true;
    error = QString("Could not move %1 into place").arg(targetPath);
    QFile::remove(tmpPath);
    return false;
}

QString createUniqueChildDir(const QString& parentPath, const QString& baseName)
{
    if (!QDir().mkpath(parentPath)) return {};
    const QDir parent(parentPath);
    for (int attempt = 1; attempt <= 999; ++attempt)
    {
        const QString name = attempt == 1 ? baseName : QString("%1-%2").arg(baseName).arg(attempt);
        if (parent.mkdir(name)) return parent.filePath(name);
        if (!parent.exists(name)) break;
    }
    return {};
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
                sz.width(), sz.height(), error, cancel))
        {
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

TimelapseExportResult exportTimelapseFromFits(const QString& fitsDir,
    const QString& outputDir,
    const VideoToneSettings& tone,
    const QRect& commonOverlap,
    std::function<void(int done, int total)> progress,
    std::atomic<bool>* cancel)
{
    TimelapseExportResult res;
    if (!tone.writeMp4 && !tone.writeGif)
    {
        res.ok = true;
        return res;
    }

    const QStringList files = sortedFitsFiles(fitsDir);
    if (files.isEmpty())
    {
        res.error = QString("No FITS stacks found in %1").arg(fitsDir);
        return res;
    }

    struct Entry
    {
        QString path;
        double quality{0};
    };
    std::vector<Entry> entries;
    entries.reserve(files.size());
    std::vector<double> qualities;
    qualities.reserve(files.size());

    // First pass: inspect FITS headers only (no pixel I/O) to find best-quality reference.
    QString bestPath = files.front();
    double bestQuality = -1.0;
    for (const QString& f : files)
    {
        if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
        FitsMetadata meta;
        int w = 0, h = 0, ch = 0;
        QString err;
        if (!readFitsMetadata(f, meta, w, h, ch, err))
        {
            res.error = err;
            return res;
        }
        entries.push_back({f, meta.qualityScore});
        qualities.push_back(meta.qualityScore);
        if (meta.qualityScore > bestQuality)
        {
            bestQuality = meta.qualityScore;
            bestPath = f;
        }
    }

    std::sort(qualities.begin(), qualities.end());
    const double medianQuality = qualities[qualities.size() / 2];
    const double minAllowed = tone.minQualityRatio > 0.0 ? tone.minQualityRatio * medianQuality : -1.0;

    std::vector<QString> keptPaths;
    for (const Entry& e : entries)
    {
        if (e.quality >= minAllowed) keptPaths.push_back(e.path);
        else ++res.stacksDropped;
    }
    if (keptPaths.empty())
    {
        keptPaths.push_back(bestPath);
        res.stacksDropped = static_cast<int>(entries.size()) - 1;
    }

    FitsImage refImg;
    if (!readFitsFloat(bestPath, refImg, res.error)) return res;

    double black = tone.blackLevel;
    double white = tone.whiteLevel;
    if (tone.autoStretch) computeAutoStretchLevels(refImg, black, white);
    const double refBrightness = std::max(1e-5, measureTargetBrightness(refImg));

    QRect crop = (tone.cropToOverlap && commonOverlap.isValid() && !commonOverlap.isEmpty())
        ? commonOverlap.intersected(QRect(0, 0, refImg.width, refImg.height))
        : QRect(0, 0, refImg.width, refImg.height);
    if (crop.width() < 2 || crop.height() < 2)
        crop = QRect(0, 0, refImg.width, refImg.height);
    // Ensure even dimensions for H.264 yuv420p.
    crop.setWidth(std::max(2, crop.width() & ~1));
    crop.setHeight(std::max(2, crop.height() & ~1));

    QDir().mkpath(outputDir);
    const QString mp4 = tone.writeMp4 ? QDir(outputDir).filePath("timelapse.mp4") : QString();
    const QString gif = tone.writeGif ? QDir(outputDir).filePath("timelapse.gif") : QString();

    const int total = static_cast<int>(keptPaths.size());
    const int passes = (tone.writeMp4 ? 1 : 0) + (tone.writeGif ? 1 : 0);
    int emitted = 0;

    // Stream one stack at a time into FFmpeg stdin so multi-hour sessions stay O(1) RAM.
    const auto produceFrame = [&](int i, QString& err) -> QImage {
        FitsImage img;
        if (!readFitsFloat(keptPaths[static_cast<size_t>(i)], img, err)) return {};
        double scale = 1.0;
        if (tone.brightnessLevelling)
        {
            const double b = measureTargetBrightness(img);
            if (b > 1e-5) scale = std::clamp(refBrightness / b, 0.4, 2.5);
        }
        QImage mapped = toneMapFitsToImage(img, black, white, tone.gamma, scale, crop, 0);
        if (mapped.isNull())
        {
            err = "could not tone-map FITS stack";
            return {};
        }
        ++emitted;
        if (progress) progress(emitted, total * std::max(1, passes));
        return mapped;
    };

    if (!encodeStreamWithFfmpeg(total, produceFrame, crop.width(), crop.height(), mp4, gif, tone.fps, res.error, cancel))
        return res;

    res.ok = true;
    res.mp4Path = mp4;
    res.gifPath = gif;
    res.stacksUsed = total;
    return res;
}

TimelapseExportResult resharpenTimelapseSession(const QString& sessionDir,
    const QString& sourceFitsDir,
    const SharpeningSettings& sharpening,
    const VideoToneSettings& tone,
    bool rebuildVideoOnly,
    QString& outTargetDir,
    std::function<void(int done, int total, const QString& stage)> progress,
    std::atomic<bool>* cancel)
{
    TimelapseExportResult res;

    // Load persisted commonOverlap from the session profile (if present) so
    // "Crop to valid overlap" works identically on re-sharpen and rebuild runs.
    std::optional<QRect> savedOverlap;
    {
        SharpeningSettings dummySharp;
        VideoToneSettings dummyTone;
        QString dummyErr;
        const QString sessionProfile = QDir(sessionDir).filePath("sharpening_profile.json");
        if (QFileInfo::exists(sessionProfile))
            (void)loadProfileJson(sessionProfile, dummySharp, dummyTone, dummyErr, &savedOverlap);
    }
    const QRect overlapRect = savedOverlap.value_or(QRect{});

    if (rebuildVideoOnly)
    {
        const QString fitsDir = sourceFitsDir.isEmpty() ? QDir(sessionDir).filePath("sharpened") : sourceFitsDir;
        const QString parentBase = (QDir::cleanPath(fitsDir) == QDir::cleanPath(QDir(sessionDir).filePath("sharpened")))
            ? sessionDir
            : QFileInfo(fitsDir).absolutePath();
        const QString stamp = utcStamp(std::chrono::system_clock::now());
        outTargetDir = createUniqueChildDir(parentBase, QString("video_%1").arg(stamp));
        if (outTargetDir.isEmpty())
        {
            res.error = QString("Could not create video rebuild folder under %1").arg(parentBase);
            return res;
        }
        QString profileErr;
        (void)saveProfileJson(QDir(outTargetDir).filePath("sharpening_profile.json"),
            sharpening, tone, profileErr, savedOverlap);
        if (progress) progress(0, 1, "Encoding video…");
        res = exportTimelapseFromFits(fitsDir, outTargetDir, tone, overlapRect, [&](int d, int t) {
            if (progress) progress(d, t, "Encoding video…");
        }, cancel);
        return res;
    }

    const QString stacksDir = QDir(sessionDir).filePath("stacks");
    const QStringList files = sortedFitsFiles(stacksDir);
    if (files.isEmpty())
    {
        res.error = QString("No unsharpened stacks found in %1").arg(stacksDir);
        return res;
    }

    const QString stamp = utcStamp(std::chrono::system_clock::now());
    const QString prof = sanitizeProfileName(sharpening.profileName);
    outTargetDir = createUniqueChildDir(sessionDir, QString("sharpened_%1_%2").arg(prof, stamp));
    if (outTargetDir.isEmpty())
    {
        res.error = QString("Could not create re-sharpen folder under %1").arg(sessionDir);
        return res;
    }
    const QString sharpOut = QDir(outTargetDir).filePath("sharpened");
    if (!QDir().mkpath(sharpOut))
    {
        res.error = QString("Could not create %1").arg(sharpOut);
        return res;
    }

    const int total = static_cast<int>(files.size());
    for (int i = 0; i < total; ++i)
    {
        if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
        FitsImage raw;
        if (!readFitsFloat(files[static_cast<size_t>(i)], raw, res.error)) return res;
        const FitsImage sharpened = applyLinearSharpening(raw, sharpening);
        const QString outFile = QDir(sharpOut).filePath(QFileInfo(files[static_cast<size_t>(i)]).fileName());
        if (!writeFitsFloat(outFile, sharpened, res.error)) return res;
        if (progress) progress(i + 1, total, "Sharpening FITS stacks…");
    }

    QString profileErr;
    (void)saveProfileJson(QDir(outTargetDir).filePath("sharpening_profile.json"),
        sharpening, tone, profileErr, savedOverlap);

    res = exportTimelapseFromFits(sharpOut, outTargetDir, tone, overlapRect, [&](int d, int t) {
        if (progress) progress(d, t, "Encoding video…");
    }, cancel);
    return res;
}
