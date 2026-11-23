#include "timelapse/video_exporter.h"
#include "recording/ffmpeg_video.h"
#include "recording/metadata_text.h"
#include "timelapse/stack_processor.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

QStringList sortedFitsFiles(const QString& dirPath)
{
    const QDir dir(dirPath);
    QStringList files = dir.entryList({"*.fits", "*.fit"}, QDir::Files | QDir::Readable, QDir::Name);
    for (QString& f : files) f = dir.filePath(f);
    return files;
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
} // namespace

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

    // First pass: inspect FITS headers only to apply quality filtering and read frame size.
    int frameW = 0, frameH = 0;
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
        if (frameW == 0 && w > 0 && h > 0) { frameW = w; frameH = h; }
        entries.push_back({f, meta.qualityScore});
        qualities.push_back(meta.qualityScore);
    }

    std::sort(qualities.begin(), qualities.end());
    const double medianQuality = qualities[qualities.size() / 2];
    const double minAllowed = tone.minQualityRatio > 0.0 ? tone.minQualityRatio * medianQuality : -1.0;

    std::vector<QString> keptPaths;
    std::vector<double> keptQualities;
    for (const Entry& e : entries)
    {
        if (e.quality >= minAllowed)
        {
            keptPaths.push_back(e.path);
            keptQualities.push_back(e.quality);
        }
        else ++res.stacksDropped;
    }
    if (keptPaths.empty())
    {
        const auto bestIt = std::max_element(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.quality < b.quality; });
        keptPaths.push_back(bestIt->path);
        keptQualities.push_back(bestIt->quality);
        res.stacksDropped = static_cast<int>(entries.size()) - 1;
    }

    QRect crop = (tone.cropToOverlap && commonOverlap.isValid() && !commonOverlap.isEmpty())
        ? commonOverlap.intersected(QRect(0, 0, frameW, frameH))
        : QRect(0, 0, frameW, frameH);
    if (crop.width() < 2 || crop.height() < 2)
        crop = QRect(0, 0, frameW, frameH);
    // Ensure even dimensions for H.264 yuv420p.
    crop.setWidth(std::max(2, crop.width() & ~1));
    crop.setHeight(std::max(2, crop.height() & ~1));

    // Pick the sharpest clear stack by measuring contrast span strictly inside crop
    // (one stack in RAM at a time), so heavy-cloud frames whose sensor noise inflated
    // qualityScore never steal the reference levels.
    int refIdx = 0;
    if (keptPaths.size() > 1)
    {
        std::vector<double> keptSpans;
        keptSpans.reserve(keptPaths.size());
        for (const QString& p : keptPaths)
        {
            if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
            FitsImage probe;
            if (!readFitsFloat(p, probe, res.error)) return res;
            keptSpans.push_back(measureStackBrightnessStats(probe, crop).contrastSpan());
        }
        refIdx = std::max(0, selectClearReferenceStack(keptQualities, keptSpans));
    }

    FitsImage refImg;
    if (!readFitsFloat(keptPaths[static_cast<size_t>(refIdx)], refImg, res.error)) return res;

    double black = tone.blackLevel;
    double white = tone.whiteLevel;
    if (tone.autoStretch) computeAutoStretchLevels(refImg, black, white, crop);
    const bool alignedPixels = refImg.meta.targetPreset != "planet";

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
        const FitsImage* refPtr = (i == refIdx) ? &img : &refImg;
        QImage mapped = toneMapFitsToImage(img, black, white, tone.gamma, 1.0, crop, 0,
            refPtr, tone.brightnessMode, alignedPixels);
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
