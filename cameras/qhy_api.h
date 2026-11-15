#pragma once

#include "frame.h"
#include <QtCore/QString>
#include <map>
#include <span>
#include <string>
#include <vector>

// Manual QHY controls exposed by the adapter when the camera supports them.
enum class QhyControl { Exposure, Gain, Offset, UsbTraffic, TransferSpeed };

// One control's range, step and current value in SDK units. Exposure uses microseconds.
struct QhyControlInfo
{
    QhyControl kind;
    std::string name;
    double minimum{0}, maximum{0}, step{1}, value{0};
};
// SDK discovery identifier and model name used to select a camera.
struct QhyDevice { std::string id, name; };

// Discovered capabilities. Sensor bit depth describes the ADC and may differ from transport depth.
struct QhyCameraInfo
{
    QhyDevice device;
    int width{0}, height{0}, sensorBits{0};
    BayerPattern bayer{BayerPattern::None};
    std::vector<PixelFormat> formats;
    bool testSource{false};
};
// Initial QHY qualification deliberately uses bin 1. ROI is independently read
// back. The bin command has no independent getter in the inspected SDK API.
struct QhySettings
{
    int width{0}, height{0}, x{0}, y{0}, bin{1};
    PixelFormat format{PixelFormat::Mono8};
    std::map<QhyControl,double> controls;
};
// Snapshot of device capabilities, SDK version and the settings read back from the camera.
struct QhyProfile
{
    QhyCameraInfo camera;
    std::string sdkVersion;
    QhySettings current;
    std::vector<QhyControlInfo> controls;
};
// Live-read result. Geometry and channel count describe the delivered buffer when ready is true.
struct QhyRead
{
    bool ready{false};
    int width{0}, height{0}, bits{0}, channels{0};
};

// One QHY connection, with a deterministic implementation used by tests. Only
// qhy_sdk.cpp includes vendor headers. read() reports not-ready separately from
// a device/error exception; the SDK's live read has no timeout argument.
class QhyApi
{
public:
    virtual ~QhyApi() = default;
    // While disconnected. An empty path restores automatic discovery.
    virtual void setRuntimeOverride(const QString& path) = 0;
    // Diagnostic path from the last successful load. Query while disconnected.
    virtual QString loadedRuntimePath() const = 0;
    virtual std::string version() = 0;
    virtual std::vector<QhyDevice> devices() = 0;
    virtual void open(const std::string& id) = 0;
    virtual void initializeLive() = 0;
    virtual QhyProfile profile() = 0;
    virtual void configureRaw(const QhySettings& settings) = 0;
    virtual void setControl(QhyControl control, double value) = 0;
    virtual size_t bufferBytes() = 0;
    virtual void startVideo() = 0;
    virtual QhyRead read(std::span<uint8_t> destination) = 0;
    virtual void stopVideo() = 0;
    virtual void close() noexcept = 0;
};

// No enumeration, DLL load or global SDK initialization at construction.
// Empty in builds without QHY SDK headers configured.
std::shared_ptr<QhyApi> makeQhyApi();
