#pragma once

#include <QtCore/QStringList>

// Feature-12 on a recording: the overview picture of a stretch of frames,
// without triggers, as a clip would get it. Explicit paths only; it never
// writes into a clip folder.
//   SeleneWatchProbe overview INPUT OUTPUT.png [FIRST [COUNT [K]]]
// FIRST is a frame position (default 0), COUNT a number of frames (default
// 300, ten seconds at 30 fps), K the sensitivity (default 6).
int runOverviewProbe(const QStringList& arguments);
