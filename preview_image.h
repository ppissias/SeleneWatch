#pragma once

#include "frame.h"

#include <QtGui/QImage>

// The conversion for the screen. It belongs to the window and returns an
// 8-bit image that owns its pixels. It may shift, stretch, or invert as the
// preview needs, so it is never used to write a file. The event writer has
// its own conversion, which keeps the stored samples exactly.
[[nodiscard]] QImage frameToPreviewImage(const Frame& frame);
