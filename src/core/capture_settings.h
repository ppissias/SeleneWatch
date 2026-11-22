#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// What SeleneWatch watches (Feature-11). Each target keeps its own detection
// and capture settings. Moon is the program as it was before the choice.
enum class Target { Moon, Sky };

[[nodiscard]] constexpr const char* targetName(Target target)
{
    return target == Target::Sky ? "sky" : "moon";
}

// Feature-31: what is watched. On the Moon, flashes on the earthlit side and
// shadows on the sunlit side need opposite settings, so they are watched one
// at a time, each with its own detection settings and profiles. Both are the
// Moon's target: its folder, capture settings, guiding and time-lapse.
//
// Feature-35: the sky is watched two ways. Sky is fast events (meteors,
// lightning, a fireball's flare): short exposures, timing and trajectory.
// SkySlow is slow and faint (aurora, airglow, slow satellites, long glows):
// long exposures, with its own detection, capture and exposure settings and
// a time-lapse beside it (Feature-36). Both are the sky's target and folder.
enum class Watch { MoonFlashes, MoonShadows, Sky, SkySlow };

[[nodiscard]] constexpr Target watchTarget(Watch watch)
{
    return watch == Watch::Sky || watch == Watch::SkySlow ? Target::Sky : Target::Moon;
}

[[nodiscard]] constexpr const char* watchName(Watch watch)
{
    switch (watch)
    {
    case Watch::MoonFlashes: return "moon_flashes";
    case Watch::MoonShadows: return "moon_shadows";
    case Watch::Sky: return "sky";
    case Watch::SkySlow: return "sky_slow";
    }
    return "moon_flashes";
}

// Capture durations use source time, not playback speed or frame counts.
// The byte ceiling includes shared pixels, detector arrays, and previews.
struct CaptureSettings
{
    double historySeconds{5};
    double postSeconds{3};
    double maxPostSeconds{10};
    size_t memoryBytes{size_t{2} * 1024 * 1024 * 1024};
    // In Sky mode a clip that continues a long event starts after the clip
    // before, instead of repeating its history.
    Target target{Target::Moon};
    // A clip is not written when it would leave less than this free on the
    // output drive. Zero has no reserve; the window's default is 10 GiB.
    uint64_t diskReserveBytes{0};
    [[nodiscard]] bool valid() const
    {
        return std::isfinite(historySeconds) && historySeconds >= 0 && historySeconds <= 600
            && std::isfinite(postSeconds) && postSeconds > 0
            && std::isfinite(maxPostSeconds) && maxPostSeconds >= postSeconds && maxPostSeconds <= 3600
            && memoryBytes > 0;
    }
};
