#pragma once

#include "core/frame.h"

#include <QtCore/QString>

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Feature-16. Player One controls the adapter reads or sets for raw capture.
enum class PlayerOneControl { Exposure, Gain, Offset, UsbBandwidth };

// SDK range and current value in native integer units (exposure: microseconds).
struct PlayerOneControlInfo
{
    PlayerOneControl kind;
    std::string name;
    int64_t minimum{0}, maximum{0}, value{0};
    bool writable{false}, automatic{false};
};

// Capabilities from enumeration. Formats are the raw transports; sensorBits
// is the ADC depth the SDK reports, separate from the 16-bit transport.
struct PlayerOneCameraInfo
{
    int id{-1}, width{0}, height{0}, sensorBits{0};
    std::string name, serial, sensor;
    BayerPattern bayer{BayerPattern::None};
    std::vector<int> bins;
    std::vector<PixelFormat> formats;
    bool usb3{false}, testSource{false};
    // Feature-30: the sensor's pixel in micrometres, unbinned; 0 when not reported.
    double pixelSizeUm{0};
};

// ROI size and origin in output pixels after binning, as the SDK reads them back.
struct PlayerOneSettings
{
    int width{0}, height{0}, bin{1}, x{0}, y{0};
    PixelFormat format{PixelFormat::Mono8};
    std::map<PlayerOneControl,int64_t> controls;
    bool operator==(const PlayerOneSettings&) const = default;
};

// Identity, capabilities and actual readbacks from one inspection or configuration.
struct PlayerOneProfile
{
    PlayerOneCameraInfo camera;
    std::string sdkVersion;
    std::vector<PlayerOneControlInfo> controls;
    PlayerOneSettings current;
};

// The SDK boundary, also implemented by the test camera. Only
// playerone_sdk.cpp includes the vendor header. Errors throw with their
// operation and SDK status; a frame not ready in time returns false.
// No guide-port, cooler or firmware operation is exposed. Calls are
// serialized by PlayerOneSource ownership.
class PlayerOneApi
{
public:
    virtual ~PlayerOneApi() = default;
    // While disconnected. An empty path means the automatic locations.
    virtual void setRuntimeOverride(const QString& path) = 0;
    [[nodiscard]] virtual QString loadedRuntimePath() const = 0;
    virtual std::string version() = 0;
    virtual std::vector<PlayerOneCameraInfo> cameras() = 0;
    virtual void open(int id) = 0;
    virtual void initialize(int id) = 0;
    virtual void close(int id) noexcept = 0;
    virtual std::vector<PlayerOneControlInfo> controls(int id) = 0;
    virtual void setControl(int id, PlayerOneControl kind, int64_t value) = 0; // manual mode
    virtual PlayerOneSettings roi(int id) = 0;
    // Bin, then format, size and origin, in the order the SDK needs.
    virtual void setRoi(int id, const PlayerOneSettings& settings) = 0;
    // No flip, binned pixels averaged rather than summed, colour bins keep Bayer.
    virtual void nativeOrientation(int id) = 0;
    virtual void startVideo(int id) = 0;
    virtual void stopVideo(int id) noexcept = 0;
    virtual bool read(int id, std::span<uint8_t> destination, int waitMs) = 0;
    virtual int dropped(int id) = 0;
};

// Empty when this build has no Player One SDK header. Constructing it neither
// loads the DLL nor touches USB; discovery is an explicit observer action.
[[nodiscard]] std::shared_ptr<PlayerOneApi> makePlayerOneApi();
