#include "guiding/guider.h"

#include "recording/event_writer.h"
#include "recording/metadata_text.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <vector>

namespace {

QString hostTime()
{
    return utcText(std::chrono::system_clock::now());
}

QString quotedText(const std::string& text)
{
    return '"' + QString::fromStdString(text).replace('"', '\'') + '"';
}

QString signedValue(double value, int decimals)
{
    return (value >= 0 ? "+" : "") + QString::number(value, 'f', decimals);
}

QString vectorText(ImageVector vector, double scale, int decimals)
{
    return QString("%1,%2").arg(vector.x * scale, 0, 'f', decimals).arg(vector.y * scale, 0, 'f', decimals);
}

QString yesNo(bool value)
{
    return value ? "yes" : "no";
}

QString mountFields(const MountState& state)
{
    return QString(" tracking=%1 slewing=%2 parked=%3 pier=%4")
        .arg(yesNo(state.tracking), yesNo(state.slewing), yesNo(state.parked), pierSideName(state.pierSide));
}

int64_t steadyTicks()
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

} // namespace

const char* guideModeName(GuideMode mode)
{
    switch (mode)
    {
    case GuideMode::Idle:
        return "idle";
    case GuideMode::Calibrating:
        return "calibrating";
    case GuideMode::Guiding:
        return "guiding";
    }
    return "unknown";
}

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

void Guider::requestConnect(MountFactory factory)
{
    {
        std::lock_guard lock(mutex_);
        requests_.connect = std::move(factory);
        requests_.disconnect = false;
    }
    wake_.notify_one();
}

void Guider::requestDisconnect()
{
    {
        std::lock_guard lock(mutex_);
        requests_.connect.reset();
        requests_.disconnect = true;
    }
    wake_.notify_one();
}

void Guider::requestCalibration()
{
    {
        std::lock_guard lock(mutex_);
        requests_.calibrate = true;
        requests_.guide.reset();
    }
    wake_.notify_one();
}

void Guider::requestGuiding(bool on)
{
    {
        std::lock_guard lock(mutex_);
        requests_.guide = on;
        if (on) requests_.calibrate = false;
    }
    wake_.notify_one();
}

void Guider::setTolerance(double pixels)
{
    {
        std::lock_guard lock(mutex_);
        requests_.tolerance = pixels;
    }
    wake_.notify_one();
}

void Guider::setLostAfter(double seconds)
{
    {
        std::lock_guard lock(mutex_);
        requests_.lostAfter = seconds;
    }
    wake_.notify_one();
}

void Guider::requestStopTracking()
{
    {
        std::lock_guard lock(mutex_);
        requests_.stopTracking = true;
        requests_.calibrate = false;
        requests_.guide.reset();
    }
    wake_.notify_one();
}

void Guider::endGuiding(std::string reason)
{
    {
        std::lock_guard lock(mutex_);
        requests_.end = std::move(reason);
        requests_.calibrate = false;
        requests_.guide.reset();
    }
    wake_.notify_one();
}

GuiderStatus Guider::status() const
{
    notifyPending_.store(false);
    GuiderStatus copy;
    {
        std::lock_guard lock(statusMutex_);
        copy = status_;
    }
    const int64_t started = callStarted_.load();
    if (started != 0)
    {
        const auto running = std::chrono::steady_clock::duration{steadyTicks() - started};
        copy.guide.mountNotResponding = std::chrono::duration<double>(running).count() > kMountCallLimitSeconds;
    }
    return copy;
}

void Guider::publish()
{
    {
        std::lock_guard lock(statusMutex_);
        status_.lock = tracker_.status();
        GuideStatus& guide = status_.guide;
        guide.mountName = mount_ ? mount_->name() : std::string{};
        guide.mountConnected = mount_ != nullptr;
        guide.mount = mountState_;
        guide.mountError = mountError_;
        guide.mode = mode_;
        guide.calibrationProgress = calibrator_ ? calibrator_->progress() : std::string{};
        guide.calibration = calibration_;
        guide.guideSpeedPixelsPerFrame = calibration_ ? GuideController::guideSpeedPixelsPerFrame(*calibration_, frameInterval_) : 0.0;
        guide.tolerancePixels = tolerance_;
        guide.frameInterval = frameInterval_;
        guide.raErrorPixels = controller_ ? controller_->raErrorPixels() : 0.0;
        guide.decErrorPixels = controller_ ? controller_->decErrorPixels() : 0.0;
        guide.pulses = pulses_;
        guide.lastPulse = lastPulse_;
        guide.lastPulseTime = lastPulseTime_;
        guide.message = message_;
        guide.unplannedStops = unplannedStops_;
        guide.trackingStops = trackingStops_;
        guide.trackingStopResult = trackingStopResult_;
    }
    if (statusChanged_ && !notifyPending_.exchange(true)) statusChanged_();
}

template <typename Call>
auto Guider::mountCall(Call&& call)
{
    struct Watch
    {
        std::atomic<int64_t>& started;
        explicit Watch(std::atomic<int64_t>& value) : started(value) { started.store(steadyTicks()); }
        ~Watch() { started.store(0); }
    } watch(callStarted_);
    return call();
}

void Guider::log(const QString& lines, bool recording)
{
    if (recording && !lines.isEmpty()) writer_.logGuiding(lines);
}

void Guider::noteFrame(const Frame& frame)
{
    if (lastFrame_ && frame.frameIndex > lastFrame_->first && frame.timestamp > lastFrame_->second)
    {
        const double seconds = std::chrono::duration<double>(frame.timestamp - lastFrame_->second).count();
        frameIntervals_.push_back(seconds / static_cast<double>(frame.frameIndex - lastFrame_->first));
        while (frameIntervals_.size() > 15) frameIntervals_.pop_front();
        std::vector<double> sorted(frameIntervals_.begin(), frameIntervals_.end());
        std::sort(sorted.begin(), sorted.end());
        frameInterval_ = sorted[sorted.size() / 2];
    }
    lastFrame_ = std::pair{frame.frameIndex, frame.timestamp};
}

GuideObservation Guider::observation(const MountState& state) const
{
    const LockStatus& lock = tracker_.status();
    GuideObservation o;
    o.now = std::chrono::steady_clock::now();
    o.locked = lock.state == LockState::Locked;
    o.foundDx = lock.foundDx;
    o.foundDy = lock.foundDy;
    o.estimateValid = lock.estimateValid;
    o.estimateSamples = lock.estimateSamples;
    o.estimateDx = lock.estimateDx;
    o.estimateDy = lock.estimateDy;
    o.mount = state;
    o.frameInterval = frameInterval_;
    return o;
}

void Guider::stopActivity(const std::string& reason, bool recording, bool unplanned)
{
    if (mode_ == GuideMode::Idle) return;
    if (unplanned && mode_ == GuideMode::Guiding) ++unplannedStops_;
    const char* what = mode_ == GuideMode::Calibrating ? " guide_calibration_stopped" : " guide_stop";
    mode_ = GuideMode::Idle;
    calibrator_.reset();
    controller_.reset();
    message_ = reason;
    log(hostTime() + what + " reason=" + quotedText(reason) + "\n", recording);
}

void Guider::connectMount(const MountFactory& factory, bool recording)
{
    disconnectMount(recording);
    mountError_.clear();
    message_.clear();
    try
    {
        std::unique_ptr<Mount> mount = factory();
        mountCall([&] { mount->connect(); });
        mountState_ = mountCall([&] { return mount->state(); });
        lastStateRead_ = std::chrono::steady_clock::now();
        mount_ = std::move(mount);
        log(hostTime() + " guide_mount_connect name=" + quotedText(mount_->name()) + mountFields(mountState_)
                + QString(" can_pulse_guide=%1 alt_azimuth=%2\n").arg(yesNo(mountState_.canPulseGuide), yesNo(mountState_.altAzimuth)),
            recording);
    }
    catch (const std::exception& error)
    {
        mountError_ = std::string("Could not connect: ") + error.what();
        mountState_ = {};
        log(hostTime() + " guide_mount_error reason=" + quotedText(mountError_) + "\n", recording);
    }
}

void Guider::disconnectMount(bool recording)
{
    if (!mount_) return;
    stopActivity("The mount was disconnected.", recording);
    const std::string name = mount_->name();
    try
    {
        mountCall([&] { mount_->disconnect(); });
    }
    catch (const std::exception& error)
    {
        mountError_ = std::string("Disconnecting: ") + error.what();
    }
    mount_.reset();
    mountState_ = {};
    calibration_.reset();
    log(hostTime() + " guide_mount_disconnect name=" + quotedText(name) + "\n", recording);
}

void Guider::stopTracking(bool recording)
{
    stopActivity("The session ended.", recording);
    std::string result;
    if (!mount_)
    {
        result = "no mount is connected";
    }
    else
    {
        try
        {
            mountState_ = mountCall([&] { return mount_->state(); });
            if (!mountState_.tracking)
            {
                result = "the mount was already not tracking";
            }
            else if (!mountState_.canSetTracking)
            {
                result = "the mount cannot stop its tracking from SeleneWatch; it is still tracking";
            }
            else
            {
                mountCall([&] { mount_->stopTracking(); });
                mountState_ = mountCall([&] { return mount_->state(); });
                result = mountState_.tracking ? "the mount still reports tracking" : "tracking stopped";
            }
            mountError_.clear();
        }
        catch (const std::exception& error)
        {
            mountError_ = error.what();
            result = std::string("the mount did not answer (") + error.what() + "); it may still be tracking";
        }
        lastStateRead_ = std::chrono::steady_clock::now();
    }
    trackingStopResult_ = result;
    ++trackingStops_;
    log(hostTime() + " guide_tracking_stop result=" + quotedText(result) + "\n", recording);
}

void Guider::handleMountRequests(Requests& requests, bool recording)
{
    if (requests.end) stopActivity(*requests.end, recording);
    if (requests.lostAfter) tracker_.setSearchSeconds(*requests.lostAfter);
    if (requests.stopTracking) stopTracking(recording);
    if (requests.disconnect) disconnectMount(recording);
    if (requests.connect) connectMount(*requests.connect, recording);
    if (requests.tolerance)
    {
        tolerance_ = *requests.tolerance;
        if (controller_) controller_->setTolerance(tolerance_);
    }
    if (requests.guide && !*requests.guide) stopActivity("Guiding stopped by the observer.", recording);

    const bool wantsCalibration = requests.calibrate;
    const bool wantsGuiding = requests.guide && *requests.guide;
    if (!wantsCalibration && !wantsGuiding) return;
    if (!mount_)
    {
        message_ = "Connect a mount first.";
        return;
    }
    try
    {
        mountState_ = mountCall([&] { return mount_->state(); });
        lastStateRead_ = std::chrono::steady_clock::now();
    }
    catch (const std::exception& error)
    {
        mountError_ = error.what();
        message_ = std::string("The mount did not answer: ") + error.what();
        return;
    }
    const GuideObservation o = observation(mountState_);

    if (wantsCalibration)
    {
        stopActivity("Calibration requested.", recording);
        std::string refusal = mountRefusal(mountState_);
        if (refusal.empty() && !o.locked) refusal = "There is no lock. Lock on a feature with clear detail first.";
        if (!refusal.empty())
        {
            message_ = "Cannot calibrate: " + refusal;
            return;
        }
        calibrator_.emplace();
        calibration_.reset();
        mode_ = GuideMode::Calibrating;
        message_.clear();
        pulses_ = 0;
        lastPulse_.reset();
        log(hostTime() + " guide_calibration_start" + mountFields(mountState_) + "\n", recording);
        return;
    }

    if (mode_ == GuideMode::Guiding) return;
    if (!calibration_)
    {
        message_ = "Cannot guide: calibrate first.";
        return;
    }
    GuideController::Settings settings;
    settings.tolerancePixels = tolerance_;
    if (std::string refusal = GuideController::refusal(*calibration_, settings, o); !refusal.empty())
    {
        message_ = "Cannot guide: " + refusal;
        return;
    }
    stopActivity("Guiding requested.", recording);
    controller_.emplace(*calibration_, settings);
    mode_ = GuideMode::Guiding;
    message_.clear();
    pulses_ = 0;
    lastPulse_.reset();
    log(hostTime() + QString(" guide_start tolerance_px=%1 speed_px_per_frame=%2 gain=%3 max_pulse_ms=%4 settle_s=%5\n")
                         .arg(tolerance_, 0, 'f', 1)
                         .arg(GuideController::guideSpeedPixelsPerFrame(*calibration_, frameInterval_), 0, 'f', 3)
                         .arg(settings.gain).arg(settings.maxPulseMs).arg(settings.settleSeconds),
        recording);
}

void Guider::execute(const GuideDecision& decision, bool calibration, bool recording)
{
    if (decision.restartEstimate) tracker_.restartEstimate();
    if (!decision.pulse) return;
    const GuidePulse pulse = *decision.pulse;
    const LockStatus& lock = tracker_.status();
    try
    {
        mountCall([&] { mount_->pulseGuide(pulse.direction, pulse.milliseconds); });
    }
    catch (const std::exception& error)
    {
        mountError_ = error.what();
        stopActivity(std::string("The mount refused a guide pulse: ") + error.what(), recording, true);
        return;
    }
    ++pulses_;
    lastPulse_ = pulse;
    lastPulseTime_ = std::chrono::system_clock::now();
    const double settle = calibration ? Calibrator::kSettleSeconds : GuideController::Settings{}.settleSeconds;
    QString line = utcText(lastPulseTime_)
        + QString(" guide_pulse purpose=%1 direction=%2 ms=%3")
              .arg(calibration ? "calibration" : "guiding", guideDirectionName(pulse.direction)).arg(pulse.milliseconds)
        + " estimate=" + (lock.estimateValid ? signedValue(lock.estimateDx, 2) + "," + signedValue(lock.estimateDy, 2) : QString("none"))
        + " found=" + signedValue(lock.foundDx, 2) + "," + signedValue(lock.foundDy, 2);
    if (!calibration && controller_)
        line += " ra_error_px=" + signedValue(controller_->raErrorPixels(), 2) + " dec_error_px=" + signedValue(controller_->decErrorPixels(), 2);
    line += mountFields(mountState_) + "\n";
    GuidePulseRecord record;
    record.start = lastPulseTime_;
    record.milliseconds = pulse.milliseconds;
    record.settleSeconds = settle;
    record.direction = guideDirectionName(pulse.direction);
    record.calibration = calibration;
    // The pulse is always kept for the clips; its line is written while Monitoring.
    writer_.logGuiding(recording ? line : QString(), record);
}

bool Guider::stepMount(bool recording)
{
    if (!mount_) return false;
    const auto now = std::chrono::steady_clock::now();
    if (mode_ == GuideMode::Idle && std::chrono::duration<double>(now - lastStateRead_).count() < kIdleStateSeconds) return false;
    try
    {
        mountState_ = mountCall([&] { return mount_->state(); });
        lastStateRead_ = now;
        mountError_.clear();
    }
    catch (const std::exception& error)
    {
        lastStateRead_ = now;
        mountError_ = error.what();
        stopActivity(std::string("The mount did not answer: ") + error.what(), recording, true);
        return true;
    }

    // Safety rule 5: a calibration holds for the side of pier it was made on.
    if (calibration_ && mode_ != GuideMode::Guiding && mountState_.pierSide != calibration_->pierSide)
    {
        calibration_.reset();
        message_ = "The mount changed its side of pier; the calibration was discarded. Calibrate again.";
        log(hostTime() + " guide_calibration_discarded reason=\"pier side changed\"\n", recording);
    }

    const GuideObservation o = observation(mountState_);
    if (mode_ == GuideMode::Calibrating)
    {
        execute(calibrator_->step(o), true, recording);
        if (mode_ == GuideMode::Calibrating && calibrator_->finished())
        {
            if (calibrator_->failed())
            {
                message_ = calibrator_->failure();
                log(hostTime() + " guide_calibration_failed reason=" + quotedText(message_) + "\n", recording);
            }
            else
            {
                calibration_ = calibrator_->result();
                const GuideCalibration& c = *calibration_;
                message_ = "Calibrated.";
                log(hostTime() + " guide_calibration west_px_per_s=" + vectorText(c.west, 1000, 3) + " north_px_per_s="
                        + vectorText(c.north, 1000, 3) + " drift_px_per_s=" + vectorText(c.drift, 1, 3)
                        + QString(" axis_angle_deg=%1 backlash_ms=%2 pier=%3 arcsec_per_px=%4\n")
                              .arg(c.axisAngleDegrees, 0, 'f', 1).arg(c.backlashMs).arg(pierSideName(c.pierSide))
                              .arg(c.arcsecPerPixel ? QString::number(*c.arcsecPerPixel, 'f', 3) : QString("unknown")),
                    recording);
            }
            mode_ = GuideMode::Idle;
            calibrator_.reset();
        }
    }
    else if (mode_ == GuideMode::Guiding)
    {
        execute(controller_->step(o), false, recording);
        if (mode_ == GuideMode::Guiding && controller_->stopped())
        {
            if (controller_->calibrationInvalid()) calibration_.reset();
            const std::string reason = controller_->stopReason();
            stopActivity(reason, recording, true);
        }
    }
    return true;
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
            auto ready = [&] {
                return pending_ != nullptr || requests_.unlock || requests_.discontinuity || requests_.connect
                    || requests_.disconnect || requests_.calibrate || requests_.guide || requests_.tolerance || requests_.end
                    || requests_.lostAfter || requests_.stopTracking;
            };
            // With a mount, the guider also wakes to follow pulses and read the mount's state.
            if (mount_) wake_.wait_for(lock, stopToken, std::chrono::milliseconds{100}, ready);
            else wake_.wait(lock, stopToken, ready);
            if (stopToken.stop_requested()) break;
            frame = std::exchange(pending_, nullptr);
            recording = recording_;
            requests.unlock = std::exchange(requests_.unlock, false);
            requests.discontinuity = std::exchange(requests_.discontinuity, false);
            // A lock is taken from a frame, so the request waits for one.
            if (frame) requests.lock = std::exchange(requests_.lock, std::nullopt);
            requests.connect = std::exchange(requests_.connect, std::nullopt);
            requests.disconnect = std::exchange(requests_.disconnect, false);
            requests.calibrate = std::exchange(requests_.calibrate, false);
            requests.guide = std::exchange(requests_.guide, std::nullopt);
            requests.tolerance = std::exchange(requests_.tolerance, std::nullopt);
            requests.end = std::exchange(requests_.end, std::nullopt);
            requests.lostAfter = std::exchange(requests_.lostAfter, std::nullopt);
            requests.stopTracking = std::exchange(requests_.stopTracking, false);
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
            if (requests.discontinuity)
            {
                tracker_.discontinuity();
                lastFrame_.reset();
            }
            if (frame)
            {
                noteFrame(*frame);
                if (requests.lock)
                {
                    tracker_.lock(*frame, *requests.lock);
                    locked = true;
                }
                else
                {
                    const LockState state = tracker_.status().state;
                    measured = tracker_.step(*frame);
                    // A layout change releases the lock inside step.
                    unlocked = unlocked || (state != LockState::Off && tracker_.status().state == LockState::Off);
                }
            }
        }
        catch (const std::exception& error)
        {
            tracker_.unlock(std::string("The lock failed: ") + error.what());
            unlocked = true;
        }
        frame.reset();
        // The observer's lock or unlock stops guiding as planned; a lock the
        // frame's layout released, or one lost for the search time, does not.
        if (locked || unlocked)
            stopActivity(locked ? "A new lock was taken." : "The lock was released.", recording, !locked && !requests.unlock);
        else if (before.state != LockState::Lost && tracker_.status().state == LockState::Lost)
            stopActivity("The lock was lost: " + tracker_.status().message, recording, true);
        {
            std::lock_guard lock(mutex_);
            wanted_.store(tracker_.active() || requests_.lock.has_value());
        }

        const LockStatus& after = tracker_.status();
        logLock(before, after, locked, unlocked, measured, recording);

        bool changed = locked || unlocked || measured;
        try
        {
            handleMountRequests(requests, recording);
            changed = stepMount(recording) || changed;
        }
        catch (const std::exception& error)
        {
            mountError_ = error.what();
            stopActivity(std::string("Guiding failed: ") + error.what(), recording, true);
        }
        if (changed || requests.connect || requests.disconnect || requests.calibrate || requests.guide || requests.tolerance
            || requests.end || requests.stopTracking)
            publish();
    }

    // Leaving: guiding ends, and the mount is released on the thread that made it.
    stopActivity("SeleneWatch is closing.", false);
    try
    {
        if (mount_) mountCall([&] { mount_->disconnect(); });
    }
    catch (...)
    {
    }
    mount_.reset();
}

void Guider::logLock(const LockStatus& before, const LockStatus& after, bool locked, bool unlocked, bool measured, bool recording)
{
    // Nothing is written in Preview; the first step after Monitoring starts writes an offset line at once.
    if (!recording)
    {
        lastLogTime_.reset();
        return;
    }
    const QString time = utcText(after.frameTime);
    const QString frame = QString(" frame=%1").arg(after.frameIndex);
    QString lines;
    if (unlocked && before.state != LockState::Off)
        lines += time + " guide_unlock" + frame + " reason=" + quotedText(after.message) + "\n";
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
            lines += time + " guide_refused" + frame + " reason=" + quotedText(after.message) + "\n";
        }
        lastLogTime_ = after.frameTime;
    }
    if (!measured)
    {
        log(lines, recording);
        return;
    }

    if (after.state != before.state)
    {
        lines += time + QString(" guide_state state=\"%1\"").arg(lockStateName(after.state)) + frame
            + " reason=" + quotedText(after.message) + "\n";
    }
    const bool due = !lastLogTime_ || after.frameTime < *lastLogTime_
        || after.frameTime - *lastLogTime_ >= std::chrono::seconds{1};
    if (due && after.lastMatch)
    {
        const LockMatch& match = *after.lastMatch;
        lines += time + QString(" guide_offset state=\"%1\" mode=%2").arg(lockStateName(after.state), guideModeName(mode_)) + frame
            + QString(" region=%1,%2,%3,%4 binning=%5")
                  .arg(after.region.x).arg(after.region.y).arg(after.region.width).arg(after.region.height).arg(after.binning)
            + " dx=" + signedValue(match.dx, 2) + " dy=" + signedValue(match.dy, 2)
            + " score=" + QString::number(match.score, 'f', 3) + (match.valid ? " valid=yes" : " valid=no")
            + (after.estimateValid
                      ? " estimate_dx=" + signedValue(after.estimateDx, 2) + " estimate_dy=" + signedValue(after.estimateDy, 2)
                      : QString(" estimate=none"))
            + QString(" samples=%1\n").arg(after.estimateSamples);
        lastLogTime_ = after.frameTime;
        if (recording)
        {
            // Kept by the writer so a clip can say what guiding was doing at its trigger.
            GuideStateRecord record;
            record.time = after.frameTime;
            record.state = std::string(lockStateName(after.state))
                + (mode_ == GuideMode::Idle ? "" : std::string(", ") + guideModeName(mode_));
            record.dx = after.foundDx;
            record.dy = after.foundDy;
            writer_.logGuiding(lines, std::nullopt, record);
            return;
        }
    }
    log(lines, recording);
}
