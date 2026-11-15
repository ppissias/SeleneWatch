#pragma once

#include <QtCore/QStringList>

// Feature-8 Part B by hand, through the real threads: a Monitoring session on
// the synthetic source with the simulated mount. Locks on the most lockable
// region, calibrates, guides for SECONDS (default 120), and reports the
// calibration against the simulation, the field offset while guiding, and
// the pulses. Writes guiding.txt and candidates.txt under OUTPUT_DIRECTORY.
//   SeleneWatchProbe guide-sim OUTPUT_DIRECTORY [SECONDS]
int runGuideSimulation(const QStringList& arguments);

// Feature-8 Part C by hand: connects to an ASCOM telescope driver on a
// worker thread (as the guider does), reads its state, sends one 300 ms
// pulse in each direction, times how long the driver reports each pulse,
// and disconnects. Use a simulator, or a real mount that is safe to nudge.
//   SeleneWatchProbe ascom PROGID OUTPUT.txt
int runAscomProbe(const QStringList& arguments);
