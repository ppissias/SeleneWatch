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

} // namespace

GuideController::GuideController(GuideCalibration calibration, Settings settings)
    : calibration_(calibration)
    , settings_(settings)
{
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
        const double error = std::hypot(o.estimateDx, o.estimateDy);
        fruitless_ = error >= errorBefore_ ? fruitless_ + 1 : 0;
        if (fruitless_ >= settings_.maxFruitlessCorrections)
        {
            stop(std::format("Guiding stopped: corrections have no effect. {} corrections in a row did not reduce the error "
                "(now {:.1f} pixels). Check the calibration, the mount, and that the locked feature is still in view.",
                fruitless_, error));
            return {};
        }
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
    if (!o.locked || !o.estimateValid) return {};

    // Split the estimate e = a w + b n: a in milliseconds of west pulse,
    // b in milliseconds of north pulse.
    const ImageVector& w = calibration_.west;
    const ImageVector& n = calibration_.north;
    const double determinant = w.x * n.y - w.y * n.x;
    if (std::abs(determinant) < 1e-12)
    {
        stop("Guiding stopped: the calibration's axes are parallel. Calibrate again.");
        return {};
    }
    const double a = (o.estimateDx * n.y - o.estimateDy * n.x) / determinant;
    const double b = (w.x * o.estimateDy - w.y * o.estimateDx) / determinant;
    raError_ = a * w.length();
    decError_ = b * n.length();

    const double tolerance = settings_.tolerancePixels;
    auto duration = [&](double milliseconds) {
        return std::clamp(static_cast<int>(std::lround(settings_.gain * std::abs(milliseconds))),
            settings_.minPulseMs, settings_.maxPulseMs);
    };
    queue_.clear();
    if (std::abs(raError_) > tolerance)
        queue_.push_back({a > 0 ? GuideDirection::East : GuideDirection::West, duration(a)});
    if (std::abs(decError_) > tolerance)
    {
        const GuideDirection direction = b > 0 ? GuideDirection::South : GuideDirection::North;
        if (lastDecDirection_ && direction != *lastDecDirection_ && std::abs(decError_) <= 2 * tolerance)
            ++reversalsWithheld_;
        else
            queue_.push_back({direction, duration(b)});
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
    errorBefore_ = std::hypot(o.estimateDx, o.estimateDy);
    ++corrections_;
    return sendNext(o);
}
