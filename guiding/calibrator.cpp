#include "guiding/calibrator.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace {

double seconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

} // namespace

std::string mountRefusal(const MountState& state)
{
    if (!state.connected) return "The mount is not connected.";
    if (!state.canPulseGuide) return "The mount's driver cannot pulse-guide.";
    if (state.altAzimuth) return "The mount is alt-azimuth. Only equatorial mounts are guided in this version.";
    if (state.parked) return "The mount is parked.";
    if (state.slewing) return "The mount is slewing.";
    if (!state.tracking) return "The mount is not tracking. Turn tracking on in the mount's driver.";
    return {};
}

void Calibrator::fail(std::string reason)
{
    phase_ = Phase::Failed;
    failure_ = "Calibration failed: " + std::move(reason);
}

std::string Calibrator::progress() const
{
    switch (phase_)
    {
    case Phase::WestStart:
        return "measuring the start position";
    case Phase::West:
        return std::format("pulsing west ({:.1f} s)", legMs_ / 1000.0);
    case Phase::WestEnd:
        return "measuring after the west pulses";
    case Phase::East:
        return std::format("returning east ({:.1f} of {:.1f} s)", eastMs_ / 1000.0, westMs_ / 1000.0);
    case Phase::EastEnd:
        return "measuring after the east pulses";
    case Phase::BacklashStart:
    case Phase::Backlash:
        return std::format("taking up declination backlash ({:.1f} s)", backlashMs_ / 1000.0);
    case Phase::NorthStart:
        return "measuring before the north pulses";
    case Phase::North:
        return std::format("pulsing north ({:.1f} s)", legMs_ / 1000.0);
    case Phase::NorthEnd:
        return "measuring after the north pulses";
    case Phase::South:
        return std::format("returning south ({:.1f} of {:.1f} s)", southMs_ / 1000.0, (northMs_ + backlashMs_) / 1000.0);
    case Phase::Done:
        return "done";
    case Phase::Failed:
        return failure_;
    }
    return {};
}

bool Calibrator::pulseRunning(const GuideObservation& o)
{
    if (!pulseEnd_) return false;
    if (o.now < *pulseEnd_ || o.mount.pulseGuiding)
    {
        if (seconds(o.now - *pulseEnd_) > kPulseOverrunSeconds)
            fail(std::format("the mount still reports pulse guiding {:.0f} s after the pulse should have ended.", kPulseOverrunSeconds));
        return true;
    }
    pulseEnd_.reset();
    settleUntil_ = o.now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kSettleSeconds));
    return false;
}

std::optional<Calibrator::Point> Calibrator::fresh(const GuideObservation& o, GuideDecision& decision)
{
    if (!awaitingFresh_)
    {
        if (o.now < settleUntil_) return std::nullopt;
        decision.restartEstimate = true;
        awaitingFresh_ = true;
        return std::nullopt;
    }
    if (!o.estimateValid || o.estimateSamples < kFreshSamples) return std::nullopt;
    awaitingFresh_ = false;
    return Point{{o.estimateDx, o.estimateDy}, o.now};
}

GuideDecision Calibrator::send(GuideDirection direction, int milliseconds, const GuideObservation& o)
{
    GuideDecision decision;
    decision.pulse = GuidePulse{direction, milliseconds};
    pulseEnd_ = o.now + std::chrono::milliseconds{milliseconds};
    return decision;
}

ImageVector Calibrator::driftSince(const Point& from, std::chrono::steady_clock::time_point now) const
{
    return drift_ * seconds(now - from.time);
}

GuideDecision Calibrator::step(const GuideObservation& o)
{
    GuideDecision decision;
    if (finished()) return decision;
    if (const std::string refusal = mountRefusal(o.mount); !refusal.empty())
    {
        fail(refusal);
        return decision;
    }
    if (!pierSide_) pierSide_ = o.mount.pierSide;
    if (o.mount.pierSide != *pierSide_)
    {
        fail("the mount changed its side of pier.");
        return decision;
    }
    if (!o.locked)
    {
        fail("the lock was lost. Lock on a feature with clear detail and calibrate again.");
        return decision;
    }
    if (pulseRunning(o) || finished()) return decision;

    const ImageVector found{o.foundDx, o.foundDy};
    switch (phase_)
    {
    case Phase::WestStart:
        if (auto point = fresh(o, decision))
        {
            westStart_ = *point;
            legMs_ = 0;
            phase_ = Phase::West;
        }
        return decision;

    case Phase::West:
        if ((found - westStart_.position).length() >= kTargetPixels || legMs_ >= kMaxLegMs)
        {
            westMs_ = legMs_;
            phase_ = Phase::WestEnd;
            return decision;
        }
        legMs_ += kStepMs;
        return send(GuideDirection::West, kStepMs, o);

    case Phase::WestEnd:
        if (auto point = fresh(o, decision))
        {
            westEnd_ = *point;
            const double moved = (westEnd_.position - westStart_.position).length();
            if (moved < kMinMovePixels)
            {
                fail(std::format("{:.1f} s of west pulses moved the feature {:.1f} pixels, less than {:.0f}. "
                    "The mount does not respond to pulses, or its guide rate is far too low.",
                    westMs_ / 1000.0, moved, kMinMovePixels));
                return decision;
            }
            eastMs_ = 0;
            phase_ = Phase::East;
        }
        return decision;

    case Phase::East:
        if (eastMs_ >= westMs_)
        {
            phase_ = Phase::EastEnd;
            return decision;
        }
        {
            const int milliseconds = std::min(kStepMs, westMs_ - eastMs_);
            eastMs_ += milliseconds;
            return send(GuideDirection::East, milliseconds, o);
        }

    case Phase::EastEnd:
        if (auto point = fresh(o, decision))
        {
            // West: w T + d tw. East: -w T + d te. Their sum is the drift alone.
            const ImageVector westMove = westEnd_.position - westStart_.position;
            const ImageVector eastMove = point->position - westEnd_.position;
            const double westSeconds = seconds(westEnd_.time - westStart_.time);
            const double eastSeconds = seconds(point->time - westEnd_.time);
            drift_ = (westMove + eastMove) / (westSeconds + eastSeconds);
            west_ = (westMove - drift_ * westSeconds) / westMs_;
            backlashMs_ = 0;
            phase_ = Phase::BacklashStart;
        }
        return decision;

    case Phase::BacklashStart:
        if (auto point = fresh(o, decision))
        {
            backlashStart_ = *point;
            phase_ = Phase::Backlash;
        }
        return decision;

    case Phase::Backlash:
        if ((found - backlashStart_.position - driftSince(backlashStart_, o.now)).length() >= kBacklashPixels
            || backlashMs_ >= kMaxBacklashMs)
        {
            phase_ = Phase::NorthStart;
            return decision;
        }
        backlashMs_ += kStepMs;
        return send(GuideDirection::North, kStepMs, o);

    case Phase::NorthStart:
        if (auto point = fresh(o, decision))
        {
            northStart_ = *point;
            legMs_ = 0;
            phase_ = Phase::North;
        }
        return decision;

    case Phase::North:
        if ((found - northStart_.position - driftSince(northStart_, o.now)).length() >= kTargetPixels || legMs_ >= kMaxLegMs)
        {
            northMs_ = legMs_;
            phase_ = Phase::NorthEnd;
            return decision;
        }
        legMs_ += kStepMs;
        return send(GuideDirection::North, kStepMs, o);

    case Phase::NorthEnd:
        if (auto point = fresh(o, decision))
        {
            const ImageVector northMove = point->position - northStart_.position - driftSince(northStart_, point->time);
            if (northMove.length() < kMinMovePixels)
            {
                fail(std::format("{:.1f} s of north pulses moved the feature {:.1f} pixels, less than {:.0f}. "
                    "The mount does not respond to declination pulses, or its backlash exceeds {:.0f} s of pulses.",
                    northMs_ / 1000.0, northMove.length(), kMinMovePixels, kMaxBacklashMs / 1000.0));
                return decision;
            }
            const ImageVector north = northMove / northMs_;
            const double cosine = dot(west_, north) / (west_.length() * north.length());
            const double angle = std::acos(std::clamp(cosine, -1.0, 1.0)) * 180.0 / std::numbers::pi;
            if (std::abs(angle - 90.0) > kMaxSkewDegrees)
            {
                fail(std::format("the RA and declination directions are {:.0f} degrees apart on the image, not near 90. "
                    "The field may have drifted unevenly, or the lock may have slipped; calibrate again.", angle));
                return decision;
            }
            GuideCalibration calibration;
            calibration.west = west_;
            calibration.north = north;
            calibration.drift = drift_;
            calibration.pierSide = *pierSide_;
            calibration.axisAngleDegrees = angle;
            calibration.westMs = westMs_;
            calibration.northMs = northMs_;
            calibration.backlashMs = backlashMs_;
            if (o.mount.guideRateDec && *o.mount.guideRateDec > 0)
                calibration.arcsecPerPixel = *o.mount.guideRateDec * 3600.0 / (north.length() * 1000.0);
            result_ = calibration;
            southMs_ = 0;
            phase_ = Phase::South;
        }
        return decision;

    case Phase::South:
        if (southMs_ >= northMs_ + backlashMs_)
        {
            phase_ = Phase::Done;
            return decision;
        }
        {
            const int milliseconds = std::min(kStepMs, northMs_ + backlashMs_ - southMs_);
            southMs_ += milliseconds;
            return send(GuideDirection::South, milliseconds, o);
        }

    case Phase::Done:
    case Phase::Failed:
        break;
    }
    return decision;
}
