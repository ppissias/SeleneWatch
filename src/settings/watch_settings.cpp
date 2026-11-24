#include "settings/watch_settings.h"
#include "detection/target_defaults.h"

#include <QtCore/QSettings>

#include <algorithm>

namespace {

constexpr uint64_t kGiB = uint64_t{1024} * 1024 * 1024;

// The group that holds a watch's own settings. The Moon's flashes use the
// root, as the Moon did before Sky mode and the shadows existed.
QString watchGroup(Watch watch)
{
    switch (watch)
    {
    case Watch::MoonFlashes: return {};
    case Watch::MoonShadows: return QStringLiteral("moonShadows/");
    case Watch::Sky: return QStringLiteral("sky/");
    case Watch::SkySlow: return QStringLiteral("skySlow/");
    }
    return {};
}

// Both Moon watches keep the Moon's capture settings.
Watch captureOwner(Watch watch)
{
    return watch == Watch::MoonShadows ? Watch::MoonFlashes : watch;
}

// Only the sky follows its brightness; a Moon watch reads the fast sky's follower.
Watch followerOwner(Watch watch)
{
    return watchTarget(watch) == Target::Sky ? watch : Watch::Sky;
}

QString detectionGroup(Watch watch) { return watchGroup(watch) + "detection"; }
QString captureKey(Watch watch) { return watchGroup(captureOwner(watch)) + "capture/"; }
QString followKey(Watch watch) { return watchGroup(followerOwner(watch)) + "follow/"; }
QString longestExposureKey(Watch watch) { return watchGroup(watch) + "autotune/longestExposureMs"; }
const QString kTimelapseKey = QStringLiteral("skySlow/timelapse/");

} // namespace

Watch savedWatch()
{
    const QSettings saved;
    if (saved.value("target", 0).toInt() == 1) return saved.value("sky/watch").toString() == "slow" ? Watch::SkySlow : Watch::Sky;
    return saved.value("moon/watch").toString() == "shadows" ? Watch::MoonShadows : Watch::MoonFlashes;
}

void saveWatch(Watch watch)
{
    QSettings saved;
    saved.setValue("target", watchTarget(watch) == Target::Sky ? 1 : 0);
    if (watchTarget(watch) == Target::Moon) saved.setValue("moon/watch", watch == Watch::MoonShadows ? "shadows" : "flashes");
    else saved.setValue("sky/watch", watch == Watch::SkySlow ? "slow" : "fast");
}

QVariantMap detectionFields(const DetectionSettings& settings, Watch watch)
{
    QVariantMap fields{
        {"sigma", settings.sigma},
        {"minPixels", settings.minPixels},
        {"maxPixels", settings.maxPixels},
        {"oneFrame", settings.oneFrame},
        {"maxPatchBoxRatio", settings.maxPatchBoxRatio},
        {"referenceTimeConstantSeconds", settings.referenceTimeConstantSeconds},
        {"minNoiseAdu", settings.minNoiseAdu},
        {"darkening", settings.darkening},
        {"maxDarkPixels", settings.maxDarkPixels},
        {"maxDarkBoxRatio", settings.maxDarkBoxRatio},
        {"darkeningShare", settings.darkeningShare},
        {"ignoreAboveShare", settings.ignoreAboveShare},
    };
    if (watchTarget(watch) != Target::Sky)
        fields.insert("stabilizeDetection", settings.stabilizeDetection);
    if (watchTarget(watch) == Target::Sky)
    {
        fields.insert("brighteningShare", settings.brighteningShare);
        fields.insert("releaseSeconds", settings.releaseSeconds);
    }
    // Feature-49: this oneFrame was saved by a version whose shadows watch
    // starts with one-frame triggers, so it is the observer's own choice.
    if (watch == Watch::MoonShadows) fields.insert("oneFrameSinceFeature49", true);
    return fields;
}

DetectionSettings detectionFromFields(const QVariantMap& fields, Watch watch)
{
    DetectionSettings settings = defaultDetectionSettings(watch);
    const auto real = [&fields](const char* name, double& value) { value = fields.value(name, value).toDouble(); };
    const auto whole = [&fields](const char* name, int& value) { value = fields.value(name, value).toInt(); };
    const auto flag = [&fields](const char* name, bool& value) { value = fields.value(name, value).toBool(); };
    real("sigma", settings.sigma);
    whole("minPixels", settings.minPixels);
    whole("maxPixels", settings.maxPixels);
    flag("oneFrame", settings.oneFrame);
    real("maxPatchBoxRatio", settings.maxPatchBoxRatio);
    real("referenceTimeConstantSeconds", settings.referenceTimeConstantSeconds);
    real("minNoiseAdu", settings.minNoiseAdu);
    flag("darkening", settings.darkening);
    whole("maxDarkPixels", settings.maxDarkPixels);
    real("maxDarkBoxRatio", settings.maxDarkBoxRatio);
    real("darkeningShare", settings.darkeningShare);
    real("ignoreAboveShare", settings.ignoreAboveShare);
    if (watchTarget(watch) != Target::Sky)
        flag("stabilizeDetection", settings.stabilizeDetection);
    if (watchTarget(watch) == Target::Sky)
    {
        real("brighteningShare", settings.brighteningShare);
        real("releaseSeconds", settings.releaseSeconds);
    }
    // Feature-49: shadows settings saved before one frame became the default
    // hold that old default, not a choice: they start with one frame too.
    if (watch == Watch::MoonShadows && !fields.contains("oneFrameSinceFeature49"))
        settings.oneFrame = defaultDetectionSettings(watch).oneFrame;
    // Feature-31: whatever an older version saved, darkening is the shadows'.
    settings = settingsForWatch(settings, watch);
    return settings.valid() ? settings : defaultDetectionSettings(watch);
}

DetectionSettings savedDetectionSettings(Watch watch)
{
    QSettings saved;
    saved.beginGroup(detectionGroup(watch));
    QVariantMap fields;
    for (const QString& name : saved.childKeys()) fields.insert(name, saved.value(name));
    DetectionSettings settings = detectionFromFields(fields, watch);
    if (watchTarget(watch) == Target::Sky)
        settings.mask = DetectionMask::fromText(saved.value("mask").toString().toStdString());
    return settings;
}

void saveDetectionSettings(Watch watch, const DetectionSettings& settings)
{
    QSettings saved;
    saved.beginGroup(detectionGroup(watch));
    const QVariantMap fields = detectionFields(settings, watch);
    for (auto field = fields.cbegin(); field != fields.cend(); ++field) saved.setValue(field.key(), field.value());
    if (watchTarget(watch) == Target::Sky) saved.setValue("mask", QString::fromStdString(settings.mask.text()));
    saved.endGroup();
    saved.sync();
}

bool hasSavedDetectionSettings(Watch watch)
{
    QSettings saved;
    saved.beginGroup(detectionGroup(watch));
    return !saved.childKeys().isEmpty();
}

CaptureSettings savedCaptureSettings(Watch watch)
{
    const QSettings saved;
    const QString key = captureKey(watch);
    CaptureSettings capture = defaultCaptureSettings(watch);
    capture.historySeconds = saved.value(key + "history", capture.historySeconds).toDouble();
    capture.postSeconds = saved.value(key + "post", capture.postSeconds).toDouble();
    capture.maxPostSeconds = saved.value(key + "maxPost", capture.maxPostSeconds).toDouble();
    capture.memoryBytes = static_cast<size_t>(std::clamp(saved.value(key + "memoryMiB", 2048).toInt(), 16, 65536)) * 1024 * 1024;
    capture.diskReserveBytes = static_cast<uint64_t>(std::clamp(
        saved.value(key + "diskReserveGiB", static_cast<qulonglong>(capture.diskReserveBytes / kGiB)).toLongLong(), 0LL, 100000LL)) * kGiB;
    return capture.valid() ? capture : defaultCaptureSettings(watch);
}

void saveCaptureSettings(Watch watch, const CaptureSettings& capture)
{
    QSettings saved;
    const QString key = captureKey(watch);
    saved.setValue(key + "history", capture.historySeconds);
    saved.setValue(key + "post", capture.postSeconds);
    saved.setValue(key + "maxPost", capture.maxPostSeconds);
    saved.setValue(key + "memoryMiB", static_cast<int>(capture.memoryBytes / (1024 * 1024)));
    saved.setValue(key + "diskReserveGiB", static_cast<qulonglong>(capture.diskReserveBytes / kGiB));
    saved.sync();
}

bool sameCaptureSettings(Watch a, Watch b)
{
    return captureOwner(a) == captureOwner(b);
}

// Feature-15, Feature-35: the follower's longest exposure starts at the
// watch's longest for auto-tune.
SkyExtras savedSkyExtras(Watch watch)
{
    const QSettings saved;
    const QString key = followKey(watch);
    SkyExtras extras;
    extras.maxExposureMs = defaultLongestExposureMs(watch);
    extras.follow = saved.value(key + "enabled", extras.follow).toBool();
    extras.minExposureMs = saved.value(key + "minExposureMs", extras.minExposureMs).toDouble();
    extras.maxExposureMs = saved.value(key + "maxExposureMs", extras.maxExposureMs).toDouble();
    extras.maxGain = saved.value(key + "maxGain", extras.maxGain).toDouble();
    extras.gainStep = saved.value(key + "gainStep", extras.gainStep).toDouble();
    return extras;
}

void saveSkyExtras(Watch watch, const SkyExtras& extras)
{
    QSettings saved;
    const QString key = followKey(watch);
    saved.setValue(key + "enabled", extras.follow);
    saved.setValue(key + "minExposureMs", extras.minExposureMs);
    saved.setValue(key + "maxExposureMs", extras.maxExposureMs);
    saved.setValue(key + "maxGain", extras.maxGain);
    saved.setValue(key + "gainStep", extras.gainStep);
}

WatchTimelapse savedWatchTimelapse()
{
    const QSettings saved;
    WatchTimelapse timelapse;
    timelapse.on = saved.value(kTimelapseKey + "enabled", timelapse.on).toBool();
    timelapse.intervalSeconds = std::clamp(saved.value(kTimelapseKey + "intervalSeconds", timelapse.intervalSeconds).toInt(), 1, 3600);
    return timelapse;
}

void saveWatchTimelapse(const WatchTimelapse& timelapse)
{
    QSettings saved;
    saved.setValue(kTimelapseKey + "enabled", timelapse.on);
    saved.setValue(kTimelapseKey + "intervalSeconds", timelapse.intervalSeconds);
}

double savedLongestExposureMs(Watch watch)
{
    if (watchTarget(watch) != Target::Sky) return defaultLongestExposureMs(watch);
    return QSettings().value(longestExposureKey(watch), defaultLongestExposureMs(watch)).toDouble();
}

void saveLongestExposureMs(Watch watch, double milliseconds)
{
    if (watchTarget(watch) == Target::Sky) QSettings().setValue(longestExposureKey(watch), milliseconds);
}

// Feature-31: the flashes keep the Moon's group, so profiles saved before the
// Moon had two watches are the flashes' profiles.
QString detectionProfilesGroup(Watch watch)
{
    switch (watch)
    {
    case Watch::MoonFlashes: return QStringLiteral("detection/profiles/moon");
    case Watch::MoonShadows: return QStringLiteral("detection/profiles/moonShadows");
    case Watch::Sky: return QStringLiteral("detection/profiles/sky");
    case Watch::SkySlow: return QStringLiteral("detection/profiles/skySlow");
    }
    return QStringLiteral("detection/profiles/moon");
}
