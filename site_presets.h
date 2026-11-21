#pragma once

#include "observing_site.h"

#include <QtCore/QString>

#include <vector>

// Feature-29: named site and instrument presets ("Home, 8-inch", "Club field,
// refractor"). Each keeps the observer, the site and its position, and the
// instrument, in QSettings under site/presets/. The clock check is the
// computer's, not the site's, and is not part of a preset.
struct SitePreset
{
    QString name;
    ObservingSite site;
};

// Sorted by name.
[[nodiscard]] std::vector<SitePreset> loadSitePresets();
// Replaces a preset of the same name.
void saveSitePreset(const SitePreset& preset);
void removeSitePreset(const QString& name);
