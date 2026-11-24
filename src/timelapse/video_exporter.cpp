#include "timelapse/video_exporter.h"
#include "recording/ffmpeg_video.h"
#include "recording/metadata_text.h"
#include "recording/ser_writer.h"
#include "timelapse/stack_processor.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

namespace {

double medianOf(std::vector<double> values)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

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

std::vector<StackFilterDecision> evaluateStackQualityFilter(
    const std::vector<double>& qualities,
    const VideoToneSettings& tone,
    const std::vector<std::optional<bool>>& manualOverrides,
    int localWindowRadius)
{
    const size_t n = qualities.size();
    std::vector<StackFilterDecision> out(n);
    if (n == 0) return out;

    const int radius = std::max(1, localWindowRadius);

    if (tone.qualityFilterMode == QualityFilterMode::KeepBestPct)
    {
        const double pct = std::clamp(tone.keepBestPercent, 1.0, 100.0);
        if (pct >= 100.0)
        {
            const double minQ = *std::min_element(qualities.begin(), qualities.end());
            for (auto& d : out) { d.kept = true; d.referenceQuality = minQ; }
        }
        else
        {
            std::vector<size_t> order(n);
            std::iota(order.begin(), order.end(), size_t{0});
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return qualities[a] > qualities[b];
            });
            const size_t keepCount = std::clamp(
                static_cast<size_t>(std::ceil(static_cast<double>(n) * pct / 100.0)),
                size_t{1}, n);
            const double cutoff = qualities[order[keepCount - 1]];
            for (size_t rank = 0; rank < n; ++rank)
            {
                const size_t idx = order[rank];
                out[idx].kept = (rank < keepCount);
                out[idx].referenceQuality = cutoff;
            }
        }
    }
    else if (tone.qualityFilterMode == QualityFilterMode::GlobalMedian)
    {
        const double globalMedian = medianOf(qualities);
        const double minAllowed = tone.minQualityRatio > 0.0 ? tone.minQualityRatio * globalMedian : -1.0;
        for (size_t i = 0; i < n; ++i)
        {
            out[i].referenceQuality = globalMedian;
            out[i].kept = (qualities[i] >= minAllowed);
        }
    }
    else // QualityFilterMode::LocalMedian
    {
        std::vector<double> window;
        window.reserve(static_cast<size_t>(2 * radius + 1));
        for (size_t i = 0; i < n; ++i)
        {
            const size_t lo = i > static_cast<size_t>(radius) ? i - static_cast<size_t>(radius) : 0;
            const size_t hi = std::min(n - 1, i + static_cast<size_t>(radius));
            window.clear();
            for (size_t j = lo; j <= hi; ++j) window.push_back(qualities[j]);
            const double localMed = medianOf(window);
            out[i].referenceQuality = localMed;
            out[i].kept = (tone.minQualityRatio <= 0.0) || (qualities[i] >= tone.minQualityRatio * localMed);
        }
    }

    for (size_t i = 0; i < std::min(n, manualOverrides.size()); ++i)
    {
        if (manualOverrides[i].has_value())
        {
            out[i].kept = *manualOverrides[i];
            out[i].overridden = true;
        }
    }

    const bool anyKept = std::any_of(out.begin(), out.end(), [](const StackFilterDecision& d) { return d.kept; });
    if (!anyKept)
    {
        const auto bestIt = std::max_element(qualities.begin(), qualities.end());
        const size_t bestIdx = static_cast<size_t>(bestIt - qualities.begin());
        out[bestIdx].kept = true;
    }
    return out;
}

TimelapseExportResult exportTimelapseFromFits(const QString& fitsDir,
    const QString& outputDir,
    const VideoToneSettings& tone,
    const QRect& commonOverlap,
    std::function<void(int done, int total)> progress,
    std::atomic<bool>* cancel,
    const std::map<QString, bool>& stackOverrides)
{
    TimelapseExportResult res;
    if (!tone.writeMp4 && !tone.writeGif && !tone.writeSer)
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

    std::vector<std::optional<bool>> manualOverrides;
    if (!stackOverrides.empty())
    {
        manualOverrides.resize(entries.size());
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto it = stackOverrides.find(QFileInfo(entries[i].path).fileName());
            if (it != stackOverrides.end()) manualOverrides[i] = it->second;
        }
    }
    const std::vector<StackFilterDecision> decisions = evaluateStackQualityFilter(qualities, tone, manualOverrides);

    std::vector<QString> keptPaths;
    std::vector<double> keptQualities;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (decisions[i].kept)
        {
            keptPaths.push_back(entries[i].path);
            keptQualities.push_back(entries[i].quality);
        }
        else ++res.stacksDropped;
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
    const QString ser = tone.writeSer ? QDir(outputDir).filePath("timelapse.ser") : QString();

    const int total = static_cast<int>(keptPaths.size());
    const int passes = (tone.writeMp4 ? 1 : 0) + (tone.writeGif ? 1 : 0) + (tone.writeSer ? 1 : 0);
    int emitted = 0;

    if (tone.writeMp4 || tone.writeGif)
    {
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
    }

    if (tone.writeSer)
    {
        QFile serFile(ser);
        if (!serFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            res.error = QString("Could not open %1 for writing: %2").arg(ser, serFile.errorString());
            return res;
        }
        SerWriter writer(serFile);
        bool started = false;
        for (int i = 0; i < total; ++i)
        {
            if (cancel && cancel->load())
            {
                serFile.close();
                QFile::remove(ser);
                res.error = "cancelled";
                return res;
            }
            FitsImage img;
            if (!readFitsFloat(keptPaths[static_cast<size_t>(i)], img, res.error))
            {
                serFile.close();
                QFile::remove(ser);
                return res;
            }
            const FitsImage* refPtr = (i == refIdx) ? &img : &refImg;
            Frame frame = fitsToSerFrame(img, tone.stretchSer, black, white, tone.gamma, crop,
                refPtr, tone.brightnessMode, alignedPixels);
            frame.frameIndex = static_cast<uint64_t>(i);
            if (!started)
            {
                const std::string camera = !img.meta.camera.empty() ? img.meta.camera : refImg.meta.camera;
                const std::string telescope = !img.meta.telescope.empty() ? img.meta.telescope : refImg.meta.telescope;
                if (!writer.start(frame, camera, res.error, false, telescope))
                {
                    serFile.close();
                    QFile::remove(ser);
                    return res;
                }
                started = true;
            }
            if (!writer.append(frame, res.error))
            {
                serFile.close();
                QFile::remove(ser);
                return res;
            }
            ++emitted;
            if (progress) progress(emitted, total * std::max(1, passes));
        }
        if (!writer.finish(res.error) || !serFile.flush())
        {
            if (res.error.isEmpty()) res.error = QString("Could not flush %1").arg(ser);
            serFile.close();
            QFile::remove(ser);
            return res;
        }
        serFile.close();
    }

    res.ok = true;
    res.mp4Path = mp4;
    res.gifPath = gif;
    res.serPath = ser;
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
    std::atomic<bool>* cancel,
    const std::map<QString, bool>& stackOverrides,
    const RealignOptions& realign)
{
    TimelapseExportResult res;

    const bool doRealign = (realign.mode != StackRealignMode::Off);
    if (doRealign) rebuildVideoOnly = false;

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
        }, cancel, stackOverrides);
        return res;
    }

    const QString stacksDir = QDir(sessionDir).filePath("stacks");
    const QStringList files = sortedFitsFiles(stacksDir);
    if (files.isEmpty())
    {
        res.error = QString("No unsharpened stacks found in %1").arg(stacksDir);
        return res;
    }

    const int total = static_cast<int>(files.size());
    std::vector<StackFilterDecision> decisions;
    int refIdx = 0;
    FitsImage refStack;

    if (doRealign)
    {
        // Pre-evaluate quality filtering so the new commonOverlap is intersected
        // strictly across kept stacks (dropped cloudy frames never shrink the crop).
        std::vector<double> qualities;
        qualities.reserve(static_cast<size_t>(total));
        std::vector<std::optional<bool>> manualOverrides;
        if (!stackOverrides.empty()) manualOverrides.resize(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i)
        {
            if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
            FitsMetadata meta;
            int w = 0, h = 0, ch = 0;
            if (!readFitsMetadata(files[static_cast<size_t>(i)], meta, w, h, ch, res.error))
                return res;
            qualities.push_back(meta.qualityScore);
            if (!stackOverrides.empty())
            {
                const auto it = stackOverrides.find(QFileInfo(files[static_cast<size_t>(i)]).fileName());
                if (it != stackOverrides.end()) manualOverrides[static_cast<size_t>(i)] = it->second;
            }
        }
        decisions = evaluateStackQualityFilter(qualities, tone, manualOverrides);

        int foundRef = -1;
        if (!realign.referenceFileName.isEmpty())
        {
            for (int i = 0; i < total; ++i)
            {
                if (QFileInfo(files[static_cast<size_t>(i)]).fileName() == realign.referenceFileName)
                {
                    foundRef = i;
                    break;
                }
            }
        }
        if (foundRef < 0)
        {
            double bestQ = -1.0;
            for (int i = 0; i < total; ++i)
            {
                if (decisions[static_cast<size_t>(i)].kept && qualities[static_cast<size_t>(i)] > bestQ)
                {
                    bestQ = qualities[static_cast<size_t>(i)];
                    foundRef = i;
                }
            }
        }
        refIdx = std::max(0, foundRef);
        if (!readFitsFloat(files[static_cast<size_t>(refIdx)], refStack, res.error))
            return res;
    }

    const QString stamp = utcStamp(std::chrono::system_clock::now());
    const QString prof = sanitizeProfileName(sharpening.profileName);
    const QString prefix = doRealign ? QString("realigned") : QString("sharpened");
    outTargetDir = createUniqueChildDir(sessionDir, QString("%1_%2_%3").arg(prefix, prof, stamp));
    if (outTargetDir.isEmpty())
    {
        res.error = QString("Could not create output folder under %1").arg(sessionDir);
        return res;
    }
    const QString sharpOut = QDir(outTargetDir).filePath("sharpened");
    if (!QDir().mkpath(sharpOut))
    {
        res.error = QString("Could not create %1").arg(sharpOut);
        return res;
    }
    QString alignedStacksOut;
    if (doRealign && realign.saveAlignedStacks)
    {
        alignedStacksOut = QDir(outTargetDir).filePath("stacks");
        if (!QDir().mkpath(alignedStacksOut))
        {
            res.error = QString("Could not create %1").arg(alignedStacksOut);
            return res;
        }
    }

    std::optional<QRect> newOverlap;
    for (int i = 0; i < total; ++i)
    {
        if (cancel && cancel->load()) { res.error = "cancelled"; return res; }
        FitsImage work;
        if (!readFitsFloat(files[static_cast<size_t>(i)], work, res.error)) return res;

        if (doRealign)
        {
            QRect stackValid;
            if (i == refIdx)
            {
                work.meta.shiftX = 0.0;
                work.meta.shiftY = 0.0;
                work.meta.rotationDeg = 0.0;
                stackValid = fitsValidOverlapRect(work);
            }
            else
            {
                SessionAlignResult aligned = realignStackToReference(
                    work, refStack, realign.mode, realign.allowRotation, realign.featureBox);
                work = std::move(aligned.aligned);
                stackValid = aligned.validOverlap;
            }
            if (decisions[static_cast<size_t>(i)].kept && stackValid.isValid() && !stackValid.isEmpty())
            {
                newOverlap = newOverlap ? newOverlap->intersected(stackValid) : stackValid;
            }
            if (!alignedStacksOut.isEmpty())
            {
                const QString alignedFile = QDir(alignedStacksOut).filePath(QFileInfo(files[static_cast<size_t>(i)]).fileName());
                if (!writeFitsFloat(alignedFile, work, res.error)) return res;
            }
        }

        const FitsImage sharpened = applyLinearSharpening(work, sharpening);
        const QString outFile = QDir(sharpOut).filePath(QFileInfo(files[static_cast<size_t>(i)]).fileName());
        if (!writeFitsFloat(outFile, sharpened, res.error)) return res;
        if (progress)
            progress(i + 1, total, doRealign ? QString("Re-aligning & sharpening stacks…") : QString("Sharpening FITS stacks…"));
    }

    std::optional<QRect> finalOverlap = savedOverlap;
    if (doRealign)
    {
        if (newOverlap && newOverlap->width() >= 2 && newOverlap->height() >= 2)
            finalOverlap = newOverlap;
        else
            finalOverlap = QRect(0, 0, refStack.width, refStack.height);
    }

    QString profileErr;
    (void)saveProfileJson(QDir(outTargetDir).filePath("sharpening_profile.json"),
        sharpening, tone, profileErr, finalOverlap);

    res = exportTimelapseFromFits(sharpOut, outTargetDir, tone, finalOverlap.value_or(QRect{}), [&](int d, int t) {
        if (progress) progress(d, t, "Encoding video…");
    }, cancel, stackOverrides);
    return res;
}
