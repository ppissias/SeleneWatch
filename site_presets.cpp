#include "site_presets.h"

#include <QtCore/QSettings>

#include <algorithm>

namespace {

constexpr char kGroup[] = "site/presets";

// A QSettings group name for a preset: letters, digits, '-' and '_' only.
QString keyFor(const QString& name)
{
    QString key;
    for (const QChar ch : name.trimmed())
        key.append(ch.isLetterOrNumber() || ch == '-' || ch == '_' ? ch : QChar('_'));
    return key.isEmpty() ? QStringLiteral("preset") : key.left(48);
}

} // namespace

std::vector<SitePreset> loadSitePresets()
{
    QSettings saved;
    saved.beginGroup(kGroup);
    std::vector<SitePreset> presets;
    for (const QString& key : saved.childGroups())
    {
        saved.beginGroup(key);
        SitePreset preset;
        preset.name = saved.value("name", key).toString();
        ObservingSite& site = preset.site;
        site.observer = saved.value("observer").toString().toStdString();
        site.site = saved.value("site").toString().toStdString();
        if (saved.contains("latitude") && saved.contains("longitude"))
        {
            site.latitudeDeg = saved.value("latitude").toDouble();
            site.longitudeDeg = saved.value("longitude").toDouble();
            site.heightM = saved.value("height", 0).toDouble();
        }
        site.telescope = saved.value("telescope").toString().toStdString();
        site.apertureMm = saved.value("aperture", 0).toDouble();
        site.focalLengthMm = saved.value("focalLength", 0).toDouble();
        site.pixelSizeUm = saved.value("pixelSize", 0).toDouble();
        presets.push_back(std::move(preset));
        saved.endGroup();
    }
    std::sort(presets.begin(), presets.end(), [](const SitePreset& a, const SitePreset& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return presets;
}

void saveSitePreset(const SitePreset& preset)
{
    QSettings saved;
    saved.beginGroup(kGroup);
    saved.remove(keyFor(preset.name));
    saved.beginGroup(keyFor(preset.name));
    const ObservingSite& site = preset.site;
    saved.setValue("name", preset.name.trimmed());
    saved.setValue("observer", QString::fromStdString(site.observer));
    saved.setValue("site", QString::fromStdString(site.site));
    if (site.hasPosition())
    {
        saved.setValue("latitude", *site.latitudeDeg);
        saved.setValue("longitude", *site.longitudeDeg);
        saved.setValue("height", site.heightM.value_or(0));
    }
    saved.setValue("telescope", QString::fromStdString(site.telescope));
    saved.setValue("aperture", site.apertureMm);
    saved.setValue("focalLength", site.focalLengthMm);
    saved.setValue("pixelSize", site.pixelSizeUm);
    saved.endGroup();
    saved.endGroup();
    saved.sync();
}

void removeSitePreset(const QString& name)
{
    QSettings saved;
    saved.beginGroup(kGroup);
    saved.remove(keyFor(name));
    saved.endGroup();
    saved.sync();
}
