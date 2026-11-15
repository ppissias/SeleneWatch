#pragma once

#include "guiding/mount.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

// Where a simulated telescope points relative to the Moon, and how that
// appears on the synthetic camera.
//
// The offset is kept in arcseconds along the mount's axes. A steady drift
// moves it; guide pulses from SimulatedMount move it at the guide rate, and
// a declination reversal first takes up the backlash. The synthetic source
// asks for the image offset of each frame: the sky offset at the plate
// scale, mirrored if asked, and turned by the camera angle.
//
// Shared by the synthetic source thread and the guide thread; every call
// takes the lock.
class SimulatedPointing
{
public:
    using Clock = std::chrono::steady_clock;

    struct Settings
    {
        // How the Moon drifts against the mount's tracking, arcseconds per second.
        double driftRa{0.10};
        double driftDec{0.25};
        // Guide rate on the sky, arcseconds per second, both axes. A quarter of sidereal.
        double guideRate{3.75};
        // Arcseconds of declination slack taken up after each reversal.
        double decBacklash{0};
        double arcsecPerPixel{1.0};
        double cameraAngleDegrees{30};
        bool mirrored{false};
        // A fault for tests: pulses are accepted but nothing moves.
        bool ignorePulses{false};
    };

    // Where a scene feature appears relative to its place at the start.
    struct Offset
    {
        double x{0}, y{0};
    };

    explicit SimulatedPointing(Settings settings, Clock::time_point start = Clock::now());

    // Inactive, the Moon stays where it is: no drift and no pulses. The
    // simulated mount activates it on connect and freezes it on disconnect,
    // so the synthetic scene moves only while the simulated mount is in use.
    // The offset starts at zero and is kept across activations.
    void setActive(bool active, Clock::time_point now);
    void pulse(GuideDirection direction, int milliseconds, Clock::time_point now);
    [[nodiscard]] bool pulsing(Clock::time_point now);
    // Frame pixels.
    [[nodiscard]] Offset imageOffset(Clock::time_point now);
    // Arcseconds along RA and Dec.
    [[nodiscard]] Offset skyOffset(Clock::time_point now);
    [[nodiscard]] Settings settings() const;

    // The image motion of one millisecond of pulse in a direction, frame pixels.
    // What a perfect calibration measures, ignoring backlash.
    [[nodiscard]] Offset imageRate(GuideDirection direction) const;

private:
    void advance(Clock::time_point now);
    [[nodiscard]] Offset toImage(double ra, double dec) const;

    mutable std::mutex mutex_;
    Settings settings_;
    bool active_{false};
    Clock::time_point time_;
    double ra_{0}, dec_{0};
    std::optional<GuideDirection> direction_;
    Clock::time_point pulseStart_{}, pulseEnd_{};
    // As if the declination gear last moved south, so a first north pulse
    // takes up the slack.
    int lastDecSign_{1};
    double slack_{0};
};

// The mount half of the simulation: a Mount whose pulses move a
// SimulatedPointing. Its reported condition can be set, so tests can check
// each safety rule. The window uses it with the synthetic source.
class SimulatedMount final : public Mount
{
public:
    // What the mount reports besides its pulses. Nominal by default.
    struct Condition
    {
        bool tracking{true};
        bool slewing{false};
        bool parked{false};
        bool canPulseGuide{true};
        bool altAzimuth{false};
        PierSide pierSide{PierSide::West};
    };

    explicit SimulatedMount(std::shared_ptr<SimulatedPointing> pointing,
        std::function<SimulatedPointing::Clock::time_point()> clock = &SimulatedPointing::Clock::now);

    [[nodiscard]] std::string name() const override { return "Simulated mount"; }
    void connect() override;
    void disconnect() override;
    [[nodiscard]] MountState state() override;
    void pulseGuide(GuideDirection direction, int milliseconds) override;

    Condition condition;

private:
    std::shared_ptr<SimulatedPointing> pointing_;
    std::function<SimulatedPointing::Clock::time_point()> clock_;
    bool connected_{false};
};
