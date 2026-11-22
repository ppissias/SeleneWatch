#include "settings/detection_profiles.h"
#include "settings/watch_settings.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>

#include <algorithm>

namespace {

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

} // namespace

std::vector<SavedDetectionProfile> loadSavedDetectionProfiles(Watch watch)
{
    std::vector<SavedDetectionProfile> list;
    QSettings saved;
    saved.beginGroup(detectionProfilesGroup(watch));
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
                profile.settings = detectionFromFields(obj["settings"].toObject().toVariantMap(), watch);
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
    // The group's last part: moon, moonShadows, sky or skySlow.
    root["target"] = detectionProfilesGroup(profile.watch).section('/', -1);
    root["settings"] = QJsonObject::fromVariantMap(detectionFields(profile.settings, profile.watch));
    const bool storeMask = (watchTarget(profile.watch) == Target::Sky) && profile.hasMask;
    root["hasMask"] = storeMask;
    if (storeMask)
        root["mask"] = QString::fromStdString(profile.mask.text());

    QSettings saved;
    saved.beginGroup(detectionProfilesGroup(profile.watch));
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
    saved.beginGroup(detectionProfilesGroup(watch));
    saved.remove(sanitizeKey(trimmed));
    saved.endGroup();
    saved.sync();
}
