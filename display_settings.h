#pragma once

#include <cmath>

enum class DisplayMode { Linear, Window, Auto };

// Levels are fractions of the source's native sample range. These affect
// only the displayed image; detection and evidence never read them.
struct DisplaySettings
{
    DisplayMode mode{DisplayMode::Linear};
    double black{0}, white{1};
    bool invert{false};
    bool debayer{true};
    [[nodiscard]] bool valid() const
    {
        return static_cast<int>(mode) >= 0 && static_cast<int>(mode) <= 2
            && std::isfinite(black) && std::isfinite(white) && black >= 0 && white <= 1 && black < white;
    }
};

struct DisplayLevels { double black{0}, white{255}, median{0}; };
