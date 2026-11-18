#include "timelapse/video_exporter.h"
#include "recording/metadata_text.h"
#include "timelapse/stack_processor.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

bool runFfmpegPipe(const QString& ffmpegExe, const QStringList& args,
    const std::vector<QImage>& rgbFrames, int w, int h,
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

    const int rowBytes = w * 3;
    for (const QImage& img : rgbFrames)
    {
        if (cancel && cancel->load())
        {
            proc.kill();
            proc.waitForFinished(2000);
            error = "cancelled";
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
                    error = QString("FFmpeg pipe write failed: %1").arg(QString::fromUtf8(proc.readAllStandardError()));
                    proc.kill();
                    proc.waitForFinished(2000);
                    return false;
                }
                written += n;
            }
        }
        while (proc.bytesToWrite() > 4 * 1024 * 1024)
        {
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
        if (cancel && cancel->load())
        {
            proc.kill();
            proc.waitForFinished(2000);
            error = "cancelled";
            return false;
        }
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
    {
        error = QString("FFmpeg failed (code %1): %2")
            .arg(proc.exitCode()).arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return false;
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
    if (mp4Path.isEmpty() && gifPath.isEmpty()) return true;

    const QString ffmpegExe = findFfmpegExecutable();
    const int evenW = std::max(2, frames.front().width() & ~1);
    const int evenH = std::max(2, frames.front().height() & ~1);
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
            "-profile", "high",
            "-coder", "cabac",
            "-rc_mode", "quality",
            "-b:v", QString("%1k").arg(targetKbps),
            "-allow_skip_frames", "0",
            "-pix_fmt", "yuv420p",
            "-movflags", "+faststart",
            tmp
        };
        if (!runFfmpegPipe(ffmpegExe, args, frames, evenW, evenH, error, cancel))
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

        const QString statsMode = frames.size() > 1 ? "diff" : "full";
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
        if (!runFfmpegPipe(ffmpegExe, args, frames, evenW, evenH, error, cancel))
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

    // First pass: inspect metadata & find best-quality reference stack.
    QString bestPath = files.front();
    double bestQuality = -1.0;
    for (const QString& f : files)
    {
        if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
        FitsImage img;
        QString err;
        if (!readFitsFloat(f, img, err))
        {
            res.error = err;
            return res;
        }
        entries.push_back({f, img.meta.qualityScore});
        qualities.push_back(img.meta.qualityScore);
        if (img.meta.qualityScore > bestQuality)
        {
            bestQuality = img.meta.qualityScore;
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

    std::vector<QImage> frames;
    frames.reserve(keptPaths.size());
    const int total = static_cast<int>(keptPaths.size());
    for (int i = 0; i < total; ++i)
    {
        if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
        FitsImage img;
        if (!readFitsFloat(keptPaths[static_cast<size_t>(i)], img, res.error)) return res;
        double scale = 1.0;
        if (tone.brightnessLevelling)
        {
            const double b = measureTargetBrightness(img);
            if (b > 1e-5) scale = std::clamp(refBrightness / b, 0.4, 2.5);
        }
        QImage mapped = toneMapFitsToImage(img, black, white, tone.gamma, scale, crop, 0);
        if (mapped.isNull())
        {
            res.error = "could not tone-map FITS stack";
            return res;
        }
        frames.push_back(std::move(mapped));
        if (progress) progress(i + 1, total);
    }

    QDir().mkpath(outputDir);
    const QString mp4 = tone.writeMp4 ? QDir(outputDir).filePath("timelapse.mp4") : QString();
    const QString gif = tone.writeGif ? QDir(outputDir).filePath("timelapse.gif") : QString();
    if (!encodeImagesWithFfmpeg(frames, mp4, gif, tone.fps, res.error, cancel))
        return res;

    res.ok = true;
    res.mp4Path = mp4;
    res.gifPath = gif;
    res.stacksUsed = static_cast<int>(frames.size());
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
    if (rebuildVideoOnly)
    {
        const QString fitsDir = sourceFitsDir.isEmpty() ? QDir(sessionDir).filePath("sharpened") : sourceFitsDir;
        outTargetDir = (QDir::cleanPath(fitsDir) == QDir::cleanPath(QDir(sessionDir).filePath("sharpened")))
            ? sessionDir
            : QFileInfo(fitsDir).absolutePath();
        if (progress) progress(0, 1, "Encoding video…");
        res = exportTimelapseFromFits(fitsDir, outTargetDir, tone, {}, [&](int d, int t) {
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
    outTargetDir = QDir(sessionDir).filePath(QString("sharpened_%1_%2").arg(prof, stamp));
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
    (void)saveProfileJson(QDir(outTargetDir).filePath("sharpening_profile.json"), sharpening, tone, profileErr);

    res = exportTimelapseFromFits(sharpOut, outTargetDir, tone, {}, [&](int d, int t) {
        if (progress) progress(d, t, "Encoding video…");
    }, cancel);
    return res;
}
