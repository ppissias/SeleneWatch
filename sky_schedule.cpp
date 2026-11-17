#include "sky_schedule.h"

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

std::optional<std::chrono::system_clock::time_point> nextSunCrossing(double latitudeDeg, double longitudeDeg,
    double altitudeDeg, std::chrono::system_clock::time_point from)
{
    using namespace std::chrono;
    const auto above = [&](system_clock::time_point at) {
        return sunAltitudeDegrees(latitudeDeg, longitudeDeg, at) >= altitudeDeg;
    };
    const bool startAbove = above(from);
    auto before = from;
    for (int step = 1; step <= 2 * 24 * 12; ++step)
    {
        const auto at = from + minutes{5 * step};
        if (above(at) == startAbove)
        {
            before = at;
            continue;
        }
        // Halve the five minutes down to a second.
        auto after = at;
        while (after - before > seconds{1})
        {
            const auto middle = before + (after - before) / 2;
            (above(middle) == startAbove ? before : after) = middle;
        }
        return time_point_cast<seconds>(after);
    }
    return std::nullopt;
}

NightAction nightAction(double sunAltitudeDeg, double limitDeg, bool watching, bool startedTonight, bool ownsWhatRuns)
{
    if (sunAltitudeDeg < limitDeg) return !watching && !startedTonight ? NightAction::Start : NightAction::None;
    return ownsWhatRuns ? NightAction::Stop : NightAction::None;
}
