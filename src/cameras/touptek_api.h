#pragma once

#include "core/frame.h"

#include <QtCore/QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Feature-16. ToupTek controls the adapter reads or sets for raw capture.
// Gain is ToupTek's percentage (100 is unity); black level and bandwidth are
// SDK options; speed is ToupTek's frame speed level.
enum class ToupTekControl { Exposure, Gain, BlackLevel, UsbBandwidth, Speed };

// SDK range and current value in native integer units (exposure: microseconds).
struct ToupTekControlInfo
{
    ToupTekControl kind;
    std::string name;
    int64_t minimum{0}, maximum{0}, value{0};
};
// The SDK's opaque camera ID (it may change between connections) and its display name.
struct ToupTekDevice { std::string id, name; };

// Capabilities read from an open camera. sensorBits is the deepest raw mode
// the SDK reports, separate from the 16-bit transport.
struct ToupTekCameraInfo
{
    ToupTekDevice device;
    int width{0}, height{0}, sensorBits{0};
    BayerPattern bayer{BayerPattern::None};
    std::vector<PixelFormat> formats;
    bool usb3{false}, testSource{false};
    // Feature-30: the sensor's pixel in micrometres, unbinned; 0 when not reported.
    double pixelSizeUm{0};
};
// ROI in sensor pixels; bin 1 only until binned raw data is qualified.
struct ToupTekSettings
{
    int width{0}, height{0}, x{0}, y{0};
    PixelFormat format{PixelFormat::Mono8};
    std::map<ToupTekControl,int64_t> controls;
    bool operator==(const ToupTekSettings&) const = default;
};
// Identity, capabilities and actual readbacks from one inspection or configuration.
struct ToupTekProfile
{
    ToupTekCameraInfo camera;
    std::string serial, sdkVersion;
    std::vector<ToupTekControlInfo> controls;
    ToupTekSettings current;
};
// A delivered frame's geometry and the camera's own frame sequence number,
// when the SDK reports one.
struct ToupTekFrame
{
    bool ready{false};
    int width{0}, height{0};
    std::optional<uint32_t> sequence;
};

// One ToupTek connection, with a test implementation. Only touptek_sdk.cpp
// includes the vendor header. Errors throw with their operation and SDK
// status; a frame not ready in time returns ready == false. No trigger,
// cooler, GPIO or firmware operation is exposed.
class ToupTekApi
{
public:
    virtual ~ToupTekApi() = default;
    // While disconnected. An empty path means the automatic locations.
    virtual void setRuntimeOverride(const QString& path) = 0;
    [[nodiscard]] virtual QString loadedRuntimePath() const = 0;
    virtual std::string version() = 0;
    virtual std::vector<ToupTekDevice> devices() = 0;
    // Opens with the image the right way up: the SDK turns it upside down
    // on Windows by default, which would also change the Bayer cell.
    virtual void open(const std::string& id) = 0;
    virtual ToupTekProfile profile() = 0;
    // Video mode, raw data at the requested depth, no flips or automatic
    // exposure, the ROI, then the controls.
    virtual void configureRaw(const ToupTekSettings& settings) = 0;
    virtual void setControl(ToupTekControl kind, int64_t value) = 0;
    virtual void startVideo() = 0;
    virtual ToupTekFrame read(std::span<uint8_t> destination, int waitMs) = 0;
    virtual void stopVideo() noexcept = 0;
    virtual void close() noexcept = 0;
};

// Empty when this build has no ToupTek SDK header. Constructing it neither
// loads the DLL nor touches USB.
[[nodiscard]] std::shared_ptr<ToupTekApi> makeToupTekApi();
