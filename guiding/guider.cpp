#include "guiding/guider.h"

#include "recording/event_writer.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>

#include <exception>
#include <utility>

namespace {

QString utcText(std::chrono::system_clock::time_point time)
{
    const qint64 epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString quoted(const std::string& text)
{
    return '"' + QString::fromStdString(text).replace('"', '\'') + '"';
}

QString signedValue(double value, int decimals)
{
    return (value >= 0 ? "+" : "") + QString::number(value, 'f', decimals);
}

} // namespace

Guider::Guider(EventWriter& writer, std::function<void()> statusChanged)
    : writer_(writer)
    , statusChanged_(std::move(statusChanged))
    , thread_([this](std::stop_token stopToken) { run(stopToken); })
{
}

Guider::~Guider()
{
    shutdown();
}

void Guider::shutdown()
{
    if (!thread_.joinable()) return;
    thread_.request_stop();
    wake_.notify_all();
    thread_.join();
}

void Guider::offer(const FramePtr& frame, bool recording)
{
    if (!wanted_.load(std::memory_order_relaxed)) return;
    FramePtr replaced;
    {
        std::lock_guard lock(mutex_);
        replaced = std::exchange(pending_, frame);
        recording_ = recording;
    }
    wake_.notify_one();
}

void Guider::discontinuity()
{
    {
        std::lock_guard lock(mutex_);
        requests_.discontinuity = true;
    }
    wake_.notify_one();
}

void Guider::requestLock(LockRegion region)
{
    {
        std::lock_guard lock(mutex_);
        requests_.lock = region;
        requests_.unlock = false;
        wanted_.store(true);
    }
    wake_.notify_one();
}

void Guider::requestUnlock()
{
    {
        std::lock_guard lock(mutex_);
        requests_.lock.reset();
        requests_.unlock = true;
    }
    wake_.notify_one();
}

LockStatus Guider::status() const
{
    notifyPending_.store(false);
    std::lock_guard lock(statusMutex_);
    return status_;
}

void Guider::publish(const LockStatus& status)
{
    {
        std::lock_guard lock(statusMutex_);
        status_ = status;
    }
    if (statusChanged_ && !notifyPending_.exchange(true)) statusChanged_();
}

void Guider::run(std::stop_token stopToken)
{
    for (;;)
    {
        FramePtr frame;
        bool recording = false;
        Requests requests;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stopToken, [&] {
                return pending_ != nullptr || requests_.unlock || requests_.discontinuity;
            });
            if (stopToken.stop_requested()) return;
            frame = std::exchange(pending_, nullptr);
            recording = recording_;
            requests.unlock = std::exchange(requests_.unlock, false);
            requests.discontinuity = std::exchange(requests_.discontinuity, false);
            // A lock is taken from a frame, so the request waits for one.
            if (frame) requests.lock = std::exchange(requests_.lock, std::nullopt);
        }

        const LockStatus before = tracker_.status();
        bool locked = false, unlocked = false, measured = false;
        try
        {
            if (requests.unlock && tracker_.status().state != LockState::Off)
            {
                tracker_.unlock("Released by the observer.");
                unlocked = true;
            }
            if (requests.discontinuity) tracker_.discontinuity();
            if (frame && requests.lock)
            {
                tracker_.lock(*frame, *requests.lock);
                locked = true;
            }
            else if (frame)
            {
                const LockState state = tracker_.status().state;
                measured = tracker_.step(*frame);
                // A layout change releases the lock inside step.
                unlocked = unlocked || (state != LockState::Off && tracker_.status().state == LockState::Off);
            }
        }
        catch (const std::exception& error)
        {
            tracker_.unlock(std::string("The lock failed: ") + error.what());
            unlocked = true;
        }
        frame.reset();
        {
            std::lock_guard lock(mutex_);
            wanted_.store(tracker_.active() || requests_.lock.has_value());
        }

        const LockStatus& after = tracker_.status();
        if (recording)
        {
            const QString lines = logLines(before, after, locked, unlocked, measured);
            if (!lines.isEmpty()) writer_.logGuiding(lines);
        }
        if (locked || unlocked || measured) publish(after);
    }
}

QString Guider::logLines(const LockStatus& before, const LockStatus& after, bool locked, bool unlocked, bool measured)
{
    const QString time = utcText(after.frameTime);
    const QString frame = QString(" frame=%1").arg(after.frameIndex);
    QString lines;
    if (unlocked && before.state != LockState::Off)
        lines += time + " guide_unlock" + frame + " reason=" + quoted(after.message) + "\n";
    if (locked)
    {
        if (after.state == LockState::Locked)
        {
            lines += time + " guide_lock" + frame
                + QString(" region=%1,%2,%3,%4 binning=%5 contrast=%6 structure=%7 saturated=%8\n")
                      .arg(after.region.x).arg(after.region.y).arg(after.region.width).arg(after.region.height)
                      .arg(after.binning).arg(after.quality.contrast, 0, 'f', 1)
                      .arg(after.quality.structureRatio, 0, 'f', 3).arg(after.quality.saturatedFraction, 0, 'f', 3);
        }
        else
        {
            lines += time + " guide_refused" + frame + " reason=" + quoted(after.message) + "\n";
        }
        lastLogTime_ = after.frameTime;
    }
    if (!measured) return lines;

    if (after.state != before.state)
    {
        lines += time + QString(" guide_state state=\"%1\"").arg(lockStateName(after.state)) + frame
            + " reason=" + quoted(after.message) + "\n";
    }
    const bool due = !lastLogTime_ || after.frameTime < *lastLogTime_
        || after.frameTime - *lastLogTime_ >= std::chrono::seconds{1};
    if (due && after.lastMatch)
    {
        const LockMatch& match = *after.lastMatch;
        lines += time + QString(" guide_offset state=\"%1\"").arg(lockStateName(after.state)) + frame
            + QString(" region=%1,%2,%3,%4 binning=%5")
                  .arg(after.region.x).arg(after.region.y).arg(after.region.width).arg(after.region.height).arg(after.binning)
            + " dx=" + signedValue(match.dx, 2) + " dy=" + signedValue(match.dy, 2)
            + " score=" + QString::number(match.score, 'f', 3) + (match.valid ? " valid=yes" : " valid=no")
            + (after.estimateValid
                      ? " estimate_dx=" + signedValue(after.estimateDx, 2) + " estimate_dy=" + signedValue(after.estimateDy, 2)
                      : QString(" estimate=none"))
            + QString(" samples=%1\n").arg(after.estimateSamples);
        lastLogTime_ = after.frameTime;
    }
    return lines;
}
