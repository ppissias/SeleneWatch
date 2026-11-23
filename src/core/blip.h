#pragma once

#include <cstdint>

// Shape/behaviour of an injected test signal (behind View -> Testing tools).
enum class BlipKind
{
    BrightFlash,   // Stationary positive Gaussian flash (default, lunar impact test)
    DarkTransit,   // Moving negative Gaussian silhouette (lunar darkening/transit test)
    MovingStreak,  // Fast motion-smeared bright streak (meteor test)
    BlinkingMover, // Slow steady nav-light with periodic bright strobe (aircraft test)
    Lightning,     // Uniform full-frame brightening (large-brightening test)
    IssSilhouette  // The ISS crossing the sunlit Moon: an opaque outline at real speed (transit test)
};

[[nodiscard]] inline const char* blipKindName(BlipKind kind)
{
    switch (kind)
    {
    case BlipKind::BrightFlash: return "bright_flash";
    case BlipKind::DarkTransit: return "dark_transit";
    case BlipKind::MovingStreak: return "moving_streak";
    case BlipKind::BlinkingMover: return "blinking_mover";
    case BlipKind::Lightning: return "lightning";
    case BlipKind::IssSilhouette: return "iss_silhouette";
    }
    return "bright_flash";
}

// A test signal, in frame coordinates and native sample units.
// For a 12-bit stream, amplitude is in 12-bit ADU, wherever those bits sit.
// For IssSilhouette, fwhm is the station's length in pixels and amplitude the
// percentage of the light its outline blocks (Feature-48).
struct BlipSettings
{
    int x{0};
    int y{0};
    double fwhm{3.0};
    double amplitude{100.0};
    int frames{2};
    BlipKind kind{BlipKind::BrightFlash};
    double vx{0.0}; // Frame pixels per delivered frame (for moving kinds).
    double vy{0.0};
};

// The test mark on one affected frame. Every mark carries the full settings,
// even if the first frame of the injection is outside the saved clip.
struct BlipMark
{
    BlipSettings settings;
    uint64_t firstFrameIndex{0};
    int frameNumber{0}; // 1 to settings.frames, counting delivered frames.
};
