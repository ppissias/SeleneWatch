#pragma once

#include "capture_settings.h"
#include "detection_mask.h"
#include "detection_result.h"

#include <QtCore/QString>

#include <vector>

// A named detection profile stored in QSettings (under detection/profiles/<target>/).
// In Sky mode, the horizon mask is optional and excluded by default unless the
// observer explicitly opts in when saving the profile.
struct SavedDetectionProfile
{
    QString name;
    Target target{Target::Moon};
    DetectionSettings settings;
    bool hasMask{false};
    DetectionMask mask;
};

[[nodiscard]] std::vector<SavedDetectionProfile> loadSavedDetectionProfiles(Target target);
void saveNamedDetectionProfile(const SavedDetectionProfile& profile);
void removeNamedDetectionProfile(Target target, const QString& name);
