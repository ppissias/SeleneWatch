#include "detection_profiles.h"
#include "target_defaults.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>

#include <algorithm>

namespace {

// Feature-31: the flashes keep the Moon's group, so profiles saved before the
// Moon had two watches are the flashes' profiles.
QString watchKey(Watch watch)
{
    switch (watch)
    {
    case Watch::MoonFlashes: return QStringLiteral("moon");
    case Watch::MoonShadows: return QStringLiteral("moonShadows");
    case Watch::Sky: return QStringLiteral("sky");
    case Watch::SkySlow: return QStringLiteral("skySlow");
    }
    return QStringLiteral("moon");
}

QString sanitizeKey(const QString& name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name.trimmed())
    {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-')
            out.append(ch);
        else if (ch.isSpace())
            out.append('_');
    }
    while (out.contains("__")) out.replace("__", "_");
    if (out.isEmpty()) out = QStringLiteral("profile");
    return out.left(48);
}

QJsonObject settingsToJson(const DetectionSettings& s, Watch watch)
{
    QJsonObject obj;
    obj["sigma"] = s.sigma;
    obj["minPixels"] = s.minPixels;
    obj["maxPixels"] = s.maxPixels;
    obj["oneFrame"] = s.oneFrame;
    obj["maxPatchBoxRatio"] = s.maxPatchBoxRatio;
    obj["referenceTimeConstantSeconds"] = s.referenceTimeConstantSeconds;
    obj["minNoiseAdu"] = s.minNoiseAdu;
    obj["darkening"] = s.darkening;
    obj["maxDarkPixels"] = s.maxDarkPixels;
    obj["maxDarkBoxRatio"] = s.maxDarkBoxRatio;
    obj["darkeningShare"] = s.darkeningShare;
    obj["ignoreAboveShare"] = s.ignoreAboveShare;
    if (watchTarget(watch) == Target::Sky)
    {
        obj["brighteningShare"] = s.brighteningShare;
        obj["releaseSeconds"] = s.releaseSeconds;
    }
    return obj;
}

DetectionSettings settingsFromJson(const QJsonObject& obj, Watch watch)
{
    DetectionSettings s = defaultDetectionSettings(watch);
    s.sigma = obj["sigma"].toDouble(s.sigma);
    s.minPixels = obj["minPixels"].toInt(s.minPixels);
    s.maxPixels = obj["maxPixels"].toInt(s.maxPixels);
    s.oneFrame = obj["oneFrame"].toBool(s.oneFrame);
    s.maxPatchBoxRatio = obj["maxPatchBoxRatio"].toDouble(s.maxPatchBoxRatio);
    s.referenceTimeConstantSeconds = obj["referenceTimeConstantSeconds"].toDouble(s.referenceTimeConstantSeconds);
    s.minNoiseAdu = obj["minNoiseAdu"].toDouble(s.minNoiseAdu);
    s.maxDarkPixels = obj["maxDarkPixels"].toInt(s.maxDarkPixels);
    s.maxDarkBoxRatio = obj["maxDarkBoxRatio"].toDouble(s.maxDarkBoxRatio);
    s.darkeningShare = obj["darkeningShare"].toDouble(s.darkeningShare);
    s.ignoreAboveShare = obj["ignoreAboveShare"].toDouble(s.ignoreAboveShare);
    if (watchTarget(watch) == Target::Sky)
    {
        s.brighteningShare = obj["brighteningShare"].toDouble(s.brighteningShare);
        s.releaseSeconds = obj["releaseSeconds"].toDouble(s.releaseSeconds);
    }
    s = settingsForWatch(s, watch);
    return s.valid() ? s : defaultDetectionSettings(watch);
}

} // namespace

std::vector<SavedDetectionProfile> loadSavedDetectionProfiles(Watch watch)
{
    std::vector<SavedDetectionProfile> list;
    QSettings saved;
    saved.beginGroup(QStringLiteral("detection/profiles/") + watchKey(watch));
    for (const QString& key : saved.childGroups())
    {
        saved.beginGroup(key);
        const QJsonDocument doc = QJsonDocument::fromJson(saved.value("json").toByteArray());
        if (doc.isObject())
        {
            const QJsonObject obj = doc.object();
            SavedDetectionProfile profile;
            profile.name = obj["name"].toString(key).trimmed();
            if (!profile.name.isEmpty())
            {
                profile.watch = watch;
                profile.settings = settingsFromJson(obj["settings"].toObject(), watch);
                profile.hasMask = (watchTarget(watch) == Target::Sky) && obj["hasMask"].toBool(false);
                if (profile.hasMask)
                {
                    profile.mask = DetectionMask::fromText(obj["mask"].toString().toStdString());
                    profile.settings.mask = profile.mask;
                }
                else
                {
                    profile.mask = {};
                    profile.settings.mask = {};
                }
                list.push_back(std::move(profile));
            }
        }
        saved.endGroup();
    }
    saved.endGroup();
    std::sort(list.begin(), list.end(), [](const SavedDetectionProfile& a, const SavedDetectionProfile& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return list;
}

void saveNamedDetectionProfile(const SavedDetectionProfile& profile)
{
    const QString trimmed = profile.name.trimmed();
    if (trimmed.isEmpty() || !profile.settings.valid()) return;

    QJsonObject root;
    root["version"] = 1;
    root["name"] = trimmed;
    root["target"] = watchKey(profile.watch);
    root["settings"] = settingsToJson(profile.settings, profile.watch);
    const bool storeMask = (watchTarget(profile.watch) == Target::Sky) && profile.hasMask;
    root["hasMask"] = storeMask;
    if (storeMask)
        root["mask"] = QString::fromStdString(profile.mask.text());

    QSettings saved;
    saved.beginGroup(QStringLiteral("detection/profiles/") + watchKey(profile.watch));
    saved.beginGroup(sanitizeKey(trimmed));
    saved.setValue("json", QJsonDocument(root).toJson(QJsonDocument::Compact));
    saved.endGroup();
    saved.endGroup();
    saved.sync();
}

void removeNamedDetectionProfile(Watch watch, const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return;
    QSettings saved;
    saved.beginGroup(QStringLiteral("detection/profiles/") + watchKey(watch));
    saved.remove(sanitizeKey(trimmed));
    saved.endGroup();
    saved.sync();
}
