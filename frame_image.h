#pragma once

#include "frame.h"

#include <QtGui/QImage>

// Preview and the event writer use this. Frame itself stays plain data.
[[nodiscard]] QImage frameToQImage(const Frame& frame);
