#include "guiding/guide_controller.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

double seconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

// A pulse the mount still reports this long after it should have ended is a fault.
constexpr double kPulseOverrunSeconds = 10;
// Minimum gap between two unpulsed Watching estimates to refine the drift rate.
constexpr double kDriftSampleMinSeconds = 2.0;
// Extra time after a pulse settles before LockTracker collects 3 fresh samples.
constexpr double kMeasureOverheadSeconds = 0.6;
// Hard cap on backlash grace/boost so an anomalous calibration cannot disable safety.
constexpr int kMaxBacklashBudgetMs = 15000;

} // namespace

GuideController::GuideController(GuideCalibration calibration, Settings settings)
    : calibration_(calibration)
    , settings_(settings)
{
    if (const auto drift = decompose(calibration_.drift.x, calibration_.drift.y))
    {
        driftRaPxPerSec_ = drift->raPx;
        driftDecPxPerSec_ = drift->decPx;
        driftEstimateSeeded_ = calibration_.drift.length() > 1e-6;
    }
}

std::optional<GuideController::AxisError> GuideController::decompose(double dx, double dy) const
{
    const ImageVector& w = calibration_.west;
    const ImageVector& n = calibration_.north;
    const double determinant = w.x * n.y - w.y * n.x;
    if (std::abs(determinant) < 1e-12) return std::nullopt;
    const double a = (dx * n.y - dy * n.x) / determinant;
    const double b = (w.x * dy - w.y * dx) / determinant;
    return AxisError{a, b, a * w.length(), b * n.length()};
}

void GuideController::updateDriftEstimate(std::chrono::steady_clock::time_point now, double raPx, double decPx)
{
    if (!watchSampleTime_)
    {
        watchSampleTime_ = now;
        watchSampleRaPx_ = raPx;
        watchSampleDecPx_ = decPx;
        return;
    }
    const double dt = seconds(now - *watchSampleTime_);
    if (dt < kDriftSampleMinSeconds) return;
    const double sampleRa = (raPx - watchSampleRaPx_) / dt;
    const double sampleDec = (decPx - watchSampleDecPx_) / dt;
    if (driftEstimateSeeded_)
    {
        driftRaPxPerSec_ = 0.3 * sampleRa + 0.7 * driftRaPxPerSec_;
        driftDecPxPerSec_ = 0.3 * sampleDec + 0.7 * driftDecPxPerSec_;
    }
    else
    {
        driftRaPxPerSec_ = sampleRa;
        driftDecPxPerSec_ = sampleDec;
        driftEstimateSeeded_ = true;
    }
    watchSampleTime_ = now;
    watchSampleRaPx_ = raPx;
    watchSampleDecPx_ = decPx;
}

int GuideController::pulseDurationMs(double errorPx, double driftPxPerSec, double axisSpeedPxPerMs, int backlashMs) const
{
    const double axisSpeedPxPerSec = axisSpeedPxPerMs * 1000.0;
    if (axisSpeedPxPerSec <= 1e-9) return settings_.minPulseMs;
    const double desiredPx = settings_.gain * std::abs(errorPx);
    // When steady drift opposes the pulse (same sign as error), compensate for
    // the drift that accumulates during the pulse and settle/re-measure window,
    // capped so the correction cannot overshoot across the opposite tolerance.
    double opposingDrift = 0.0;
    if (errorPx * driftPxPerSec > 0)
        opposingDrift = std::min(std::abs(driftPxPerSec), 0.75 * axisSpeedPxPerSec);
    const double extraDriftPx = std::min(opposingDrift * (settings_.settleSeconds + kMeasureOverheadSeconds),
        0.8 * settings_.tolerancePixels);
    const double durationSeconds = (desiredPx + extraDriftPx) / (axisSpeedPxPerSec - opposingDrift);
    return std::clamp(static_cast<int>(std::lround(durationSeconds * 1000.0)) + backlashMs,
        settings_.minPulseMs, settings_.maxPulseMs);
}

double GuideController::guideSpeedPixelsPerFrame(const GuideCalibration& calibration, double frameInterval)
{
    if (frameInterval <= 0) return 0;
    return std::max(calibration.west.length(), calibration.north.length()) * 1000.0 * frameInterval;
}

std::string GuideController::refusal(const GuideCalibration& calibration, const Settings& settings, const GuideObservation& o)
{
    if (std::string reason = mountRefusal(o.mount); !reason.empty()) return reason;
    if (o.mount.pierSide != calibration.pierSide)
        return "The mount is on the other side of the pier from the calibration. Calibrate again.";
    if (!o.locked) return "There is no lock. Lock on a feature first.";
    if (settings.tolerancePixels <= 0) return "The tolerance must be positive.";
    if (o.frameInterval <= 0) return "The frame rate is not known yet. Wait for frames.";
    const double speed = guideSpeedPixelsPerFrame(calibration, o.frameInterval);
    if (speed > settings.maxGuideSpeedPixelsPerFrame)
    {
        return std::format("A pulse moves the image {:.2f} pixels per frame, more than {:.2f}. The detector would see "
            "the motion. Set a lower guide rate in the mount's driver and calibrate again.",
            speed, settings.maxGuideSpeedPixelsPerFrame);
    }
    return {};
}

void GuideController::stop(std::string reason)
{
    phase_ = Phase::Stopped;
    stopReason_ = std::move(reason);
    queue_.clear();
}

GuideDecision GuideController::sendNext(const GuideObservation& o)
{
    GuideDecision decision;
    const GuidePulse pulse = queue_.front();
    queue_.erase(queue_.begin());
    recentPulses_.emplace_back(o.now, pulse.milliseconds);
    ++pulses_;
    if (pulse.direction == GuideDirection::North || pulse.direction == GuideDirection::South)
        lastDecDirection_ = pulse.direction;
    pulseEnd_ = o.now + std::chrono::milliseconds{pulse.milliseconds};
    phase_ = Phase::Pulsing;
    decision.pulse = pulse;
    return decision;
}

GuideDecision GuideController::step(const GuideObservation& o)
{
    if (stopped()) return {};
    if (std::string reason = mountRefusal(o.mount); !reason.empty())
    {
        stop("Guiding stopped: " + reason);
        return {};
    }
    if (o.mount.pierSide != calibration_.pierSide)
    {
        calibrationInvalid_ = true;
        stop("Guiding stopped: the mount changed its side of pier, so the calibration no longer holds. Calibrate again.");
        return {};
    }

    switch (phase_)
    {
    case Phase::Pulsing:
        if (o.now < *pulseEnd_ || o.mount.pulseGuiding)
        {
            if (seconds(o.now - *pulseEnd_) > kPulseOverrunSeconds)
                stop(std::format("Guiding stopped: the mount still reports pulse guiding {:.0f} s after the pulse should have ended.",
                    kPulseOverrunSeconds));
            return {};
        }
        pulseEnd_.reset();
        if (!queue_.empty()) return sendNext(o);
        settleUntil_ = o.now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(settings_.settleSeconds));
        phase_ = Phase::Settling;
        [[fallthrough]];
    case Phase::Settling:
    {
        if (o.now < settleUntil_) return {};
        phase_ = Phase::Measuring;
        GuideDecision decision;
        decision.restartEstimate = true;
        return decision;
    }
    case Phase::Measuring:
    {
        if (!o.locked || !o.estimateValid) return {};
        const auto axes = decompose(o.estimateDx, o.estimateDy);
        if (!axes)
        {
            stop("Guiding stopped: the calibration's axes are parallel. Calibrate again.");
            return {};
        }
        raError_ = axes->raPx;
        decError_ = axes->decPx;
        // Judge effectiveness on the axis(es) actually pulsed, so drift on an
        // unpulsed axis does not trigger a false stop.
        const bool raImproved = pulsedRa_ && std::abs(raError_) < raErrorBefore_;
        const bool decImproved = pulsedDec_ && std::abs(decError_) < decErrorBefore_;
        if (decImproved) decBacklashPendingMs_ = 0;
        const bool improved = raImproved || decImproved;
        const bool clearingBacklash = !pulsedRa_ && pulsedDec_ && decBacklashUsedThisStep_ > 0;
        if (improved) fruitless_ = 0;
        else if (!clearingBacklash) ++fruitless_;
        if (fruitless_ >= settings_.maxFruitlessCorrections)
        {
            const double error = std::hypot(o.estimateDx, o.estimateDy);
            stop(std::format("Guiding stopped: corrections have no effect. {} corrections in a row did not reduce the error "
                "(now {:.1f} pixels). Check the calibration, the mount, and that the locked feature is still in view.",
                fruitless_, error));
            return {};
        }
        watchSampleTime_ = o.now;
        watchSampleRaPx_ = raError_;
        watchSampleDecPx_ = decError_;
        phase_ = Phase::Watching;
        return decide(o);
    }
    case Phase::Watching:
        return decide(o);
    case Phase::Stopped:
        break;
    }
    return {};
}

GuideDecision GuideController::decide(const GuideObservation& o)
{
    // Safety rule 4: nothing on an invalid estimate or a lost lock.
    if (!o.locked || !o.estimateValid)
    {
        watchSampleTime_.reset();
        return {};
    }

    const auto axes = decompose(o.estimateDx, o.estimateDy);
    if (!axes)
    {
        stop("Guiding stopped: the calibration's axes are parallel. Calibrate again.");
        return {};
    }
    raError_ = axes->raPx;
    decError_ = axes->decPx;
    updateDriftEstimate(o.now, raError_, decError_);

    const double tolerance = settings_.tolerancePixels;
    queue_.clear();
    bool pulseRa = false, pulseDec = false;
    int pendingBacklashAfter = decBacklashPendingMs_;
    int backlashUsed = 0;
    if (std::abs(raError_) > tolerance)
    {
        pulseRa = true;
        queue_.push_back({axes->raMs > 0 ? GuideDirection::East : GuideDirection::West,
            pulseDurationMs(raError_, driftRaPxPerSec_, calibration_.west.length(), 0)});
    }
    if (std::abs(decError_) > tolerance)
    {
        const GuideDirection direction = axes->decMs > 0 ? GuideDirection::South : GuideDirection::North;
        if (lastDecDirection_ && direction != *lastDecDirection_ && std::abs(decError_) <= 2 * tolerance)
        {
            ++reversalsWithheld_;
        }
        else
        {
            pulseDec = true;
            // Calibration finishes moving South, so a first North pulse or any
            // later Dec reversal must take up measured Dec backlash first.
            const bool firstNorthAfterCal = !lastDecDirection_ && calibration_.northMs > 0
                && direction == GuideDirection::North;
            const bool reversal = lastDecDirection_ && direction != *lastDecDirection_;
            if ((firstNorthAfterCal || reversal) && calibration_.backlashMs > 0)
                pendingBacklashAfter = std::min(calibration_.backlashMs, kMaxBacklashBudgetMs);
            const int ms = pulseDurationMs(decError_, driftDecPxPerSec_, calibration_.north.length(), pendingBacklashAfter);
            backlashUsed = std::min(pendingBacklashAfter, ms);
            pendingBacklashAfter -= backlashUsed;
            queue_.push_back({direction, ms});
        }
    }
    if (queue_.empty()) return {};

    // Safety rule 7: the pulse time of the last minute is capped.
    while (!recentPulses_.empty() && seconds(o.now - recentPulses_.front().first) >= 60.0) recentPulses_.pop_front();
    int total = 0;
    for (const auto& [time, milliseconds] : recentPulses_) total += milliseconds;
    for (const GuidePulse& pulse : queue_) total += pulse.milliseconds;
    if (total > settings_.maxPulseMsPerMinute)
    {
        stop(std::format("Guiding stopped: {:.1f} s of pulses within one minute would pass the cap of {:.0f} s. "
            "The field drifts faster than guiding should correct, or the calibration is wrong.",
            total / 1000.0, settings_.maxPulseMsPerMinute / 1000.0));
        return {};
    }
    pulsedRa_ = pulseRa;
    pulsedDec_ = pulseDec;
    raErrorBefore_ = std::abs(raError_);
    decErrorBefore_ = std::abs(decError_);
    decBacklashPendingMs_ = pendingBacklashAfter;
    decBacklashUsedThisStep_ = backlashUsed;
    watchSampleTime_.reset();
    ++corrections_;
    return sendNext(o);
}
