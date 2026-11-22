#pragma once

#include "mounts/mount.h"

#include <memory>
#include <optional>
#include <string>

// An installed ASCOM telescope driver, reached through late-bound COM
// (IDispatch) by its ProgID. No ASCOM code is copied into SeleneWatch or
// linked at build time; the observer installs the ASCOM Platform and the
// driver.
//
// It reads Connected, CanPulseGuide, CanSetTracking, Tracking, Slewing,
// AtPark, SideOfPier, AlignmentMode, GuideRateRightAscension,
// GuideRateDeclination, and IsPulseGuiding. It writes Connected, writes
// Tracking only to stop it (Feature-32), and calls PulseGuide. Nothing else.
//
// Create, use, and destroy it on one thread: the guide thread. It joins the
// multithreaded COM apartment there. A 32-bit in-process driver cannot be
// loaded into this 64-bit program; connecting to one fails with that reason.
// Drivers that run as their own process (local servers) are not affected.
class AscomMount final : public Mount
{
public:
    explicit AscomMount(std::string progId);
    ~AscomMount() override;

    AscomMount(const AscomMount&) = delete;
    AscomMount& operator=(const AscomMount&) = delete;

    [[nodiscard]] std::string name() const override { return progId_; }
    void connect() override;
    void disconnect() override;
    [[nodiscard]] MountState state() override;
    void pulseGuide(GuideDirection direction, int milliseconds) override;
    void stopTracking() override;

private:
    struct Driver;
    std::string progId_;
    std::unique_ptr<Driver> driver_;
};

// True when this build has the adapter and the ASCOM Platform is installed.
[[nodiscard]] bool ascomAvailable();

// Shows the ASCOM Platform's telescope chooser on the calling (GUI) thread.
// Returns the chosen ProgID, an empty string when the observer cancelled,
// or nothing with the reason in error.
[[nodiscard]] std::optional<std::string> chooseAscomTelescope(const std::string& current, std::string& error);
