#include "timelapse/timelapse_session.h"

#include "recording/metadata_text.h"
#include "recording/ser_writer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QTextStream>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <opencv2/core.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

QString createUniqueSessionDir(const QString& root, const QString& stamp)
{
    if (!QDir().mkpath(root)) return {};
    const QDir parent(root);
    for (int attempt = 1; attempt <= 999; ++attempt)
    {
        const QString name = attempt == 1 ? stamp : QString("%1-%2").arg(stamp).arg(attempt);
        if (parent.mkdir(name)) return parent.filePath(name);
        if (!parent.exists(name)) break;
    }
    return {};
}

std::optional<double> parseOptionalDouble(const std::string& text)
{
    if (text.empty()) return std::nullopt;
    try
    {
        size_t idx = 0;
        const double val = std::stod(text, &idx);
        if (idx == 0 || !std::isfinite(val)) return std::nullopt;
        return val;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// Parses exposure text with explicit unit awareness:
// - Live cameras format as "<N> us" (microseconds)
// - Playback logs (FireCapture) format as "<N>ms", "<N>s", or "<N>us"
double parseExposureSeconds(const std::string& text)
{
    if (text.empty()) return 0.0;
    try
    {
        size_t idx = 0;
        const double val = std::stod(text, &idx);
        if (idx == 0 || !std::isfinite(val) || val <= 0.0) return 0.0;

        while (idx < text.size() && std::isspace(static_cast<unsigned char>(text[idx])))
            ++idx;
        std::string unit;
        for (size_t i = idx; i < text.size() && unit.size() < 8; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            if (std::isspace(ch) || ch == '(' || ch == ',' || ch == ';') break;
            unit.push_back(static_cast<char>(std::tolower(ch)));
        }

        // UTF-8 micro sign U+00B5 (0xC2 0xB5) or Greek mu U+03BC (0xCE 0xBC) + "s".
        const bool isMicro = unit == "us" || unit == "usec" || unit == "usecs"
            || unit == "\xc2\xb5s" || unit == "\xce\xbcs";
        if (isMicro) return val * 1e-6;
        if (unit == "ms" || unit == "msec" || unit == "msecs") return val * 1e-3;
        if (unit == "s" || unit == "sec" || unit == "secs") return val;
        // Bare number without unit: treat as milliseconds for backwards compatibility.
        return val * 1e-3;
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
    bool dirsOk = !sessionDir_.isEmpty()
        && QDir(sessionDir_).mkdir("stacks")
        && QDir(sessionDir_).mkdir("sharpened")
        && (!config_.settings.saveRawSer || QDir(sessionDir_).mkdir("raw"));
    if (!dirsOk)
    {
        initError_ = QString("Could not create time-lapse session folder under %1")
            .arg(QDir::toNativeSeparators(config_.timelapsesRoot));
        accepting_.store(false);
        status_.active = false;
        status_.stopReason = initError_;
        workerFinished_ = true;
        return;
    }

    QString err;
    (void)saveProfileJson(QDir(sessionDir_).filePath("sharpening_profile.json"),
        config_.settings.sharpening, config_.settings.video, err);

    status_.active = true;
    status_.sessionDir = sessionDir_;

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

void TimelapseSession::setBeforeStackForTests(std::function<void(int periodIndex)> hook)
{
    std::lock_guard lock(mutex_);
    beforeStackForTests_ = std::move(hook);
}

void TimelapseSession::cancelExport()
{
    exportCancel_.store(true);
}

bool TimelapseSession::budgetHasRoomFor(size_t bytes) const
{
    const MemoryUsage mem = config_.memory.usage();
    const size_t allowed = mem.limit / 100 * kBudgetHeadroomPercent;
    return mem.used <= allowed && bytes <= allowed - mem.used;
}

bool TimelapseSession::tryReserveBurstMemory(size_t bytes)
{
    if (bytes == 0) return true;
    // Check-then-acquire is not atomic, but only this (source) thread adds burst
    // bytes; a concurrent camera allocation can at most use part of the headroom.
    return budgetHasRoomFor(bytes) && config_.memory.acquire(bytes);
}

// A full-frame copy owned by the time-lapse, so the camera's pooled frame goes
// straight back for reuse. The copy's pixels are charged to the session budget
// by its allocator; nullptr when the headroom or the ceiling refuses it.
FramePtr TimelapseSession::copyRawFrame(const Frame& frame) const
{
    if (!budgetHasRoomFor(frame.data.size())) return {};
    try
    {
        auto copy = std::make_shared<Frame>(config_.memory);
        copy->width = frame.width;
        copy->height = frame.height;
        copy->format = frame.format;
        copy->significantBits = frame.significantBits;
        copy->alignment = frame.alignment;
        copy->bayer = frame.bayer;
        copy->frameIndex = frame.frameIndex;
        copy->timestamp = frame.timestamp;
        copy->steadyTimestamp = frame.steadyTimestamp;
        copy->blip = frame.blip;
        copy->syntheticFlash = frame.syntheticFlash;
        copy->testSource = frame.testSource;
        copy->sourceDroppedFrames = frame.sourceDroppedFrames;
        copy->data.assign(frame.data.begin(), frame.data.end());
        return copy;
    }
    catch (const MemoryLimitError&) { return {}; }
    catch (const std::bad_alloc&) { return {}; }
}

void TimelapseSession::releaseBurstMemory(size_t bytes)
{
    if (bytes > 0) config_.memory.release(bytes);
}

void TimelapseSession::discardCurrentBurstLocked()
{
    currentHeap_.clear();
    releaseBurstMemory(currentBurstBytes_);
    currentBurstBytes_ = 0;
    collecting_ = false;
    currentCaptured_ = 0;
    currentRamRefused_ = 0;
}

void TimelapseSession::onFrame(const FramePtr& frame)
{
    if (!accepting_.load(std::memory_order_acquire) || !frame) return;

    {
        std::lock_guard collectorLock(collectorMutex_);
        if (!accepting_.load(std::memory_order_relaxed)) return;

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

        if (collecting_ && tSec >= windowEndSec_)
            finishCurrentWindowLocked(/*allowAfterStop=*/false);

        startNextWindowIfDueLocked(tSec, frame->timestamp);

        if (collecting_)
        {
            std::optional<QRect> qualityRoi;
            if (config_.settings.lunarAnchor && config_.settings.lunarAnchor->isValid())
                qualityRoi = config_.settings.lunarAnchor;
            else if (config_.lockRegion)
                qualityRoi = QRect(config_.lockRegion->x, config_.lockRegion->y,
                    config_.lockRegion->width, config_.lockRegion->height);

            RawBurstFrame captured;
            try
            {
                captured = captureRawBurstFrame(*frame, effectiveCropRoi_, qualityRoi);
            }
            catch (...)
            {
                // Never allow an allocation/scoring failure in time-lapse collection
                // to escape onto the camera acquisition thread.
                captured = {};
            }

            if (captured.width > 0 && captured.height > 0)
            {
                ++currentCaptured_;
                windowEndUtc_ = frame->timestamp;
                const size_t cap = static_cast<size_t>(
                    std::clamp(config_.settings.maxFramesPerPeriod, 1, TimelapseSettings::kMaxFramesCap));
                const size_t frameMem = captured.data.size();

                auto minCmp = [](const RawBurstFrame& a, const RawBurstFrame& b) {
                    return a.sharpness > b.sharpness; // std::push_heap puts smallest sharpness at front
                };
                const bool heapHasRoom = currentHeap_.size() < cap;
                const bool beatsWorst = !heapHasRoom && !currentHeap_.empty()
                    && captured.sharpness > currentHeap_.front().sharpness;
                // Copy the full raw frame only for a frame that will enter the burst.
                // Without the copy the frame is not admitted, so the SER always holds
                // exactly the frames that were stacked.
                bool admissible = heapHasRoom || beatsWorst;
                if (admissible && config_.settings.saveRawSer)
                {
                    captured.rawFullFrame = copyRawFrame(*frame);
                    if (!captured.rawFullFrame)
                    {
                        admissible = false;
                        ++currentRamRefused_;
                    }
                }

                if (admissible && heapHasRoom)
                {
                    if (tryReserveBurstMemory(frameMem))
                    {
                        currentBurstBytes_ += frameMem;
                        currentHeap_.push_back(std::move(captured));
                        std::push_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                    }
                    else
                    {
                        ++currentRamRefused_;
                    }
                }
                else if (admissible)
                {
                    std::pop_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                    const size_t oldBytes = currentHeap_.back().data.size();
                    if (frameMem <= oldBytes || tryReserveBurstMemory(frameMem - oldBytes))
                    {
                        if (frameMem > oldBytes) currentBurstBytes_ += (frameMem - oldBytes);
                        else if (oldBytes > frameMem)
                        {
                            releaseBurstMemory(oldBytes - frameMem);
                            currentBurstBytes_ -= (oldBytes - frameMem);
                        }
                        // Assigning replaces the evicted entry, immediately dropping any
                        // evicted rawFullFrame FramePtr back to the FramePool.
                        currentHeap_.back() = std::move(captured);
                        std::push_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                    }
                    else
                    {
                        ++currentRamRefused_;
                        std::push_heap(currentHeap_.begin(), currentHeap_.end(), minCmp);
                    }
                }
            }
        }

        // Update lightweight timing telemetry under lock without allocating.
        {
            std::lock_guard lock(mutex_);
            status_.elapsedSeconds = tSec;
            const double interval = config_.settings.intervalSeconds;
            const int nextK = std::max(0, currentPeriodIndex_ + 1);
            status_.nextBurstInSeconds = collecting_ ? 0.0 : std::max(0.0, nextK * interval - tSec);
        }
    }
}

void TimelapseSession::startNextWindowIfDueLocked(double tSec, std::chrono::system_clock::time_point utcNow)
{
    if (collecting_ || !accepting_.load(std::memory_order_relaxed)) return;

    const double interval = config_.settings.intervalSeconds;
    const int k = static_cast<int>(std::floor(tSec / interval + 1e-9));
    if (k <= currentPeriodIndex_) return;

    const double kStart = k * interval;
    const double kWindowEnd = kStart + config_.settings.captureWindowSeconds;

    // Record any intermediate periods skipped due to stalls or timestamp jumps
    // (capped at 1000 entries to guard against malformed timestamps).
    const int firstMissed = currentPeriodIndex_ + 1;
    const int lastMissed = (tSec >= kWindowEnd) ? k : (k - 1);
    if (lastMissed >= firstMissed)
    {
        const int logCount = std::min(lastMissed - firstMissed + 1, 1000);
        TimelapseStatus snap;
        {
            std::lock_guard lock(mutex_);
            status_.periodsSkipped += logCount;
            consecutiveSkips_ += logCount;
            status_.overloadWarning = true;
            overloadWarningText_ = QString("Capture window missed (%1 skipped).").arg(status_.periodsSkipped);
            for (int m = firstMissed; m < firstMissed + logCount; ++m)
            {
                skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"window missed\"")
                    .arg(m).arg(utcText(utcNow)));
            }
            refreshWarningTextLocked();
            snap = status_;
        }
        postStatusSnapshot(std::move(snap));
    }

    if (tSec >= kWindowEnd)
    {
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
        TimelapseStatus snap;
        {
            std::lock_guard lock(mutex_);
            ++status_.periodsSkipped;
            ++consecutiveSkips_;
            status_.overloadWarning = true;
            overloadWarningText_ = QString("Processing fell behind interval (%1 skipped; %2).")
                .arg(status_.periodsSkipped).arg(reason);
            skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"%3\"")
                .arg(k).arg(utcText(utcNow), reason));
            refreshWarningTextLocked();
            snap = status_;
        }
        postStatusSnapshot(std::move(snap));
        return;
    }

    collecting_ = true;
    windowStartSec_ = kStart;
    windowEndSec_ = kWindowEnd;
    windowStartUtc_ = utcNow;
    windowEndUtc_ = utcNow;
    currentCaptured_ = 0;
    currentRamRefused_ = 0;
    currentHeap_.clear();
    currentBurstBytes_ = 0;
}

void TimelapseSession::finishCurrentWindowLocked(bool allowAfterStop)
{
    if (!collecting_) return;
    collecting_ = false;
    if (currentCaptured_ == 0 || currentHeap_.empty())
    {
        discardCurrentBurstLocked();
        return;
    }

    // Keep best N% capped at maxFramesPerPeriod (<= 200).
    const int maxAllowed = std::clamp(config_.settings.maxFramesPerPeriod, 1, TimelapseSettings::kMaxFramesCap);
    const size_t want = static_cast<size_t>(std::clamp(
        static_cast<int>(std::ceil(static_cast<double>(currentCaptured_) * config_.settings.keepPercent / 100.0)),
        1, maxAllowed));

    std::sort(currentHeap_.begin(), currentHeap_.end(), [](const RawBurstFrame& a, const RawBurstFrame& b) {
        return a.sharpness > b.sharpness;
    });
    size_t keptBytes = currentBurstBytes_;
    if (currentHeap_.size() > want)
    {
        size_t droppedBytes = 0;
        for (size_t i = want; i < currentHeap_.size(); ++i)
            droppedBytes += currentHeap_[i].data.size();
        // Resizing drops both raw pixel bytes and any rawFullFrame FramePtrs for evicted frames.
        currentHeap_.resize(want);
        releaseBurstMemory(droppedBytes);
        keptBytes -= droppedBytes;
    }

    BurstJob job;
    job.periodIndex = currentPeriodIndex_;
    job.windowStartUtc = windowStartUtc_;
    job.windowEndUtc = windowEndUtc_;
    job.framesCaptured = currentCaptured_;
    job.ramRefused = currentRamRefused_;
    job.frames = std::move(currentHeap_);
    job.chargedBytes = keptBytes;
    currentHeap_.clear();
    currentBurstBytes_ = 0;

    bool droppedAtHandoff = false;
    std::optional<TimelapseStatus> statusSnap;
    {
        std::lock_guard lock(mutex_);
        if (pendingJob_.has_value() || (!allowAfterStop && !accepting_.load()))
        {
            droppedAtHandoff = true;
            if (accepting_.load())
            {
                ++status_.periodsSkipped;
                ++consecutiveSkips_;
                status_.overloadWarning = true;
                overloadWarningText_ = QString("Processing fell behind (%1 skipped).").arg(status_.periodsSkipped);
                skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"queue full at handoff\"")
                    .arg(job.periodIndex).arg(utcText(job.windowStartUtc)));
                refreshWarningTextLocked();
                statusSnap = status_;
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
        if (statusSnap) postStatusSnapshot(std::move(*statusSnap));
    }
}

void TimelapseSession::setGuidingStatus(const GuiderStatus& status, bool guidingWanted)
{
    std::optional<TimelapseStatus> snap;
    {
        std::lock_guard lock(mutex_);
        const bool lost = guidingWanted && status.lock.state != LockState::Locked;
        if (status_.guidingLostWarning != lost)
        {
            status_.guidingLostWarning = lost;
            refreshWarningTextLocked();
            snap = status_;
        }
    }
    if (snap) postStatusSnapshot(std::move(*snap));
}

void TimelapseSession::refreshWarningTextLocked()
{
    if (!fatalWarningText_.isEmpty())
        status_.warningText = fatalWarningText_;
    else if (status_.diskLowWarning && !diskLowWarningText_.isEmpty())
        status_.warningText = diskLowWarningText_;
    else if (status_.guidingLostWarning)
        status_.warningText = "Guiding lock lost during time-lapse.";
    else if (status_.overloadWarning && !overloadWarningText_.isEmpty())
        status_.warningText = overloadWarningText_;
    else if (status_.cloudWarning)
        status_.warningText = "Sharpness dropped sharply (possible cloud).";
    else
        status_.warningText.clear();
}

void TimelapseSession::stop(const std::string& reason)
{
    {
        std::lock_guard collectorLock(collectorMutex_);
        const bool wasAccepting = accepting_.exchange(false);
        if (wasAccepting && collecting_)
            finishCurrentWindowLocked(/*allowAfterStop=*/true);
        else if (collecting_)
            discardCurrentBurstLocked();

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
    TimelapseStatus snap;
    {
        std::lock_guard lock(mutex_);
        snap = status_;
    }
    postStatusSnapshot(std::move(snap));
}

void TimelapseSession::postStatusSnapshot(TimelapseStatus snap) const
{
    if (!config_.statusChanged || !context_) return;
    QMetaObject::invokeMethod(context_, [cb = config_.statusChanged, snap = std::move(snap)] { cb(snap); },
        Qt::QueuedConnection);
}

void TimelapseSession::requestStopLocked(const std::string& reason, bool discardPending)
{
    if (!stopRequested_)
    {
        stopRequested_ = true;
        stopReason_ = reason;
    }
    if (discardPending) discardPending_ = true;
}

void TimelapseSession::workerLoop(std::stop_token stopToken)
{
    std::string finalReason = "stopped by the observer";
    std::optional<BurstJob> discarded;
    for (;;)
    {
        std::optional<BurstJob> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stopToken, [this] { return pendingJob_.has_value() || stopRequested_; });
            if (pendingJob_ && !discardPending_)
            {
                job = std::move(pendingJob_);
                pendingJob_.reset();
            }
            else if (stopRequested_ || stopToken.stop_requested())
            {
                // A burst queued behind the one that ended the session (target
                // reached, disk reserve, write failure, internal error) is not stacked.
                if (pendingJob_)
                {
                    discarded = std::move(pendingJob_);
                    pendingJob_.reset();
                }
                finalReason = stopReason_.empty() ? "stopped" : stopReason_;
                break;
            }
        }

        if (job)
        {
            processBurstGuarded(*job);
            job->frames.clear(); // Frees any raw copies before the bytes are released
            releaseBurstMemory(job->chargedBytes);

            if (config_.settings.stopKind == StopConditionKind::StackCount)
            {
                std::lock_guard lock(mutex_);
                if (status_.stacksCompleted >= config_.settings.targetStacks)
                {
                    accepting_.store(false);
                    requestStopLocked("target stack count reached", /*discardPending=*/true);
                }
            }
        }
    }

    if (discarded)
    {
        problemLogs_.push_back(QString("discarded period=%1 utc=%2 reason=\"%3\"")
            .arg(discarded->periodIndex).arg(utcText(discarded->windowStartUtc), QString::fromStdString(finalReason)));
        discarded->frames.clear();
        releaseBurstMemory(discarded->chargedBytes);
    }

    finalizeSession(finalReason);
}

// The worker thread's exception boundary. A RAM refusal costs one period; any
// other exception is unexpected, so the session stops rather than stacking on
// in an unknown state. The program itself always keeps running.
void TimelapseSession::processBurstGuarded(BurstJob& job)
{
    try
    {
        processBurst(job);
    }
    catch (const MemoryLimitError&)
    {
        skipBurstAfterError(job, "RAM limit while stacking");
    }
    catch (const std::bad_alloc&)
    {
        skipBurstAfterError(job, "out of memory while stacking");
    }
    catch (const cv::Exception& ex)
    {
        if (ex.code == cv::Error::StsNoMem)
            skipBurstAfterError(job, "out of memory while stacking");
        else
            stopAfterInternalError(&job, QString::fromStdString(ex.what()).trimmed());
    }
    catch (const std::exception& ex)
    {
        stopAfterInternalError(&job, QString::fromUtf8(ex.what()).trimmed());
    }
    catch (...)
    {
        stopAfterInternalError(&job, "unknown exception");
    }
}

void TimelapseSession::skipBurstAfterError(const BurstJob& job, const QString& reason)
{
    TimelapseStatus snap;
    {
        std::lock_guard lock(mutex_);
        ++status_.periodsSkipped;
        status_.overloadWarning = true;
        overloadWarningText_ = QString("Stack skipped: %1 (%2 skipped).").arg(reason).arg(status_.periodsSkipped);
        skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"%3\"")
            .arg(job.periodIndex).arg(utcText(job.windowStartUtc), reason));
        refreshWarningTextLocked();
        snap = status_;
    }
    postStatusSnapshot(std::move(snap));
}

void TimelapseSession::stopAfterInternalError(const BurstJob* job, const QString& what)
{
    accepting_.store(false);
    problemLogs_.push_back(QString("error period=%1 utc=%2 what=\"%3\"")
        .arg(job ? job->periodIndex : -1)
        .arg(job ? utcText(job->windowStartUtc) : utcText(std::chrono::system_clock::now()), what));
    TimelapseStatus snap;
    {
        std::lock_guard lock(mutex_);
        if (job)
        {
            ++status_.periodsSkipped;
            skippedLogs_.push_back(QString("skip period=%1 utc=%2 reason=\"internal error\"")
                .arg(job->periodIndex).arg(utcText(job->windowStartUtc)));
        }
        fatalWarningText_ = QString("Internal error while stacking (%1) — time-lapse stopped.").arg(what);
        refreshWarningTextLocked();
        requestStopLocked("internal error: " + what.toStdString(), /*discardPending=*/true);
        snap = status_;
    }
    postStatusSnapshot(std::move(snap));
}

void TimelapseSession::processBurst(BurstJob& job)
{
    const auto tStart = std::chrono::steady_clock::now();

    std::chrono::milliseconds delay{0};
    std::function<void(int)> beforeStack;
    {
        std::lock_guard lock(mutex_);
        delay = stackDelay_;
        beforeStack = beforeStackForTests_;
    }
    if (delay.count() > 0) std::this_thread::sleep_for(delay);
    if (beforeStack) beforeStack(job.periodIndex);

    // Free-space safety check before writing anything to disk.
    const uint64_t reserve = config_.diskReserveBytes;
    if (reserve > 0 && config_.freeSpace)
    {
        const auto free = config_.freeSpace(sessionDir_);
        if (free && *free < reserve)
        {
            accepting_.store(false);
            TimelapseStatus snap;
            {
                std::lock_guard lock(mutex_);
                status_.diskLowWarning = true;
                diskLowWarningText_ = QString("Low disk space (%1 GiB free, reserve %2 GiB) — stopping capture and assembling video.")
                    .arg(static_cast<double>(*free) / kGiB, 0, 'f', 1)
                    .arg(static_cast<double>(reserve) / kGiB, 0, 'f', 0);
                refreshWarningTextLocked();
                requestStopLocked("disk reserve", /*discardPending=*/true);
                snap = status_;
            }
            postStatusSnapshot(std::move(snap));
            return;
        }
    }

    // Write optional raw SER strictly from the kept frames in chronological order,
    // releasing the raw copies immediately afterwards. A failure costs only the
    // SER; it is recorded and the stack is still made.
    if (config_.settings.saveRawSer && !sessionDir_.isEmpty())
    {
        QString serError;
        if (!writeRawSer(job, serError))
        {
            problemLogs_.push_back(QString("raw_ser_error period=%1 utc=%2 error=\"%3\"")
                .arg(job.periodIndex).arg(utcText(job.windowStartUtc), serError));
        }
    }
    for (auto& f : job.frames) f.rawFullFrame.reset();

    std::optional<QRect> alignRoi;
    if (!effectiveCropRoi_ && config_.settings.lunarAnchor && config_.settings.lunarAnchor->isValid())
        alignRoi = config_.settings.lunarAnchor;
    else if (!effectiveCropRoi_ && config_.lockRegion)
        alignRoi = QRect(config_.lockRegion->x, config_.lockRegion->y,
            config_.lockRegion->width, config_.lockRegion->height);

    BurstStackResult stacked = stackBurst(job.frames, alignRoi);
    if (!stacked.ok)
    {
        skipBurstAfterError(job, QString("stacking failed: %1").arg(stacked.error));
        return;
    }

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
        referenceQuality_ = avgSharp;
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
        // If an early stack (within the first few intervals) is substantially sharper
        // than a cloudy/smeared opening reference, promote its already-aligned image
        // to be the session alignment reference so later stacks lock onto crisp detail
        // without introducing any coordinate jump relative to stack 1.
        if (stackLogs_.size() < 4 && avgSharp > std::max(referenceQuality_ * 2.0, 1e-4))
        {
            referenceStack_ = aligned.aligned;
            referenceQuality_ = avgSharp;
        }
    }

    // Track running median quality so an early cloudy stack does not permanently
    // depress the cloud-detection baseline.
    qualityHistory_.push_back(avgSharp);
    {
        std::vector<double> sortedQ = qualityHistory_;
        std::sort(sortedQ.begin(), sortedQ.end());
        baselineQuality_ = sortedQ[sortedQ.size() / 2];
    }

    const auto midOffset = (job.windowEndUtc - job.windowStartUtc) / 2;
    FitsMetadata meta;
    meta.dateObs = job.windowStartUtc;
    meta.dateAvg = job.windowStartUtc + midOffset;
    meta.dateEnd = job.windowEndUtc;
    meta.exptimeSeconds = parseExposureSeconds(config_.stream.exposure);
    if (const auto parsedGain = parseOptionalDouble(config_.stream.gain))
    {
        meta.gain = *parsedGain;
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
    const bool rawOk = !sessionDir_.isEmpty()
        && writeFitsFloat(QDir(sessionDir_).filePath("stacks/" + fitsName), aligned.aligned, writeErr);
    const bool sharpOk = rawOk
        && writeFitsFloat(QDir(sessionDir_).filePath("sharpened/" + fitsName), sharpened, writeErr);
    if (!sharpOk)
    {
        accepting_.store(false);
        TimelapseStatus snap;
        {
            std::lock_guard lock(mutex_);
            fatalWarningText_ = QString("FITS write failed (%1) — stopping capture.").arg(writeErr);
            refreshWarningTextLocked();
            requestStopLocked("FITS write failed: " + writeErr.toStdString(), /*discardPending=*/true);
            snap = status_;
        }
        postStatusSnapshot(std::move(snap));
        return;
    }

    // The stack is on disk; from here a failure must not lose its log entry.
    QImage thumb;
    try
    {
        double b = 0, w = 1;
        computeAutoStretchLevels(sharpened, b, w);
        thumb = toneMapFitsToImage(sharpened, b, w, config_.settings.video.gamma, 1.0, {}, 512);
    }
    catch (...)
    {
        thumb = {}; // Display only
    }
    const double procSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
    const bool cloudDrop = baselineQuality_ > 0 && avgSharp < 0.35 * baselineQuality_;

    stackLogs_.push_back({stackIdx, fitsName, meta.dateObs, meta.dateEnd, meta.framesCaptured, meta.framesKept,
        avgSharp, aligned.shiftX, aligned.shiftY, aligned.rotationDeg, procSec, job.ramRefused});

    TimelapseStatus snap;
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
            overloadWarningText_ = QString("Stacking took %1 s (near %2 s interval).")
                .arg(procSec, 0, 'f', 1).arg(config_.settings.intervalSeconds, 0, 'f', 0);
        }
        else if (consecutiveSkips_ == 0)
        {
            status_.overloadWarning = false;
            overloadWarningText_.clear();
        }
        refreshWarningTextLocked();
        snap = status_;
    }
    postStatusSnapshot(std::move(snap));
}

bool TimelapseSession::writeRawSer(BurstJob& job, QString& error)
{
    std::vector<FramePtr> rawKept;
    rawKept.reserve(job.frames.size());
    for (auto& f : job.frames)
        if (f.rawFullFrame) rawKept.push_back(std::move(f.rawFullFrame));
    if (rawKept.empty())
    {
        error = "no raw frames kept";
        return false;
    }
    std::sort(rawKept.begin(), rawKept.end(), [](const FramePtr& a, const FramePtr& b) {
        return a->frameIndex < b->frameIndex;
    });
    const QString serName = QString("%1_%2.ser").arg(job.periodIndex + 1, 4, 10, QChar('0'))
        .arg(utcStamp(job.windowStartUtc));
    QFile serFile(QDir(sessionDir_).filePath("raw/" + serName));
    if (!serFile.open(QIODevice::WriteOnly))
    {
        error = QString("raw/%1: %2").arg(serName, serFile.errorString());
        return false;
    }
    SerWriter sw(serFile);
    QString err;
    bool ok = sw.start(*rawKept.front(), config_.stream.camera, err, false, config_.site.telescope);
    for (size_t i = 0; ok && i < rawKept.size(); ++i)
        ok = sw.append(*rawKept[i], err);
    if (ok) ok = sw.finish(err);
    if (!ok)
    {
        error = QString("raw/%1 (%2 of %3 frames): %4").arg(serName).arg(sw.frames()).arg(rawKept.size()).arg(err);
        return false;
    }
    return true;
}

void TimelapseSession::finalizeSession(const std::string& reason)
{
    accepting_.store(false);
    {
        // Release any uncommitted collector burst (e.g., if worker stopped on
        // StackCount, disk reserve, or FITS write error without another onFrame).
        std::lock_guard collectorLock(collectorMutex_);
        if (collecting_ || !currentHeap_.empty() || currentBurstBytes_ > 0)
            discardCurrentBurstLocked();
    }

    TimelapseStatus snap;
    {
        std::lock_guard lock(mutex_);
        status_.finalizing = true;
        status_.stopReason = QString::fromStdString(reason);
        snap = status_;
    }
    postStatusSnapshot(std::move(snap));

    // Persist final commonOverlap alongside sharpening/video settings so Re-sharpen
    // and Rebuild can apply the exact same valid-overlap crop later.
    if (!sessionDir_.isEmpty())
    {
        QString profErr;
        const std::optional<QRect> overlapOpt = (commonOverlap_.isValid() && !commonOverlap_.isEmpty())
            ? std::optional<QRect>(commonOverlap_) : std::nullopt;
        (void)saveProfileJson(QDir(sessionDir_).filePath("sharpening_profile.json"),
            config_.settings.sharpening, config_.settings.video, profErr, overlapOpt);
    }

    TimelapseExportResult exportRes;
    if (!sessionDir_.isEmpty() && !stackLogs_.empty())
    {
        const auto progress = [this](int done, int total) {
            TimelapseStatus progressSnap;
            {
                std::lock_guard lock(mutex_);
                status_.exportDone = done;
                status_.exportTotal = total;
                progressSnap = status_;
            }
            postStatusSnapshot(std::move(progressSnap));
        };
        try
        {
            exportRes = exportTimelapseFromFits(QDir(sessionDir_).filePath("sharpened"),
                sessionDir_, config_.settings.video, commonOverlap_, progress, &exportCancel_);
        }
        catch (const std::exception& ex)
        {
            exportRes = {};
            exportRes.error = QString("internal error while building the video: %1").arg(QString::fromUtf8(ex.what()));
        }
        catch (...)
        {
            exportRes = {};
            exportRes.error = "internal error while building the video";
        }
        if (!exportRes.ok && exportCancel_.load())
            exportRes.error = "video not built (cancelled when the program closed); rebuild it with Re-sharpen";
    }
    else
    {
        exportRes.ok = stackLogs_.empty();
    }

    std::vector<QString> skippedCopy;
    {
        std::lock_guard lock(mutex_);
        skippedCopy = skippedLogs_;
    }
    if (!sessionDir_.isEmpty())
    {
        try
        {
            writeMetadataLog(QString::fromStdString(reason), exportRes, skippedCopy);
        }
        catch (...)
        {
            // The session must still finish and report; the stacks are on disk.
        }
    }

    TimelapseFinishResult finished;
    TimelapseStatus finalSnap;
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
        finalSnap = status_;
    }
    doneCv_.notify_all();
    postStatusSnapshot(std::move(finalSnap));

    if (config_.finished && context_)
    {
        QMetaObject::invokeMethod(context_, [cb = config_.finished, finished] { cb(finished); }, Qt::QueuedConnection);
    }
}

void TimelapseSession::writeMetadataLog(const QString& reason, const TimelapseExportResult& exportRes,
    const std::vector<QString>& skippedLogs) const
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
    out << "periods_skipped: " << skippedLogs.size() << "\n";
    if (commonOverlap_.isValid() && !commonOverlap_.isEmpty())
    {
        out << "common_overlap: x=" << commonOverlap_.x() << " y=" << commonOverlap_.y()
            << " width=" << commonOverlap_.width() << " height=" << commonOverlap_.height() << "\n";
    }
    out << "video_stacks_used: " << exportRes.stacksUsed << "\n";
    out << "video_stacks_dropped: " << exportRes.stacksDropped << "\n";
    if (!exportRes.mp4Path.isEmpty()) out << "mp4: timelapse.mp4\n";
    if (!exportRes.gifPath.isEmpty()) out << "gif: timelapse.gif\n";
    if (!exportRes.error.isEmpty()) out << "export_error: " << exportRes.error << "\n";
    writeSiteLines(out, config_.site);
    writeStreamLines(out, config_.stream, nullptr);
    for (const QString& s : skippedLogs) out << s << "\n";
    for (const QString& s : problemLogs_) out << s << "\n";
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
            << " proc_s=" << QString::number(e.processSeconds, 'f', 3);
        if (e.ramRefused > 0) out << " ram_refused=" << e.ramRefused;
        out << "\n";
    }
    out.flush();
}
