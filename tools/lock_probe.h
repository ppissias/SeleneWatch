#pragma once

#include "guiding/feature_lock.h"

#include <QtCore/QStringList>

// The 64 x 64 box of a 32-pixel grid with the most two-dimensional detail.
// Throws when no box can be locked.
[[nodiscard]] LockRegion mostLockable(const Frame& frame);

// Feature-8 Part A on a real recording: lock on a region of the first frame
// and follow it through the file at the guider's measuring rate. Writes one
// CSV row per search and prints a summary. Explicit paths only.
//   SeleneWatchProbe lock INPUT OUTPUT.csv [X Y WIDTH HEIGHT]
// Without a region, the most lockable 64 x 64 box of a grid is chosen.
int runLockProbe(const QStringList& arguments);
