#include "sky/sky_schedule.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
double radians(double degrees) { return degrees * kPi / 180; }
double degrees(double radians) { return radians * 180 / kPi; }

} // namespace

double sunAltitudeDegrees(double latitudeDeg, double longitudeDeg, std::chrono::system_clock::time_point utc)
{
    const double unixSeconds = std::chrono::duration<double>(utc.time_since_epoch()).count();
    const double julianDay = unixSeconds / 86400.0 + 2440587.5;
    const double t = (julianDay - 2451545.0) / 36525.0;

    const double meanLongitude = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double meanAnomaly = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double m = radians(meanAnomaly);
    const double centre = std::sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t))
        + std::sin(2 * m) * (0.019993 - 0.000101 * t) + std::sin(3 * m) * 0.000289;
    const double trueLongitude = meanLongitude + centre;
    const double omega = radians(125.04 - 1934.136 * t);
    const double apparentLongitude = radians(trueLongitude - 0.00569 - 0.00478 * std::sin(omega));
    const double meanObliquity = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double obliquity = radians(meanObliquity + 0.00256 * std::cos(omega));
    const double declination = std::asin(std::sin(obliquity) * std::sin(apparentLongitude));

    // The equation of time, in minutes.
    const double y = std::pow(std::tan(obliquity / 2), 2);
    const double l0 = radians(meanLongitude);
    const double equationOfTime = 4 * degrees(y * std::sin(2 * l0) - 2 * eccentricity * std::sin(m)
        + 4 * eccentricity * y * std::sin(m) * std::cos(2 * l0) - 0.5 * y * y * std::sin(4 * l0)
        - 1.25 * eccentricity * eccentricity * std::sin(2 * m));

    const double minutesOfDay = std::fmod(unixSeconds, 86400.0) / 60.0;
    double trueSolarTime = std::fmod(minutesOfDay + equationOfTime + 4 * longitudeDeg, 1440.0);
    if (trueSolarTime < 0) trueSolarTime += 1440.0;
    const double hourAngle = radians(trueSolarTime / 4 - 180);
    const double latitude = radians(latitudeDeg);
    const double cosZenith = std::sin(latitude) * std::sin(declination)
        + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    return 90.0 - degrees(std::acos(std::clamp(cosZenith, -1.0, 1.0)));
}

namespace {

// The first passage in the direction asked for, found in five-minute steps
// and halved down to a second.
template <typename Altitude>
std::optional<std::chrono::system_clock::time_point> nextCrossing(Altitude altitude, double altitudeDeg,
    std::chrono::system_clock::time_point from, Crossing direction)
{
    using namespace std::chrono;
    const auto above = [&](system_clock::time_point at) { return altitude(at) >= altitudeDeg; };
    const bool after = direction == Crossing::Rising;
    auto before = from;
    bool wasAbove = above(from);
    for (int step = 1; step <= 2 * 24 * 12; ++step)
    {
        const auto at = from + minutes{5 * step};
        const bool isAbove = above(at);
        if (isAbove == wasAbove || isAbove != after)
        {
            before = at;
            wasAbove = isAbove;
            continue;
        }
        auto end = at;
        while (end - before > seconds{1})
        {
            const auto middle = before + (end - before) / 2;
            (above(middle) == after ? end : before) = middle;
        }
        return time_point_cast<seconds>(end);
    }
    return std::nullopt;
}

} // namespace

double moonAltitudeDegrees(double latitudeDeg, double longitudeDeg, std::chrono::system_clock::time_point utc)
{
    const double unixSeconds = std::chrono::duration<double>(utc.time_since_epoch()).count();
    const double days = unixSeconds / 86400.0 + 2440587.5 - 2451545.0;
    const double t = days / 36525.0;
    const auto s = [](double angle) { return std::sin(radians(angle)); };
    const auto c = [](double angle) { return std::cos(radians(angle)); };

    // Ecliptic longitude and latitude and the horizontal parallax, degrees.
    const double longitude = 218.32 + 481267.881 * t + 6.29 * s(135.0 + 477198.87 * t)
        - 1.27 * s(259.3 - 413335.36 * t) + 0.66 * s(235.7 + 890534.22 * t) + 0.21 * s(269.9 + 954397.74 * t)
        - 0.19 * s(357.5 + 35999.05 * t) - 0.11 * s(186.5 + 966404.03 * t);
    const double latitude = 5.13 * s(93.3 + 483202.02 * t) + 0.28 * s(228.2 + 960400.89 * t)
        - 0.28 * s(318.3 + 6003.15 * t) - 0.17 * s(217.6 - 407332.21 * t);
    const double parallax = 0.9508 + 0.0518 * c(135.0 + 477198.87 * t) + 0.0095 * c(259.3 - 413335.36 * t)
        + 0.0078 * c(235.7 + 890534.22 * t) + 0.0028 * c(269.9 + 954397.74 * t);

    // To right ascension and declination.
    const double obliquity = radians(23.439291 - 0.0130042 * t);
    const double l = radians(longitude), b = radians(latitude);
    const double x = std::cos(b) * std::cos(l);
    const double y = std::cos(obliquity) * std::cos(b) * std::sin(l) - std::sin(obliquity) * std::sin(b);
    const double z = std::sin(obliquity) * std::cos(b) * std::sin(l) + std::cos(obliquity) * std::sin(b);
    const double rightAscension = std::atan2(y, x);
    const double declination = std::asin(std::clamp(z, -1.0, 1.0));

    // The hour angle from Greenwich mean sidereal time.
    const double siderealDeg = 280.46061837 + 360.98564736629 * days + longitudeDeg;
    const double hourAngle = radians(std::fmod(siderealDeg, 360.0)) - rightAscension;
    const double site = radians(latitudeDeg);
    const double sinAltitude = std::sin(site) * std::sin(declination)
        + std::cos(site) * std::cos(declination) * std::cos(hourAngle);
    const double geocentric = std::asin(std::clamp(sinAltitude, -1.0, 1.0));
    // Seen from the surface the Moon stands lower by up to its parallax, about a degree.
    return degrees(geocentric - std::asin(std::sin(radians(parallax)) * std::cos(geocentric)));
}

std::optional<std::chrono::system_clock::time_point> nextSunCrossing(double latitudeDeg, double longitudeDeg,
    double altitudeDeg, std::chrono::system_clock::time_point from, Crossing direction)
{
    return nextCrossing([&](auto at) { return sunAltitudeDegrees(latitudeDeg, longitudeDeg, at); }, altitudeDeg, from,
        direction);
}

std::optional<std::chrono::system_clock::time_point> nextMoonCrossing(double latitudeDeg, double longitudeDeg,
    double altitudeDeg, std::chrono::system_clock::time_point from, Crossing direction)
{
    return nextCrossing([&](auto at) { return moonAltitudeDegrees(latitudeDeg, longitudeDeg, at); }, altitudeDeg, from,
        direction);
}
