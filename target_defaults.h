#pragma once

#include "capture_settings.h"
#include "detection_settings.h"

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
