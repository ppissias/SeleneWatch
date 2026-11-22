#pragma once

#include "core/capture_settings.h"
#include "detection/detection_mask.h"
#include "detection/detection_result.h"

#include <QtCore/QString>

#include <vector>

// A named detection profile stored in QSettings (under detection/profiles/<watch>/;
// Feature-31: the Moon's flashes keep "moon", its shadows have "moonShadows").
// In Sky mode, the horizon mask is optional and excluded by default unless the
// observer explicitly opts in when saving the profile.
struct SavedDetectionProfile
{
    QString name;
    Watch watch{Watch::MoonFlashes};
    DetectionSettings settings;
    bool hasMask{false};
    DetectionMask mask;
};

[[nodiscard]] std::vector<SavedDetectionProfile> loadSavedDetectionProfiles(Watch watch);
void saveNamedDetectionProfile(const SavedDetectionProfile& profile);
void removeNamedDetectionProfile(Watch watch, const QString& name);
