#pragma once

#include <cstdint>

// A Gaussian test flash, in frame coordinates and native sample units.
// For a 12-bit stream, amplitude is in 12-bit ADU, wherever those bits sit.
struct BlipSettings
{
    int x{0};
    int y{0};
    double fwhm{3.0};
    double amplitude{100.0};
    int frames{2};
};

// The test mark on one affected frame. Every mark carries the full settings,
// even if the first frame of the injection is outside the saved clip.
struct BlipMark
{
    BlipSettings settings;
    uint64_t firstFrameIndex{0};
    int frameNumber{0}; // 1 to settings.frames, counting delivered frames.
};
