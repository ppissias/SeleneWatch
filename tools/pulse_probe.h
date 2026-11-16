#pragma once

#include <QtCore/QStringList>

// Feature-8 follow-up by hand: do guide corrections make the detector fire?
//
// Plays a recording once and runs the detector, as the application does, on
// a crop of every frame. The control crop stays put. Each trial moves its
// crop the way a guide correction moves the image: from 6 s on, every 6 s a
// pulse moves it at a fixed speed in pixels per frame for a fixed time,
// along +x, +y, -x, -y in turn, so the crop comes back to where it started.
// Offsets are rounded to whole pixels: no pixel is resampled, so the noise
// is the recording's own, but a step of one pixel replaces a smooth glide.
//
// For each K it reports the candidates inside the windows the writer marks
// "during mount correction" (a pulse and its settle time), those after a
// window, and the same counts for the control. Writes a JSON report and
// prints a table. Explicit paths only; the recording is only read.
//   SeleneWatchProbe guide-pulses INPUT OUTPUT.json K[,K...] [FRAME_LIMIT]
int runGuidePulseProbe(const QStringList& arguments);
