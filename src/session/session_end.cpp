#include "session/session_end.h"

#include "sky/sky_schedule.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <array>

namespace {

QDateTime toDateTime(std::chrono::system_clock::time_point time)
{
    return QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count(), QTimeZone::UTC);
}

std::chrono::system_clock::time_point fromDateTime(const QDateTime& time)
{
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{time.toMSecsSinceEpoch()}};
}

} // namespace

std::optional<std::chrono::system_clock::time_point> endTime(const SessionEndSettings& settings, EndCause cause,
    std::chrono::system_clock::time_point start, const ObservingSite& site, bool moon, const QTimeZone& zone)
{
    switch (cause)
    {
    case EndCause::LocalTime:
    {
        if (!settings.atLocalTime || !settings.localTime.isValid()) return std::nullopt;
        const QDateTime begin = toDateTime(start).toTimeZone(zone);
        QDateTime end(begin.date(), settings.localTime, zone);
        if (end <= begin) end = QDateTime(begin.date().addDays(1), settings.localTime, zone);
        return fromDateTime(end);
    }
    case EndCause::Duration:
        if (!settings.afterDuration || settings.durationMinutes <= 0) return std::nullopt;
        return start + std::chrono::minutes{settings.durationMinutes};
    case EndCause::Dawn:
        if (!settings.atDawn || !site.hasPosition()) return std::nullopt;
        return nextSunCrossing(*site.latitudeDeg, *site.longitudeDeg, settings.sunLimitDeg, start, Crossing::Rising);
    case EndCause::MoonBelow:
        if (!moon || !settings.moonBelow || !site.hasPosition()) return std::nullopt;
        return nextMoonCrossing(*site.latitudeDeg, *site.longitudeDeg, settings.moonLimitDeg, start, Crossing::Setting);
    }
    return std::nullopt;
}

std::optional<PlannedEnd> plannedEnd(const SessionEndSettings& settings, std::chrono::system_clock::time_point start,
    const ObservingSite& site, bool moon, const QTimeZone& zone)
{
    std::optional<PlannedEnd> first;
    for (const EndCause cause : std::array{EndCause::LocalTime, EndCause::Duration, EndCause::Dawn, EndCause::MoonBelow})
    {
        const auto time = endTime(settings, cause, start, site, moon, zone);
        if (time && (!first || *time < first->time)) first = PlannedEnd{*time, cause};
    }
    return first;
}

QString endCauseText(const SessionEndSettings& settings, EndCause cause)
{
    switch (cause)
    {
    case EndCause::LocalTime:
        return "the local time";
    case EndCause::Duration:
        return QString("%1 after the start").arg(durationText(std::chrono::minutes{settings.durationMinutes}));
    case EndCause::Dawn:
        return QString("dawn (the Sun above %1°)").arg(settings.sunLimitDeg, 0, 'f', 0);
    case EndCause::MoonBelow:
        return QString("the Moon below %1°").arg(settings.moonLimitDeg, 0, 'f', 0);
    }
    return {};
}

QString localAndUtc(std::chrono::system_clock::time_point time, const QTimeZone& zone)
{
    const QDateTime utc = toDateTime(time);
    return QString("%1 local (%2 UTC)").arg(utc.toTimeZone(zone).toString("HH:mm"), utc.toString("HH:mm"));
}

QString durationText(std::chrono::seconds duration)
{
    const auto total = std::max<int64_t>(0, duration.count());
    if (total < 60) return QString("%1 s").arg(total);
    const int64_t minutes = total / 60;
    if (minutes < 60) return QString("%1 min").arg(minutes);
    return QString("%1 h %2 min").arg(minutes / 60).arg(minutes % 60, 2, 10, QChar('0'));
}

const char* mountAtEndName(MountAtEnd choice)
{
    switch (choice)
    {
    case MountAtEnd::KeepTracking:
        return "keep_tracking";
    case MountAtEnd::StopTracking:
        return "stop_tracking";
    case MountAtEnd::Unset:
        break;
    }
    return "not_chosen";
}
