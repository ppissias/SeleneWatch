#pragma once

#include <chrono>
#include <optional>

// Feature-15 Part A: watching the sky from dusk to dawn.

// The Sun's geometric altitude in degrees (no refraction), seen from a place
// (degrees, north and east positive) at a UTC time. The NOAA solar
// calculator's formulas; within a few hundredths of a degree for this century.
[[nodiscard]] double sunAltitudeDegrees(double latitudeDeg, double longitudeDeg,
    std::chrono::system_clock::time_point utc);

// The next time, after from and within the next two days, at which the Sun
// crosses the given altitude, to the second. None when it does not (polar
// day or night).
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> nextSunCrossing(double latitudeDeg,
    double longitudeDeg, double altitudeDeg, std::chrono::system_clock::time_point from);

// What the schedule does now. Below the limit it starts watching once a
// night, if the sky is not being watched; so a stop by the observer holds
// until the next dusk. Above the limit it stops what it started and still
// runs, never what the observer started.
enum class NightAction { None, Start, Stop };
[[nodiscard]] NightAction nightAction(double sunAltitudeDeg, double limitDeg, bool watching, bool startedTonight,
    bool ownsWhatRuns);
