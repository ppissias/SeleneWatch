#pragma once

#include "frame.h"
#include "display_settings.h"

#include <QtGui/QImage>

// The conversion for the screen. It belongs to the window and returns an
// 8-bit image that owns its pixels. It may shift, stretch, or invert as the
// preview needs. The event writer may use it for clearly labelled display
// PNGs; native SER evidence has its own unchanged-byte write path.
//
// Window levels and automatic percentile/midtones operate in native sample
// units before conversion. Bayer interpolation is display only.
[[nodiscard]] QImage frameToPreviewImage(const Frame& frame, DisplaySettings settings = {}, DisplayLevels* levels = nullptr);

// A writable QImage backed by charged pixels. Shared image copies keep one
// allocation until the last copy is destroyed. For display and writer previews.
[[nodiscard]] QImage makeBudgetImage(int width, int height, QImage::Format format, MemoryBudget budget);
