#pragma once

#include "flash_detector.h"

// Direct detector tests use both phases. Capture retains its pre-update
// difference between these phases; tests needing that snapshot call them apart.
inline bool observe(FlashDetector& detector, const Frame& frame, DetectionDisplayRequest display = {})
{
    const bool triggered=detector.analyze(frame,display);
    detector.updateReference(frame);
    return triggered;
}
