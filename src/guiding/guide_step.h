#pragma once

#include "mounts/mount.h"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>

// A displacement or a rate in frame pixels.
struct ImageVector
{
    double x{0}, y{0};

    [[nodiscard]] double length() const { return std::hypot(x, y); }
    friend ImageVector operator+(ImageVector a, ImageVector b) { return {a.x + b.x, a.y + b.y}; }
    friend ImageVector operator-(ImageVector a, ImageVector b) { return {a.x - b.x, a.y - b.y}; }
    friend ImageVector operator*(ImageVector a, double s) { return {a.x * s, a.y * s}; }
    friend ImageVector operator/(ImageVector a, double s) { return {a.x / s, a.y / s}; }
    friend double dot(ImageVector a, ImageVector b) { return a.x * b.x + a.y * b.y; }
};

// What the calibration measured: how the image moves for each mount axis.
// It covers camera angle, mirror flips, plate scale, and guide rate in one
// measurement, and holds only for the side of pier it was made on.
struct GuideCalibration
{
    // Image motion per millisecond of a west and of a north pulse.
    ImageVector west, north;
    // The field's own drift during the calibration, pixels per second.
    ImageVector drift;
    PierSide pierSide{PierSide::Unknown};
    // The angle between the two axes as seen on the image; near 90.
    double axisAngleDegrees{90};
    // From the mount's reported declination guide rate, when it has one.
    std::optional<double> arcsecPerPixel;
    // Pulse time each measured leg took.
    int westMs{0}, northMs{0}, backlashMs{0};
};

// One pulse to send now.
struct GuidePulse
{
    GuideDirection direction{GuideDirection::North};
    int milliseconds{0};
};

// What the calibration and the guiding decision see at one step. Built by
// the guider from the lock and the mount, or by a test from a simulation.
struct GuideObservation
{
    // Wall-clock time: pulses and settling happen in real time.
    std::chrono::steady_clock::time_point now{};
    // The lock holds (not lost, not off).
    bool locked{false};
    // Where the feature was last found, relative to its lock position.
    double foundDx{0}, foundDy{0};
    // The median estimate, built only from matches since the last restart.
    bool estimateValid{false};
    int estimateSamples{0};
    double estimateDx{0}, estimateDy{0};
    // Live drift speed (image px/s), fitted over recent unpulsed matches.
    bool driftRateValid{false};
    double driftRateDx{0}, driftRateDy{0};
    double driftRateSpanSeconds{0};
    MountState mount;
    // Seconds between frames; 0 when not yet known.
    double frameInterval{0};
};

// What a step asks the guider to do.
struct GuideDecision
{
    std::optional<GuidePulse> pulse;
    // Discard the estimate: the image has moved, so only later matches count.
    bool restartEstimate{false};
};

// Why the mount must not be pulsed now, or empty when it may (safety rule 3,
// and the alt-azimuth refusal of rule 9).
[[nodiscard]] std::string mountRefusal(const MountState& state);
