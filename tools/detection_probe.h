#pragma once

#include <QtCore/QStringList>

// Explicit-path, manual recording survey and detection calibration modes.
int runDetectionProbe(const QStringList& arguments);

// Compare detector rearming with and without scheduled test flashes on the same stream.
int runRearmingProbe(const QStringList& arguments);
