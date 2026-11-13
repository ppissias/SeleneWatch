#pragma once

#include "frame.h"

#include <mutex>
#include <optional>

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
