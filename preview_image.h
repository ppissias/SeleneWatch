#pragma once

#include "frame.h"

#include <QtGui/QImage>

// The conversion for the screen. It belongs to the window and returns an
// 8-bit image that owns its pixels. It may shift, stretch, or invert as the
// preview needs, so it is never used to write a file. The event writer has
// its own conversion, which keeps the stored samples exactly.
//
// Mono16 is shown as its top 8 significant bits. A Bayer mosaic is shown as
// the grey mosaic it is. A frame whose data does not match its size and
// format gives a null image.
[[nodiscard]] QImage frameToPreviewImage(const Frame& frame);
