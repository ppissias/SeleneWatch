#pragma once

#include <optional>
#include <string>

// A guide pulse direction, as ASCOM numbers them.
enum class GuideDirection
{
    North = 0,
    South = 1,
    East = 2,
    West = 3
};

[[nodiscard]] constexpr const char* guideDirectionName(GuideDirection direction)
{
    switch (direction)
    {
    case GuideDirection::North:
        return "north";
    case GuideDirection::South:
        return "south";
    case GuideDirection::East:
        return "east";
    case GuideDirection::West:
        return "west";
    }
    return "unknown";
}

enum class PierSide
{
    Unknown,
    East,
    West
};

[[nodiscard]] constexpr const char* pierSideName(PierSide side)
{
    return side == PierSide::East ? "east" : side == PierSide::West ? "west" : "unknown";
}

// What the mount reports. A value the driver does not provide stays empty.
struct MountState
{
    bool connected{false};
    bool tracking{false};
    bool slewing{false};
    bool parked{false};
    bool canPulseGuide{false};
    bool pulseGuiding{false};
    // Feature-32: tracking can be stopped at the end of a session.
    bool canSetTracking{false};
    PierSide pierSide{PierSide::Unknown};
    // True for an alt-azimuth mount, which this step refuses to guide.
    bool altAzimuth{false};
    // Degrees per second, as the driver reports them.
    std::optional<double> guideRateRa, guideRateDec;
};

// A mount that can be connected, asked for its state, and sent guide pulses.
// One more command, stopTracking, which a session sends when it ends by
// itself and the observer chose it (Feature-32). Nothing else: no slew,
// sync, park, tracking start, axis rate, or rate offset. A pulse ends by
// itself and stopping tracking stops motion, so a crash or a lost connection
// cannot leave the mount moving because of SeleneWatch.
//
// Two implementations: SimulatedMount for tests and the synthetic source,
// and AscomMount for real drivers. All calls are made on the guide thread.
// Errors from the mount are thrown as std::runtime_error with the reason.
class Mount
{
public:
    virtual ~Mount() = default;

    // What the observer chose, for the window and the log.
    [[nodiscard]] virtual std::string name() const = 0;
    virtual void connect() = 0;
    virtual void disconnect() = 0;
    [[nodiscard]] virtual MountState state() = 0;
    // May return before the pulse ends; state().pulseGuiding tells when it has.
    virtual void pulseGuide(GuideDirection direction, int milliseconds) = 0;
    // Only when state().canSetTracking.
    virtual void stopTracking() = 0;
};
