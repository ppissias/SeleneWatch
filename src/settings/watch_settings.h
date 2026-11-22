#pragma once

#include "core/capture_settings.h"
#include "detection/detection_settings.h"

#include <QtCore/QString>
#include <QtCore/QVariantMap>

// Feature-15: a sky watch's exposure follower. The window acts on it; the
// Detection panel shows and edits it.
struct SkyExtras
{
    // Follow the sky's brightness within these limits.
    bool follow{false};
    double minExposureMs{1}, maxExposureMs{2000};
    double maxGain{0}, gainStep{0};
    bool operator==(const SkyExtras&) const = default;
};

// Feature-36: a time-lapse beside the slow sky watch while Monitoring.
struct WatchTimelapse
{
    bool on{false};
    int intervalSeconds{30};
    bool operator==(const WatchTimelapse&) const = default;
};

// Design-1: what each watch keeps between launches, in the program's
// QSettings. This file is the only place that knows the keys. Each watch has
// its own detection settings, exposure follower, auto-tune longest exposure
// and named profiles; both Moon watches share the Moon's capture settings, and
// the slow sky has the time-lapse. The keys are the ones earlier versions
// wrote: the Moon's flashes keep the keys from before there was a choice, so
// an observer's saved values are read unchanged.
//
// Each function opens QSettings itself, so call it from the thread that owns
// the window. Values that are missing or no longer valid read as the watch's
// defaults (detection/target_defaults.h).

// The watch chosen last.
[[nodiscard]] Watch savedWatch();
void saveWatch(Watch watch);

[[nodiscard]] DetectionSettings savedDetectionSettings(Watch watch);
void saveDetectionSettings(Watch watch, const DetectionSettings& settings);

[[nodiscard]] CaptureSettings savedCaptureSettings(Watch watch);
void saveCaptureSettings(Watch watch, const CaptureSettings& capture);
// Both Moon watches keep the same capture settings; the sky watches each their own.
[[nodiscard]] bool sameCaptureSettings(Watch a, Watch b);

// The Moon's watches have no follower of their own and read the fast sky's.
[[nodiscard]] SkyExtras savedSkyExtras(Watch watch);
void saveSkyExtras(Watch watch, const SkyExtras& extras);

// The slow sky's time-lapse.
[[nodiscard]] WatchTimelapse savedWatchTimelapse();
void saveWatchTimelapse(const WatchTimelapse& timelapse);

// Auto-tune's longest exposure for a sky watch; a Moon watch always has its default.
[[nodiscard]] double savedLongestExposureMs(Watch watch);
void saveLongestExposureMs(Watch watch, double milliseconds);

// The QSettings group of a watch's named detection profiles (detection_profiles.h).
[[nodiscard]] QString detectionProfilesGroup(Watch watch);

// The one list of the detection settings' saved fields, by name, used for the
// watch's settings here and for named profiles. The sky's fields are included
// only for a sky watch; the mask is not a field (each caller keeps it its own
// way). Reading starts from the watch's defaults, takes each field present,
// keeps what only that watch uses (settingsForWatch), and falls back to the
// defaults when the result is not valid.
[[nodiscard]] QVariantMap detectionFields(const DetectionSettings& settings, Watch watch);
[[nodiscard]] DetectionSettings detectionFromFields(const QVariantMap& fields, Watch watch);
