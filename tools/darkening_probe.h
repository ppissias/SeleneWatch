#pragma once

#include <QtCore/QStringList>

// Feature-13 Part C on a real recording. Counts the dark candidates (with
// darkening on) and the bright ones (with a second detector, without it,
// since Feature-45) of the unchanged recording, then injects dark
// silhouettes crossing the frame and counts the crossings that fire, for
// each K. Explicit paths only; the output is a new JSON file.
//   SeleneWatchProbe darkening INPUT OUTPUT.json K[,K...] [FRAME_LIMIT]
int runDarkeningProbe(const QStringList& arguments);
