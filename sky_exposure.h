#pragma once

#include "detection_mask.h"
#include "frame.h"

#include <chrono>
#include <optional>
#include <string>

// Feature-15 Part B: an exposure that follows the sky's brightness.
//
// The sky's level is the median sample outside the mask, as a share of the
// full scale. While it stays inside the bounds nothing changes. After it has
// been outside them for the hold time, one change is made: the exposure by
// the ratio to the aim (at most a factor of two), within its limits; when
// the exposure is at a limit, the gain by its step instead. Changes are at
// least the spacing apart. Applying a change is the window's job.

struct SkyExposureRule
{
    double low{0.08}, high{0.30}, aim{0.15};
    double minExposureUs{1000}, maxExposureUs{2'000'000};
    double minGain{0}, maxGain{0}, gainStep{0};
    std::chrono::seconds hold{60};
    std::chrono::seconds spacing{120};
};

// The rule's memory between measurements.
struct SkyExposureState
{
    std::optional<std::chrono::steady_clock::time_point> outsideSince;
    std::optional<std::chrono::steady_clock::time_point> lastChange;
};

struct SkyExposureChange
{
    double exposureUs{0};
    double gain{0};
    std::string reason;
};

[[nodiscard]] std::optional<SkyExposureChange> decideSkyExposure(double level, double exposureUs, double gain,
    std::chrono::steady_clock::time_point now, SkyExposureState& state, const SkyExposureRule& rule);

// The median of a bounded grid of samples outside the mask, as a share of
// the frame's full scale. Bayer and RGB samples count as they are stored.
[[nodiscard]] double skyLevel(const Frame& frame, const DetectionMask& mask);
