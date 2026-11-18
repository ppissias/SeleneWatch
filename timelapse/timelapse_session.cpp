#include "timelapse/timelapse_session.h"

#include "recording/metadata_text.h"
#include "recording/ser_writer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

QString createUniqueSessionDir(const QString& root, const QString& stamp)
{
    QDir().mkpath(root);
    const QDir parent(root);
    for (int attempt = 1; attempt <= 999; ++attempt)
    {
        const QString name = attempt == 1 ? stamp : QString("%1-%2").arg(stamp).arg(attempt);
        if (parent.mkdir(name)) return parent.filePath(name);
        if (!parent.exists(name)) break;
    }
    return {};
}

double parseDoubleFromText(const std::string& text, double scale = 1.0)
{
    if (text.empty()) return 0.0;
    try
    {
        size_t idx = 0;
        const double val = std::stod(text, &idx);
        return std::isfinite(val) ? val * scale : 0.0;
    }
    catch (...)
    {
        return 0.0;
    }
}

} // namespace

TimelapseSession::TimelapseSession(QObject* context, Config config)
    : context_(context)
    , config_(std::move(config))
    , useSourceTime_(!config_.stream.playback.empty())
{
    if (!config_.freeSpace) config_.freeSpace = freeBytesAt;

    if (config_.settings.region && config_.settings.region->isValid())
    {
        effectiveCropRoi_ = config_.settings.region;
    }
    else if (config_.settings.cropToLock && config_.lockRegion)
    {
        const LockRegion& lr = *config_.lockRegion;
        const int padX = std::max(32, lr.width / 2);
        const int padY = std::max(32, lr.height / 2);
        effectiveCropRoi_ = QRect(std::max(0, lr.x - padX), std::max(0, lr.y - padY),
            lr.width + 2 * padX, lr.height + 2 * padY);
    }

    const QString stamp = utcStamp(std::chrono::system_clock::now());
    sessionDir_ = createUniqueSessionDir(config_.timelapsesRoot, stamp);
    if (!sessionDir_.isEmpty())
    {
        QDir(sessionDir_).mkdir("stacks");
        QDir(sessionDir_).mkdir("sharpened");
        if (config_.settings.saveRawSer) QDir(sessionDir_).mkdir("raw");
        QString err;
        (void)saveProfileJson(QDir(sessionDir_).filePath("sharpening_profile.json"),
            config_.settings.sharpening, config_.settings.video, err);
    }

    status_.active = true;
    status_.sessionDir = sessionDir_;
    status_.remainingSeconds = config_.settings.stopKind == StopConditionKind::Duration
        ? config_.settings.durationMinutes * 60.0
        : -1.0;

    worker_ = std::jthread([this](std::stop_token stopToken) { workerLoop(stopToken); });
}

TimelapseSession::~TimelapseSession()
{
    stop("the program closed");
    waitFinished();
}

void TimelapseSession::setStackDelay(std::chrono::milliseconds delay)
{
    std::lock_guard lock(mutex_);
    stackDelay_ = delay;
}

bool TimelapseSession::tryReserveBurstMemory(size_t bytes)
{
    if (bytes == 0) return true;
    return config_.memory.acquire(bytes);
}

void TimelapseSession::releaseBurstMemory(size_t bytes)
{
    if (bytes > 0) config_.memory.release(bytes);
}

void TimelapseSession::discardCurrentBurst()
{
    currentHeap_.clear();
    currentRaw_.clear();
    releaseBurstMemory(currentBurstBytes_);
    currentBurstBytes_ = 0;
    collecting_ = false;
    currentCaptured_ = 0;
}

void TimelapseSession::onFrame(const FramePtr& frame)
{
    if (!accepting_.load() || !frame) return;

    if (!clockStarted_)
    {
        clockStarted_ = true;
        startSteady_ = frame->steadyTimestamp != std::chrono::steady_clock::time_point{}
            ? frame->steadyTimestamp : std::chrono::steady_clock::now();
        startSourceUtc_ = frame->timestamp;
        startWallUtc_ = std::chrono::system_clock::now();
    }

    const double tSec = useSourceTime_
        ? std::max(0.0, std::chrono::duration<double>(frame->timestamp - startSourceUtc_).count())
        : std::max(0.0, std::chrono::duration<double>(
            (frame->steadyTimestamp != std::chrono::steady_clock::time_point{}
                ? frame->steadyTimestamp : std::chrono::steady_clock::now()) - startSteady_).count());

    // Check duration / UTC end stop conditions.
    if (config_.settings.stopKind == StopConditionKind::Duration
        && tSec >= config_.settings.durationMinutes * 60.0)
    {
        if (collecting_) finishCurrentWindow();
        stop("duration complete");
        return;
    }
    if (config_.settings.stopKind == StopConditionKind::UtcEnd
        && config_.settings.utcEndTime.isValid())
    {
        const qint64 endMs = config_.settings.utcEndTime.toMSecsSinceEpoch();
        const qint64 frameMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame->timestamp.time_since_epoch()).count();
        if (frameMs >= endMs)
        {
            if (collecting_) finishCurrentWindow();
            stop("UTC end time reached");
            return;
        }
    }

    if (collecting_ && tSec >= windowEndSec_)
        finishCurrentWindow();

    startNextWindowIfDue(tSec, frame->timestamp);

    if (collecting_)
    {
        std::optional<QRect> qualityRoi;
        if (config_.settings.lunarAnchor && config_.settings.lunarAnchor->isValid())
            qualityRoi = config_.settings.lunarAnchor;
        else if (config_.lockRegion)
            qualityRoi = QRect(config_.lockRegion->x, config_.lockRegion->y,
                config_.lockRegion->width, config_.lockRegion->height);

        PreparedBurstFrame prepared = prepareBurstFrame(*frame, effectiveCropRoi_, qualityRoi);
        if (prepared.width > 0 && prepared.height > 0)
        {
            ++currentCaptured_;
            windowEndUtc_ = frame->timestamp;
            const size_t cap = static_cast<size_t>(
                std::clamp(config_.settings.maxFramesPerPeriod, 1, TimelapseSettings::kMaxFramesCap));
            const size_t frameMem = prepared.pixels.size() * sizeof(float);

            auto minCmp = [](const PreparedBurstFrame& a, const PreparedBurstFrame& b) {
                return a.sharpness > b.sharpness; // std::push_heap puts smallest sharpness at front
            };
            if (currentHeap_.size() < cap)
            {
                if (tryReserveBurstMemory(frameMem))
                {
                    currentBurstBytes_ += frameMem;
                    currentHeap_.push_back(std::move(prepared));
                    std::push_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                    if (config_.settings.saveRawSer) currentRaw_.push_back(frame);
                }
            }
            else if (!currentHeap_.empty() && prepared.sharpness > currentHeap_.front().sharpness)
            {
                std::pop_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                const size_t oldBytes = currentHeap_.back().pixels.size() * sizeof(float);
                if (frameMem <= oldBytes || tryReserveBurstMemory(frameMem - oldBytes))
                {
                    if (frameMem > oldBytes) currentBurstBytes_ += (frameMem - oldBytes);
                    else if (oldBytes > frameMem)
                    {
                        releaseBurstMemory(oldBytes - frameMem);
                        currentBurstBytes_ -= (oldBytes - frameMem);
                    }
                    currentHeap_.back() = std::move(prepared);
                    std::push_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                    if (config_.settings.saveRawSer) currentRaw_.push_back(frame);
                }
                else
                {
                    std::push_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                }
            }
        }
    }

    // Update lightweight timing telemetry under lock without allocating.
    {
        std::lock_guard lock(mutex_);
        status_.elapsedSeconds = tSec;
        if (config_.settings.stopKind == StopConditionKind::Duration)
            status_.remainingSeconds = std::max(0.0, config_.settings.durationMinutes * 60.0 - tSec);
        const double interval = config_.settings.intervalSeconds;
        const int nextK = std::max(0, currentPeriodIndex_ + 1);
        status_.nextBurstInSeconds = collecting_ ? 0.0 : std::max(0.0, nextK * interval - tSec);
    }
}

void TimelapseSession::startNextWindowIfDue(double tSec, std::chrono::system_clock::time_point utcNow)
{
    if (collecting_ || !accepting_.load()) return;

    const double interval = config_.settings.intervalSeconds;
    const int k = static_cast<int>(std::floor(tSec / interval + 1e-9));
    if (k <= currentPeriodIndex_) return;

    const double kStart = k * interval;
    const double kWindowEnd = kStart + config_.settings.captureWindowSeconds;
    if (tSec >= kWindowEnd)
    {
        // Fast jump past whole window: mark skipped and advance.
        currentPeriodIndex_ = k;
        return;
    }

    // Pre-capture overload skip gate: skip period k immediately if the depth-1
    // pending slot is still occupied or memory is already tight.
    bool overload = false;
    {
        std::lock_guard lock(mutex_);
        overload = pendingJob_.has_value();
    }
    const MemoryUsage mem = config_.memory.usage();
    const bool memTight = mem.limit > 0 && mem.used >= (mem.limit * 9) / 10;

    currentPeriodIndex_ = k;
    if (overload || memTight)
    {
        const QString reason = overload ? "processing overload" : "memory limit";
        std::lock_guard lock(mutex_);
        ++status_.periodsSkipped;
        ++consecutiveSkips_;
        status_.overloadWarning = true;
        status_.warningText = QString("Processing fell behind interval (%1 skipped; %2).")
            .arg(status_.periodsSkipped).arg(reason);
        skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"%3\"")
            .arg(k).arg(utcText(utcNow), reason));
        postStatus();
        return;
    }

    collecting_ = true;
    windowStartSec_ = kStart;
    windowEndSec_ = kWindowEnd;
    windowStartUtc_ = utcNow;
    windowEndUtc_ = utcNow;
    currentCaptured_ = 0;
    currentHeap_.clear();
    currentRaw_.clear();
    currentBurstBytes_ = 0;
}

void TimelapseSession::finishCurrentWindow()
{
    if (!collecting_) return;
    collecting_ = false;
    if (currentCaptured_ == 0 || currentHeap_.empty())
    {
        discardCurrentBurst();
        return;
    }

    // Keep best N% capped at maxFramesPerPeriod (<= 200).
    const int maxAllowed = std::clamp(config_.settings.maxFramesPerPeriod, 1, TimelapseSettings::kMaxFramesCap);
    const size_t want = static_cast<size_t>(std::clamp(
        static_cast<int>(std::ceil(static_cast<double>(currentCaptured_) * config_.settings.keepPercent / 100.0)),
        1, maxAllowed));

    std::sort(currentHeap_.begin(), currentHeap_.end(), [](const PreparedBurstFrame& a, const PreparedBurstFrame& b) {
        return a.sharpness > b.sharpness;
    });
    size_t keptBytes = currentBurstBytes_;
    if (currentHeap_.size() > want)
    {
        size_t droppedBytes = 0;
        for (size_t i = want; i < currentHeap_.size(); ++i)
            droppedBytes += currentHeap_[i].pixels.size() * sizeof(float);
        currentHeap_.resize(want);
        releaseBurstMemory(droppedBytes);
        keptBytes -= droppedBytes;
    }

    BurstJob job;
    job.periodIndex = currentPeriodIndex_;
    job.windowStartUtc = windowStartUtc_;
    job.windowEndUtc = windowEndUtc_;
    job.framesCaptured = currentCaptured_;
    job.frames = std::move(currentHeap_);
    job.rawFrames = std::move(currentRaw_);
    job.chargedBytes = keptBytes;
    currentHeap_.clear();
    currentRaw_.clear();
    currentBurstBytes_ = 0;

    bool droppedAtHandoff = false;
    {
        std::lock_guard lock(mutex_);
        if (pendingJob_.has_value() || !accepting_.load())
        {
            droppedAtHandoff = true;
            if (accepting_.load())
            {
                ++status_.periodsSkipped;
                ++consecutiveSkips_;
                status_.overloadWarning = true;
                status_.warningText = QString("Processing fell behind (%1 skipped).").arg(status_.periodsSkipped);
                skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"queue full at handoff\"")
                    .arg(job.periodIndex).arg(utcText(job.windowStartUtc)));
            }
        }
        else
        {
            consecutiveSkips_ = 0;
            pendingJob_ = std::move(job);
            cv_.notify_one();
        }
    }
    if (droppedAtHandoff)
    {
        releaseBurstMemory(job.chargedBytes);
        postStatus();
    }
}

void TimelapseSession::setGuidingStatus(const GuiderStatus& status, bool guidingWanted)
{
    std::lock_guard lock(mutex_);
    const bool lost = guidingWanted && status.lock.state != LockState::Locked;
    if (status_.guidingLostWarning != lost)
    {
        status_.guidingLostWarning = lost;
        if (lost) status_.warningText = "Guiding lock lost during time-lapse.";
        postStatus();
    }
}

void TimelapseSession::stop(const std::string& reason)
{
    const bool wasAccepting = accepting_.exchange(false);
    if (wasAccepting && collecting_)
        finishCurrentWindow();
    else if (collecting_)
        discardCurrentBurst();

    {
        std::lock_guard lock(mutex_);
        if (!stopRequested_)
        {
            stopRequested_ = true;
            stopReason_ = reason;
        }
    }
    cv_.notify_all();
}

void TimelapseSession::waitFinished()
{
    std::unique_lock lock(mutex_);
    doneCv_.wait(lock, [this] { return workerFinished_; });
}

TimelapseStatus TimelapseSession::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

void TimelapseSession::postStatus()
{
    if (!config_.statusChanged || !context_) return;
    const TimelapseStatus snap = status_;
    QMetaObject::invokeMethod(context_, [cb = config_.statusChanged, snap] { cb(snap); }, Qt::QueuedConnection);
}

void TimelapseSession::workerLoop(std::stop_token stopToken)
{
    std::string finalReason = "stopped by the observer";
    for (;;)
    {
        std::optional<BurstJob> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stopToken, [this] { return pendingJob_.has_value() || stopRequested_; });
            if (pendingJob_)
            {
                job = std::move(pendingJob_);
                pendingJob_.reset();
            }
            else if (stopRequested_ || stopToken.stop_requested())
            {
                finalReason = stopReason_.empty() ? "stopped" : stopReason_;
                break;
            }
        }

        if (job)
        {
            processBurst(*job);
            releaseBurstMemory(job->chargedBytes);

            if (config_.settings.stopKind == StopConditionKind::StackCount)
            {
                int done = 0;
                {
                    std::lock_guard lock(mutex_);
                    done = status_.stacksCompleted;
                }
                if (done >= config_.settings.targetStacks)
                {
                    accepting_.store(false);
                    std::lock_guard lock(mutex_);
                    stopRequested_ = true;
                    stopReason_ = "target stack count reached";
                }
            }
        }
    }

    finalizeSession(finalReason);
}

void TimelapseSession::processBurst(BurstJob& job)
{
    const auto tStart = std::chrono::steady_clock::now();

    std::chrono::milliseconds delay{0};
    {
        std::lock_guard lock(mutex_);
        delay = stackDelay_;
    }
    if (delay.count() > 0) std::this_thread::sleep_for(delay);

    // Free-space safety check before writing anything to disk.
    const uint64_t reserve = config_.diskReserveBytes;
    if (reserve > 0 && config_.freeSpace)
    {
        const auto free = config_.freeSpace(sessionDir_);
        if (free && *free < reserve)
        {
            accepting_.store(false);
            std::lock_guard lock(mutex_);
            status_.diskLowWarning = true;
            status_.warningText = QString("Low disk space (%1 GiB free, reserve %2 GiB) — stopping capture and assembling video.")
                .arg(static_cast<double>(*free) / kGiB, 0, 'f', 1)
                .arg(static_cast<double>(reserve) / kGiB, 0, 'f', 0);
            stopRequested_ = true;
            stopReason_ = "disk reserve";
            postStatus();
            return;
        }
    }

    if (config_.settings.saveRawSer && !job.rawFrames.empty() && !sessionDir_.isEmpty())
    {
        const QString serName = QString("%1_%2.ser").arg(job.periodIndex + 1, 4, 10, QChar('0'))
            .arg(utcStamp(job.windowStartUtc));
        QFile serFile(QDir(sessionDir_).filePath("raw/" + serName));
        if (serFile.open(QIODevice::WriteOnly))
        {
            SerWriter sw(serFile);
            QString err;
            if (sw.start(*job.rawFrames.front(), config_.stream.camera, err, false, config_.site.telescope))
            {
                for (const auto& rf : job.rawFrames)
                    if (!sw.append(*rf, err)) break;
                (void)sw.finish(err);
            }
        }
    }
    job.rawFrames.clear();

    std::optional<QRect> alignRoi;
    if (!effectiveCropRoi_ && config_.settings.lunarAnchor && config_.settings.lunarAnchor->isValid())
        alignRoi = config_.settings.lunarAnchor;
    else if (!effectiveCropRoi_ && config_.lockRegion)
        alignRoi = QRect(config_.lockRegion->x, config_.lockRegion->y,
            config_.lockRegion->width, config_.lockRegion->height);

    BurstStackResult stacked = stackBurst(job.frames, alignRoi);
    if (!stacked.ok) return;

    double avgSharp = 0;
    for (const auto& f : job.frames) avgSharp += f.sharpness;
    avgSharp /= static_cast<double>(job.frames.size());

    // Shift lunar anchor into cropped space if crop ROI is active.
    std::optional<QRect> anchor = config_.settings.lunarAnchor;
    if (anchor && effectiveCropRoi_)
        anchor = anchor->translated(-effectiveCropRoi_->x(), -effectiveCropRoi_->y());

    SessionAlignResult aligned;
    if (!referenceStack_)
    {
        aligned.aligned = stacked.stack;
        aligned.validOverlap = stacked.validOverlap;
        referenceStack_ = stacked.stack;
        commonOverlap_ = stacked.validOverlap;
        baselineQuality_ = avgSharp;
    }
    else
    {
        aligned = alignStackToReference(stacked.stack, *referenceStack_, config_.settings.preset, anchor);
        const QRect stepOverlap = stacked.validOverlap.intersected(aligned.validOverlap);
        if (commonOverlap_.isEmpty()) commonOverlap_ = stepOverlap;
        else
        {
            const QRect inter = commonOverlap_.intersected(stepOverlap);
            if (inter.width() >= 16 && inter.height() >= 16) commonOverlap_ = inter;
        }
    }

    const auto midOffset = (job.windowEndUtc - job.windowStartUtc) / 2;
    FitsMetadata meta;
    meta.dateObs = job.windowStartUtc;
    meta.dateAvg = job.windowStartUtc + midOffset;
    meta.dateEnd = job.windowEndUtc;
    meta.exptimeSeconds = parseDoubleFromText(config_.stream.exposure, 0.001);
    if (!config_.stream.gain.empty())
    {
        meta.gain = parseDoubleFromText(config_.stream.gain, 1.0);
        meta.hasGain = true;
    }
    meta.camera = config_.stream.camera;
    meta.telescope = config_.site.telescope;
    meta.observer = config_.site.observer;
    meta.siteName = config_.site.site;
    meta.targetPreset = timelapsePresetName(config_.settings.preset);
    meta.framesCaptured = job.framesCaptured;
    meta.framesKept = job.frames.size();
    meta.qualityScore = avgSharp;
    meta.shiftX = aligned.shiftX;
    meta.shiftY = aligned.shiftY;
    meta.rotationDeg = aligned.rotationDeg;
    meta.sharpeningProfile = "none";
    meta.softwareVersion = config_.softwareVersion;
    meta.rawMaxAdu = stacked.stack.meta.rawMaxAdu;
    aligned.aligned.meta = meta;

    const int stackIdx = static_cast<int>(stackLogs_.size()) + 1;
    const QString fitsName = QString("%1_%2.fits").arg(stackIdx, 4, 10, QChar('0')).arg(utcStamp(job.windowStartUtc));
    const FitsImage sharpened = applyLinearSharpening(aligned.aligned, config_.settings.sharpening);

    QString writeErr;
    if (!sessionDir_.isEmpty())
    {
        (void)writeFitsFloat(QDir(sessionDir_).filePath("stacks/" + fitsName), aligned.aligned, writeErr);
        (void)writeFitsFloat(QDir(sessionDir_).filePath("sharpened/" + fitsName), sharpened, writeErr);
    }

    double b = 0, w = 1;
    computeAutoStretchLevels(sharpened, b, w);
    const QImage thumb = toneMapFitsToImage(sharpened, b, w, config_.settings.video.gamma, 1.0, {}, 512);
    const double procSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
    const bool cloudDrop = baselineQuality_ > 0 && avgSharp < 0.35 * baselineQuality_;

    stackLogs_.push_back({stackIdx, fitsName, meta.dateObs, meta.dateEnd, meta.framesCaptured, meta.framesKept,
        avgSharp, aligned.shiftX, aligned.shiftY, aligned.rotationDeg, procSec});

    {
        std::lock_guard lock(mutex_);
        status_.stacksCompleted = stackIdx;
        status_.lastCapturedFrames = meta.framesCaptured;
        status_.lastKeptFrames = meta.framesKept;
        status_.lastQuality = avgSharp;
        status_.lastShiftX = aligned.shiftX;
        status_.lastShiftY = aligned.shiftY;
        status_.lastRotationDeg = aligned.rotationDeg;
        status_.lastProcessingSeconds = procSec;
        status_.latestThumbnail = thumb;
        status_.cloudWarning = cloudDrop;
        if (procSec > config_.settings.intervalSeconds * 0.9)
        {
            status_.overloadWarning = true;
            status_.warningText = QString("Stacking took %1 s (near %2 s interval).")
                .arg(procSec, 0, 'f', 1).arg(config_.settings.intervalSeconds, 0, 'f', 0);
        }
        else if (cloudDrop)
        {
            status_.warningText = "Sharpness dropped sharply (possible cloud).";
        }
        else if (consecutiveSkips_ == 0 && !status_.guidingLostWarning && !status_.diskLowWarning)
        {
            status_.overloadWarning = false;
            status_.warningText.clear();
        }
    }
    postStatus();
}

void TimelapseSession::finalizeSession(const std::string& reason)
{
    {
        std::lock_guard lock(mutex_);
        status_.finalizing = true;
        status_.stopReason = QString::fromStdString(reason);
    }
    postStatus();

    TimelapseExportResult exportRes;
    if (!sessionDir_.isEmpty() && !stackLogs_.empty())
    {
        exportRes = exportTimelapseFromFits(QDir(sessionDir_).filePath("sharpened"),
            sessionDir_, config_.settings.video, commonOverlap_);
    }
    else
    {
        exportRes.ok = stackLogs_.empty();
    }

    if (!sessionDir_.isEmpty())
        writeMetadataLog(QString::fromStdString(reason), exportRes);

    TimelapseFinishResult finished;
    {
        std::lock_guard lock(mutex_);
        status_.active = false;
        status_.finalizing = false;
        workerFinished_ = true;
        finished.ok = exportRes.ok;
        finished.sessionDir = sessionDir_;
        finished.stopReason = status_.stopReason;
        finished.stacksCompleted = status_.stacksCompleted;
        finished.periodsSkipped = status_.periodsSkipped;
        finished.exportResult = exportRes;
    }
    doneCv_.notify_all();
    postStatus();

    if (config_.finished && context_)
    {
        QMetaObject::invokeMethod(context_, [cb = config_.finished, finished] { cb(finished); }, Qt::QueuedConnection);
    }
}

void TimelapseSession::writeMetadataLog(const QString& reason, const TimelapseExportResult& exportRes) const
{
    QFile file(QDir(sessionDir_).filePath("timelapse.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&file);
    out << "software: SeleneWatch " << QString::fromStdString(config_.softwareVersion) << "\n";
    out << "preset: " << timelapsePresetName(config_.settings.preset) << "\n";
    out << "interval_seconds: " << config_.settings.intervalSeconds << "\n";
    out << "capture_window_seconds: " << config_.settings.captureWindowSeconds << "\n";
    out << "keep_percent: " << config_.settings.keepPercent << "\n";
    out << "max_frames_per_period: " << config_.settings.maxFramesPerPeriod << "\n";
    out << "sharpening_profile: " << config_.settings.sharpening.profileName << "\n";
    out << "video_fps: " << config_.settings.video.fps << "\n";
    out << "stop_reason: " << reason << "\n";
    out << "stacks_completed: " << stackLogs_.size() << "\n";
    out << "periods_skipped: " << skippedLogs_.size() << "\n";
    out << "video_stacks_used: " << exportRes.stacksUsed << "\n";
    out << "video_stacks_dropped: " << exportRes.stacksDropped << "\n";
    if (!exportRes.mp4Path.isEmpty()) out << "mp4: timelapse.mp4\n";
    if (!exportRes.gifPath.isEmpty()) out << "gif: timelapse.gif\n";
    if (!exportRes.error.isEmpty()) out << "export_error: " << exportRes.error << "\n";
    writeSiteLines(out, config_.site);
    writeStreamLines(out, config_.stream, nullptr);
    for (const QString& s : skippedLogs_) out << s << "\n";
    for (const StackLogEntry& e : stackLogs_)
    {
        out << "stack: index=" << e.index
            << " file=" << e.fileName
            << " start=" << utcText(e.dateObs)
            << " end=" << utcText(e.dateEnd)
            << " captured=" << e.captured
            << " kept=" << e.kept
            << " quality=" << QString::number(e.quality, 'g', 6)
            << " dx=" << QString::number(e.shiftX, 'f', 2)
            << " dy=" << QString::number(e.shiftY, 'f', 2)
            << " rot_deg=" << QString::number(e.rotationDeg, 'f', 3)
            << " proc_s=" << QString::number(e.processSeconds, 'f', 3) << "\n";
    }
    out.flush();
}
