#pragma once

#include "core/observing_site.h"

#include <QtCore/QString>
#include <QtCore/QTime>
#include <QtCore/QTimeZone>

#include <chrono>
#include <optional>

// Feature-32: when a session ends by itself, and what the mount does then.
// A session is Monitoring or a time-lapse. These are plain values and
// functions; the window keeps the session's start and acts on them.

// The conditions that end a session. It ends at the first one met; with
// none ticked it runs until the observer stops it. Times are wall-clock.
struct SessionEndSettings
{
    bool atLocalTime{false};
    QTime localTime{4, 30};
    bool afterDuration{false};
    int durationMinutes{6 * 60};
    // Dawn: the Sun rises above this altitude. Needs the site's position.
    bool atDawn{false};
    double sunLimitDeg{-10};
    // The Moon and a Moon time-lapse only. Needs the site's position.
    bool moonBelow{false};
    double moonLimitDeg{10};

    [[nodiscard]] bool any(bool moon) const { return atLocalTime || afterDuration || atDawn || (moon && moonBelow); }
    bool operator==(const SessionEndSettings&) const = default;
};

// What the mount does when a session ends by itself (a condition met, a
// failure, lost guiding). Unset until the observer chooses; the observer's
// own Stop leaves the mount as it is.
enum class MountAtEnd { Unset, KeepTracking, StopTracking };

enum class EndCause { LocalTime, Duration, Dawn, MoonBelow };

struct PlannedEnd
{
    std::chrono::system_clock::time_point time;
    EndCause cause;
};

// When one condition is met, for a session that started at start: the next
// time the local clock shows the time, the duration after the start, the
// first sunrise above the limit, the first time the Moon sinks below its
// limit. Empty when the condition is off, needs a position the site does
// not have, or is not met within two days.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> endTime(const SessionEndSettings& settings,
    EndCause cause, std::chrono::system_clock::time_point start, const ObservingSite& site, bool moon,
    const QTimeZone& zone);

// The first of the conditions met.
[[nodiscard]] std::optional<PlannedEnd> plannedEnd(const SessionEndSettings& settings,
    std::chrono::system_clock::time_point start, const ObservingSite& site, bool moon, const QTimeZone& zone);

// "the local time", "6 h 00 min after the start", "dawn (the Sun above
// -10°)", "the Moon below 10°".
[[nodiscard]] QString endCauseText(const SessionEndSettings& settings, EndCause cause);
// "04:30 local (03:30 UTC)".
[[nodiscard]] QString localAndUtc(std::chrono::system_clock::time_point time, const QTimeZone& zone);
// "3 h 12 min", "45 s".
[[nodiscard]] QString durationText(std::chrono::seconds duration);
// For the logs: "keep_tracking", "stop_tracking", "not_chosen".
[[nodiscard]] const char* mountAtEndName(MountAtEnd choice);
