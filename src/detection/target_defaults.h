#pragma once

#include "core/capture_settings.h"
#include "detection/detection_settings.h"

// The starting settings of each target (Feature-11), used until the observer
// changes and saves them. Moon's are the program's settings before Sky mode
// existed. Sky's are first values, to be checked on the first nights.

[[nodiscard]] inline DetectionSettings defaultDetectionSettings(Target target)
{
    DetectionSettings settings;
    if (target == Target::Sky)
    {
        // A 45-degree streak 3 px wide and 180 px long is near 30 times as
        // spread out as its pixel count; a bright meteor's glow and train
        // are thousands of pixels.
        settings.maxPixels = 3000;
        settings.maxPatchBoxRatio = 30;
        settings.brighteningShare = 0.02;
        settings.releaseSeconds = 20;
    }
    return settings;
}

// Feature-31: the starting settings of each watch.
// - Flashes on the earthlit side: the Moon's settings, sensitive, with the
//   sunlit side and its glare (above 30 % of full scale) left out.
// - Shadows on the sunlit side: the reverse aim, no false triggers from
//   seeing on bright terrain. K 12 (12-16 in CAMERA_SETTINGS_GUIDE.md), a
//   shadow must cover 20 pixels and last two frames, and darkening is on with
//   room for a plane's whole silhouette. A large darkening (clouds) is off.
[[nodiscard]] inline DetectionSettings defaultDetectionSettings(Watch watch)
{
    DetectionSettings settings = defaultDetectionSettings(watchTarget(watch));
    if (watch == Watch::MoonFlashes) settings.ignoreAboveShare = 0.30;
    if (watch == Watch::MoonShadows)
    {
        settings.sigma = 12;
        settings.minPixels = 20;
        settings.oneFrame = false;
        settings.darkening = true;
        settings.maxDarkPixels = 20000;
        settings.maxDarkBoxRatio = 10;
    }
    if (watch == Watch::SkySlow)
    {
        // Feature-35: frames a second or more apart. The background learns
        // over about five frames of 2 s, and a patch that keeps firing for a
        // minute (a star drifting past a fixed camera) is learned. First
        // values, to be checked on the first nights.
        settings.referenceTimeConstantSeconds = 10;
        settings.releaseSeconds = 60;
    }
    return settings;
}

// Feature-31: the settings as a watch uses them, whatever was saved before:
// darkening is the shadows' watch and only theirs; the ceiling is the
// flashes' only; the large brightening, release and mask are the sky's.
[[nodiscard]] inline DetectionSettings settingsForWatch(DetectionSettings settings, Watch watch)
{
    const bool shadows = watch == Watch::MoonShadows;
    settings.darkening = shadows;
    if (!shadows) settings.darkeningShare = 0;
    if (watch != Watch::MoonFlashes) settings.ignoreAboveShare = 0;
    if (watchTarget(watch) != Target::Sky)
    {
        settings.brighteningShare = 0;
        settings.releaseSeconds = 0;
        settings.mask = {};
    }
    return settings;
}

[[nodiscard]] inline CaptureSettings defaultCaptureSettings(Target target)
{
    CaptureSettings settings;
    settings.target = target;
    settings.diskReserveBytes = uint64_t{10} * 1024 * 1024 * 1024;
    if (target == Target::Sky)
    {
        // Repeated lightning strokes and a fireball's flare stay in one clip.
        // Full frame at 30 fps adds about 37 MB a second: history and 20 s
        // fit the clip's share of the default RAM ceiling.
        settings.historySeconds = 3;
        settings.postSeconds = 5;
        settings.maxPostSeconds = 20;
    }
    return settings;
}

// Feature-35: the slow sky's clips hold a few frames each side at exposures
// of a second or more; the other watches use their target's.
[[nodiscard]] inline CaptureSettings defaultCaptureSettings(Watch watch)
{
    CaptureSettings settings = defaultCaptureSettings(watchTarget(watch));
    if (watch == Watch::SkySlow)
    {
        settings.historySeconds = 20;
        settings.postSeconds = 10;
        settings.maxPostSeconds = 60;
    }
    return settings;
}

// Feature-35: the longest exposure auto-tune uses for each sky watch, in
// milliseconds, until the observer changes it. Fast events: 25 frames a
// second, as meteor networks use; a moving meteor leaves no more light in a
// pixel when exposed longer, only more sky. Slow and faint: 2 s.
[[nodiscard]] constexpr double defaultLongestExposureMs(Watch watch)
{
    return watch == Watch::SkySlow ? 2000 : 40;
}
