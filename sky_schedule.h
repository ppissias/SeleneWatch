#pragma once

#include <chrono>
#include <optional>

// The Sun's and the Moon's altitude, for the end of a session (Feature-32):
// at dawn, or when the Moon sinks below an altitude.

// The Sun's geometric altitude in degrees (no refraction), seen from a place
// (degrees, north and east positive) at a UTC time. The NOAA solar
// calculator's formulas; within a few hundredths of a degree for this century.
[[nodiscard]] double sunAltitudeDegrees(double latitudeDeg, double longitudeDeg,
    std::chrono::system_clock::time_point utc);

// The Moon's topocentric altitude in degrees (its parallax included, no
// refraction). The Astronomical Almanac's low-precision lunar position;
// within a few tenths of a degree, a few hundredths against JPL Horizons.
[[nodiscard]] double moonAltitudeDegrees(double latitudeDeg, double longitudeDeg,
    std::chrono::system_clock::time_point utc);

// Which way a body passes an altitude.
enum class Crossing { Rising, Setting };

// The next time, after from and within the next two days, at which the body
// passes the given altitude in that direction, to the second. None when it
// does not (polar day or night).
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> nextSunCrossing(double latitudeDeg,
    double longitudeDeg, double altitudeDeg, std::chrono::system_clock::time_point from, Crossing direction);
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> nextMoonCrossing(double latitudeDeg,
    double longitudeDeg, double altitudeDeg, std::chrono::system_clock::time_point from, Crossing direction);
