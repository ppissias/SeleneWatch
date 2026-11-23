#pragma once

#include "core/frame.h"

#include <mutex>
#include <optional>

// Feature-48: the ISS as it crosses the sunlit Moon, for tests of the shadows
// watch. Its outline is drawn opaque: the 109 m truss across its path, eight
// solar-array wings (12 x 34 m) and the module stack along it, and two
// radiators. It is smeared along its motion during the exposure (8 % of a
// frame's travel) and softened by seeing (a Gaussian of a fiftieth of its
// length). Under it each sample is scaled by 1 - depth x cover, in every
// channel. lengthPixels is the truss's length; (vx, vy), its motion per
// frame, also orients it, since the station flies with its modules along its
// path. The one implementation for the Test flash bar and the darkening probe.
void paintIssSilhouette(Frame& frame, double centreX, double centreY, double lengthPixels, double depth,
    double vx, double vy);

// Feature-48: the ISS covers its own length in about 14 ms (109 m at
// 7.66 km/s), at any distance, since its size and its speed in the image both
// fall with the distance. Its travel on one frame, in pixels.
[[nodiscard]] double issTravelPerFrame(double lengthPixels, double frameSeconds);

// Adds a requested Gaussian to writable frames, before publication.
//
// The caller may request from another thread. There is one request slot;
// a second request is refused until the first has finished. apply owns the
// active injection on the source thread. Only the small request handoff is
// locked; pixel work never holds the lock.
class BlipInjector
{
public:
    // False for invalid settings or while another injection is pending/active.
    bool request(const BlipSettings& settings);
    [[nodiscard]] bool busy() const;

    // Source thread only. Clears a recycled frame's old blip mark first.
    // Throws for an invalid frame or a centre outside the image.
    void apply(Frame& frame);

    // Only while the source is stopped. Cancels pending and active injections.
    void reset();

private:
    mutable std::mutex mutex_;
    std::optional<BlipSettings> requested_;
    bool busy_{false};

    // Source thread only (or reset while it is joined).
    std::optional<BlipMark> active_;
};
