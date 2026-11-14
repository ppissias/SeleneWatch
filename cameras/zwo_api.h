#pragma once

#include "frame.h"

#include <map>
#include <span>
#include <string>
#include <vector>

enum class ZwoControl { Exposure, Gain, Offset, Bandwidth, HighSpeed, Flip };

struct ZwoControlInfo
{
    ZwoControl kind;
    std::string name;
    int64_t minimum{0}, maximum{0}, value{0};
    bool writable{false}, automatic{false};
};

struct ZwoCameraInfo
{
    int id{-1}, width{0}, height{0}, sensorBits{0};
    std::string name;
    BayerPattern bayer{BayerPattern::None};
    std::vector<int> bins;
    std::vector<PixelFormat> formats;
    bool usb3{false}, triggerCamera{false}, testSource{false};
};

// ROI dimensions and origin are in output pixels, after binning, as returned
// by the SDK. Controls use the SDK's integer units (exposure: microseconds).
struct ZwoSettings
{
    int width{0}, height{0}, bin{1}, x{0}, y{0};
    PixelFormat format{PixelFormat::Mono8};
    std::map<ZwoControl,int64_t> controls;
    bool operator==(const ZwoSettings&) const = default;
};

struct ZwoProfile
{
    ZwoCameraInfo camera;
    std::string serial, sdkVersion;
    std::vector<ZwoControlInfo> controls;
    ZwoSettings current;
};

// The SDK boundary, also implemented by the deterministic test camera.
// Only zwo_sdk.cpp includes vendor headers. Errors throw with their operation
// and SDK status; an ordinary frame timeout returns false. No guide-port or
// firmware operations are exposed. Calls are serialized by ZwoSource ownership.
class ZwoApi
{
public:
    virtual ~ZwoApi() = default;
    virtual std::string version() const = 0;
    virtual std::vector<ZwoCameraInfo> cameras() = 0;
    virtual void open(int id) = 0;
    virtual void initialize(int id) = 0;
    virtual void close(int id) noexcept = 0;
    virtual std::string serial(int id) = 0; // Empty when not supported.
    virtual std::vector<ZwoControlInfo> controls(int id) = 0;
    virtual void setControl(int id,ZwoControl kind,int64_t value) = 0; // Manual mode.
    virtual ZwoSettings roi(int id) = 0;
    virtual void setRoi(int id,const ZwoSettings& settings) = 0;
    virtual void disableDarkSubtract(int id) = 0;
    virtual void normalMode(int id) = 0;
    virtual void startVideo(int id) = 0;
    virtual void stopVideo(int id) noexcept = 0;
    virtual bool read(int id,std::span<uint8_t> destination,int waitMs) = 0;
    virtual int dropped(int id) = 0;
};

// Empty when this build has no SDK configured. Constructing it does not open
// or enumerate hardware; discovery is an explicit observer action.
[[nodiscard]] std::shared_ptr<ZwoApi> makeZwoApi();
