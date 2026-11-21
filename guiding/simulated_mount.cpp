#include "guiding/simulated_mount.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

double seconds(SimulatedPointing::Clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

bool isDec(GuideDirection direction)
{
    return direction == GuideDirection::North || direction == GuideDirection::South;
}

} // namespace

SimulatedPointing::SimulatedPointing(Settings settings, Clock::time_point start)
    : settings_(settings)
    , time_(start)
{
}

void SimulatedPointing::setActive(bool active, Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    advance(now);
    active_ = active;
    time_ = std::max(time_, now);
    direction_.reset();
}

void SimulatedPointing::advance(Clock::time_point now)
{
    if (!active_)
    {
        time_ = std::max(time_, now);
        return;
    }
    if (now <= time_) return;
    const double elapsed = seconds(now - time_);
    ra_ += settings_.driftRa * elapsed;
    dec_ += settings_.driftDec * elapsed;
    if (direction_)
    {
        const auto from = std::max(time_, pulseStart_);
        const auto to = std::min(now, pulseEnd_);
        if (to > from && !settings_.ignorePulses)
        {
            double motion = settings_.guideRate * seconds(to - from);
            switch (*direction_)
            {
            case GuideDirection::West:
                ra_ -= motion;
                break;
            case GuideDirection::East:
                ra_ += motion;
                break;
            case GuideDirection::North:
            case GuideDirection::South:
            {
                const double taken = std::min(slack_, motion);
                slack_ -= taken;
                motion -= taken;
                dec_ += *direction_ == GuideDirection::North ? -motion : motion;
                break;
            }
            }
        }
        if (now >= pulseEnd_) direction_.reset();
    }
    time_ = now;
}

void SimulatedPointing::pulse(GuideDirection direction, int milliseconds, Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    advance(now);
    if (isDec(direction))
    {
        const int sign = direction == GuideDirection::North ? -1 : 1;
        if (sign != lastDecSign_) slack_ = settings_.decBacklash;
        lastDecSign_ = sign;
    }
    direction_ = direction;
    pulseStart_ = now;
    pulseEnd_ = now + std::chrono::milliseconds{milliseconds};
}

bool SimulatedPointing::pulsing(Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    advance(now);
    return direction_.has_value();
}

SimulatedPointing::Offset SimulatedPointing::toImage(double ra, double dec) const
{
    double x = ra / settings_.arcsecPerPixel;
    const double y = dec / settings_.arcsecPerPixel;
    if (settings_.mirrored) x = -x;
    const double angle = settings_.cameraAngleDegrees * std::numbers::pi / 180.0;
    return {std::cos(angle) * x - std::sin(angle) * y, std::sin(angle) * x + std::cos(angle) * y};
}

SimulatedPointing::Offset SimulatedPointing::imageOffset(Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    advance(now);
    return toImage(ra_, dec_);
}

SimulatedPointing::Offset SimulatedPointing::skyOffset(Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    advance(now);
    return {ra_, dec_};
}

void SimulatedPointing::setTracking(bool tracking)
{
    std::lock_guard lock(mutex_);
    tracking_ = tracking;
}

bool SimulatedPointing::tracking() const
{
    std::lock_guard lock(mutex_);
    return tracking_;
}

SimulatedPointing::Settings SimulatedPointing::settings() const
{
    std::lock_guard lock(mutex_);
    return settings_;
}

SimulatedPointing::Offset SimulatedPointing::imageRate(GuideDirection direction) const
{
    std::lock_guard lock(mutex_);
    const double step = settings_.ignorePulses ? 0.0 : settings_.guideRate / 1000.0;
    switch (direction)
    {
    case GuideDirection::West:
        return toImage(-step, 0);
    case GuideDirection::East:
        return toImage(step, 0);
    case GuideDirection::North:
        return toImage(0, -step);
    case GuideDirection::South:
        return toImage(0, step);
    }
    return {};
}

SimulatedMount::SimulatedMount(std::shared_ptr<SimulatedPointing> pointing,
    std::function<SimulatedPointing::Clock::time_point()> clock)
    : pointing_(std::move(pointing))
    , clock_(std::move(clock))
{
}

void SimulatedMount::connect()
{
    connected_ = true;
    pointing_->setActive(true, clock_());
}

void SimulatedMount::disconnect()
{
    connected_ = false;
    pointing_->setActive(false, clock_());
}

MountState SimulatedMount::state()
{
    MountState state;
    state.connected = connected_;
    if (!connected_) return state;
    state.tracking = condition.tracking && pointing_->tracking();
    state.canSetTracking = condition.canSetTracking;
    state.slewing = condition.slewing;
    state.parked = condition.parked;
    state.canPulseGuide = condition.canPulseGuide;
    state.altAzimuth = condition.altAzimuth;
    state.pierSide = condition.pierSide;
    state.pulseGuiding = pointing_->pulsing(clock_());
    const double rate = pointing_->settings().guideRate / 3600.0;
    state.guideRateRa = rate;
    state.guideRateDec = rate;
    return state;
}

void SimulatedMount::stopTracking()
{
    if (!connected_) throw std::runtime_error("the simulated mount is not connected");
    pointing_->setTracking(false);
}

void SimulatedMount::pulseGuide(GuideDirection direction, int milliseconds)
{
    if (!connected_) throw std::runtime_error("the simulated mount is not connected");
    if (milliseconds <= 0) throw std::invalid_argument("a guide pulse needs a positive duration");
    pointing_->pulse(direction, milliseconds, clock_());
}
