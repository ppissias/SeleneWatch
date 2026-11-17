#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cameras/playerone_api.h"

#ifdef SELENEWATCH_WITH_PLAYERONE
#include "cameras/vendor_runtime.h"
// The declarations give the function types; every call goes through a pointer
// resolved from the observer's DLL, so nothing here links against the SDK.
#include SELENEWATCH_PLAYERONE_HEADER

#include <chrono>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

template<class Function> Function resolve(void* module, const char* name)
{
    return reinterpret_cast<Function>(vendorExport(module, name));
}

// Every export the adapter uses, resolved before the DLL is accepted.
struct PlayerOneFunctions
{
    explicit PlayerOneFunctions(void* module) : module(module) {}
    void* module;
#define POA_FUNCTION(name) decltype(&::name) name{resolve<decltype(&::name)>(module, #name)}
    POA_FUNCTION(POAGetCameraCount);
    POA_FUNCTION(POAGetCameraProperties);
    POA_FUNCTION(POAOpenCamera);
    POA_FUNCTION(POAInitCamera);
    POA_FUNCTION(POACloseCamera);
    POA_FUNCTION(POAGetConfigsCount);
    POA_FUNCTION(POAGetConfigAttributes);
    POA_FUNCTION(POASetConfig);
    POA_FUNCTION(POAGetConfig);
    POA_FUNCTION(POAGetImageStartPos);
    POA_FUNCTION(POASetImageStartPos);
    POA_FUNCTION(POAGetImageSize);
    POA_FUNCTION(POASetImageSize);
    POA_FUNCTION(POAGetImageBin);
    POA_FUNCTION(POASetImageBin);
    POA_FUNCTION(POAGetImageFormat);
    POA_FUNCTION(POASetImageFormat);
    POA_FUNCTION(POAStartExposure);
    POA_FUNCTION(POAStopExposure);
    POA_FUNCTION(POAImageReady);
    POA_FUNCTION(POAGetImageData);
    POA_FUNCTION(POAGetDroppedImagesCount);
    POA_FUNCTION(POAGetErrorString);
    POA_FUNCTION(POAGetSDKVersion);
#undef POA_FUNCTION
};

const VendorRuntime playerOneRuntime{"Player One", "PlayerOneCamera.dll",
    "Install Player One's camera driver, then select the x64 PlayerOneCamera.dll from Player One's Windows camera SDK "
    "(lib\\x64) or from your capture software's folder."};

POAConfig native(PlayerOneControl kind)
{
    switch (kind)
    {
    case PlayerOneControl::Exposure: return POA_EXPOSURE;
    case PlayerOneControl::Gain: return POA_GAIN;
    case PlayerOneControl::Offset: return POA_OFFSET;
    case PlayerOneControl::UsbBandwidth: return POA_USB_BANDWIDTH_LIMIT;
    }
    throw std::invalid_argument("Unknown Player One control");
}

const char* controlName(PlayerOneControl kind)
{
    switch (kind)
    {
    case PlayerOneControl::Exposure: return "Exposure";
    case PlayerOneControl::Gain: return "Gain";
    case PlayerOneControl::Offset: return "Offset";
    case PlayerOneControl::UsbBandwidth: return "USB bandwidth";
    }
    return "";
}

std::optional<PixelFormat> rawFormat(POAImgFormat format)
{
    if (format == POA_RAW8) return PixelFormat::Mono8;
    if (format == POA_RAW16) return PixelFormat::Mono16;
    return std::nullopt;
}

std::string text(const char* buffer, size_t size)
{
    size_t length = 0;
    while (length < size && buffer[length]) ++length;
    return {buffer, length};
}

class PlayerOneSdk final : public PlayerOneApi
{
public:
    ~PlayerOneSdk() override { unloadRuntime(); }
    void setRuntimeOverride(const QString& path) override
    {
        if (path == runtimeOverride_) return;
        unloadRuntime();
        runtimeOverride_ = path;
    }
    QString loadedRuntimePath() const override { return loadedRuntimePath_; }
    std::string version() override
    {
        const char* version = functions().POAGetSDKVersion();
        return version ? version : "";
    }
    std::vector<PlayerOneCameraInfo> cameras() override
    {
        const int count = functions().POAGetCameraCount();
        std::vector<PlayerOneCameraInfo> result;
        for (int i = 0; i < count; ++i)
        {
            POACameraProperties p{};
            check(functions().POAGetCameraProperties(i, &p), "Reading Player One camera information");
            PlayerOneCameraInfo c;
            c.id = p.cameraID;
            c.name = text(p.cameraModelName, sizeof p.cameraModelName);
            c.serial = text(p.SN, sizeof p.SN);
            c.sensor = text(p.sensorModelName, sizeof p.sensorModelName);
            c.width = p.maxWidth;
            c.height = p.maxHeight;
            c.sensorBits = p.bitDepth;
            c.usb3 = p.isUSB3Speed == POA_TRUE;
            if (p.isColorCamera == POA_TRUE)
            {
                switch (p.bayerPattern)
                {
                case POA_BAYER_RG: c.bayer = BayerPattern::RGGB; break;
                case POA_BAYER_BG: c.bayer = BayerPattern::BGGR; break;
                case POA_BAYER_GR: c.bayer = BayerPattern::GRBG; break;
                case POA_BAYER_GB: c.bayer = BayerPattern::GBRG; break;
                default: throw std::runtime_error("Player One colour camera reported an unknown Bayer pattern");
                }
            }
            for (const int bin : p.bins) { if (!bin) break; c.bins.push_back(bin); }
            for (const auto format : p.imgFormats)
            {
                if (format == POA_END) break;
                if (const auto raw = rawFormat(format)) c.formats.push_back(*raw);
            }
            result.push_back(std::move(c));
        }
        return result;
    }
    void open(int id) override { check(functions().POAOpenCamera(id), "Opening Player One camera"); }
    void initialize(int id) override { check(functions().POAInitCamera(id), "Initializing Player One camera"); }
    void close(int id) noexcept override
    {
        if (functions_) (void)functions_->POACloseCamera(id);
    }
    std::vector<PlayerOneControlInfo> controls(int id) override
    {
        int count = 0;
        check(functions().POAGetConfigsCount(id, &count), "Reading Player One control count");
        std::vector<PlayerOneControlInfo> result;
        for (int i = 0; i < count; ++i)
        {
            POAConfigAttributes attributes{};
            check(functions().POAGetConfigAttributes(id, i, &attributes), "Reading Player One control capabilities");
            for (const auto kind : {PlayerOneControl::Exposure, PlayerOneControl::Gain, PlayerOneControl::Offset,
                     PlayerOneControl::UsbBandwidth})
            {
                if (attributes.configID != native(kind)) continue;
                if (attributes.valueType != VAL_INT) throw std::runtime_error(std::string("Player One control is not an integer: ") + controlName(kind));
                POAConfigValue value{};
                POABool automatic = POA_FALSE;
                check(functions().POAGetConfig(id, attributes.configID, &value, &automatic), "Reading Player One control value");
                result.push_back({kind, controlName(kind), attributes.minValue.intValue, attributes.maxValue.intValue,
                    value.intValue, attributes.isWritable == POA_TRUE, automatic == POA_TRUE});
            }
        }
        return result;
    }
    void setControl(int id, PlayerOneControl kind, int64_t value) override
    {
        if (value < std::numeric_limits<long>::min() || value > std::numeric_limits<long>::max())
            throw std::invalid_argument("Player One control exceeds the SDK integer range");
        POAConfigValue native_value{};
        native_value.intValue = static_cast<long>(value);
        check(functions().POASetConfig(id, native(kind), native_value, POA_FALSE), "Setting Player One manual control");
    }
    PlayerOneSettings roi(int id) override
    {
        PlayerOneSettings s;
        POAImgFormat format = POA_END;
        check(functions().POAGetImageBin(id, &s.bin), "Reading Player One bin");
        check(functions().POAGetImageSize(id, &s.width, &s.height), "Reading Player One ROI size");
        check(functions().POAGetImageStartPos(id, &s.x, &s.y), "Reading Player One ROI origin");
        check(functions().POAGetImageFormat(id, &format), "Reading Player One image format");
        const auto raw = rawFormat(format);
        // A processed format reads back as RGB and is never accepted for capture.
        s.format = raw ? *raw : PixelFormat::RGB888;
        return s;
    }
    void setRoi(int id, const PlayerOneSettings& s) override
    {
        check(functions().POASetImageBin(id, s.bin), "Setting Player One bin");
        check(functions().POASetImageFormat(id, s.format == PixelFormat::Mono16 ? POA_RAW16 : POA_RAW8), "Setting Player One raw format");
        check(functions().POASetImageSize(id, s.width, s.height), "Setting Player One ROI size");
        check(functions().POASetImageStartPos(id, s.x, s.y), "Setting Player One ROI origin");
    }
    void nativeOrientation(int id) override
    {
        int count = 0;
        check(functions().POAGetConfigsCount(id, &count), "Reading Player One control count");
        for (int i = 0; i < count; ++i)
        {
            POAConfigAttributes attributes{};
            check(functions().POAGetConfigAttributes(id, i, &attributes), "Reading Player One control capabilities");
            if (attributes.isWritable != POA_TRUE) continue;
            POAConfigValue value{};
            if (attributes.configID == POA_FLIP_NONE) value.boolValue = POA_TRUE;
            else if (attributes.configID == POA_PIXEL_BIN_SUM || attributes.configID == POA_MONO_BIN) value.boolValue = POA_FALSE;
            else continue;
            check(functions().POASetConfig(id, attributes.configID, value, POA_FALSE), "Setting Player One native orientation");
        }
    }
    void startVideo(int id) override { check(functions().POAStartExposure(id, POA_FALSE), "Starting Player One video"); }
    void stopVideo(int id) noexcept override
    {
        if (functions_) (void)functions_->POAStopExposure(id);
    }
    bool read(int id, std::span<uint8_t> data, int waitMs) override
    {
        if (data.size() > static_cast<size_t>(std::numeric_limits<long>::max())) throw std::invalid_argument("Player One frame buffer too large");
        // The SDK recommends waiting on POAImageReady; the read then returns at once.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{waitMs};
        for (;;)
        {
            POABool ready = POA_FALSE;
            check(functions().POAImageReady(id, &ready), "Checking for a Player One frame");
            if (ready == POA_TRUE)
            {
                check(functions().POAGetImageData(id, data.data(), static_cast<long>(data.size()), 1000), "Reading Player One frame");
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    int dropped(int id) override
    {
        int count = 0;
        check(functions().POAGetDroppedImagesCount(id, &count), "Reading Player One dropped-frame count");
        return count;
    }

private:
    void check(POAErrors code, const char* operation)
    {
        if (code == POA_OK) return;
        const char* reason = functions_ ? functions_->POAGetErrorString(code) : nullptr;
        throw std::runtime_error(std::format("{}: {} (Player One status {})", operation,
            reason && *reason ? reason : "SDK operation failed", static_cast<int>(code)));
    }
    const PlayerOneFunctions& functions()
    {
        if (functions_) return *functions_;
        // The module is kept in functions_, which resolved every export from it.
        (void)loadVendorRuntime(playerOneRuntime, runtimeOverride_, {runtimeBesideProgram("PlayerOneCamera.dll")},
            [this](void* candidate) { functions_.emplace(candidate); }, loadedRuntimePath_);
        return *functions_;
    }
    void unloadRuntime() noexcept
    {
        if (!functions_) return;
        freeVendorRuntime(functions_->module);
        functions_.reset();
        loadedRuntimePath_.clear();
    }
    QString runtimeOverride_;
    QString loadedRuntimePath_;
    std::optional<PlayerOneFunctions> functions_;
};

} // namespace

std::shared_ptr<PlayerOneApi> makePlayerOneApi() { return std::make_shared<PlayerOneSdk>(); }
#else
std::shared_ptr<PlayerOneApi> makePlayerOneApi() { return {}; }
#endif
