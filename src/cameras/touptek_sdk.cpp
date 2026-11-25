#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cameras/touptek_api.h"

#ifdef SELENEWATCH_WITH_TOUPTEK
#include "cameras/vendor_runtime.h"
#include <QtCore/QDir>
// Declarations only: every call goes through a pointer resolved from the
// observer's DLL, so the header's import markers are switched off.
#define TOUPCAM_NOIMPORTS
#include SELENEWATCH_TOUPTEK_HEADER

#include <format>
#include <optional>
#include <stdexcept>

namespace {

template<class Function> Function resolve(void* module, const char* name)
{
    return reinterpret_cast<Function>(vendorExport(module, name));
}

// An export the adapter can do without: null when the runtime lacks it.
template<class Function> Function optionalExport(void* module, const char* name)
{
    try { return resolve<Function>(module, name); }
    catch (const std::runtime_error&) { return nullptr; }
}

// Every export the adapter uses, resolved before the DLL is accepted.
struct ToupTekFunctions
{
    explicit ToupTekFunctions(void* module) : module(module) {}
    void* module;
#define TOUPCAM_FUNCTION(name) decltype(&::name) name{resolve<decltype(&::name)>(module, #name)}
    TOUPCAM_FUNCTION(Toupcam_Version);
    TOUPCAM_FUNCTION(Toupcam_EnumV2);
    TOUPCAM_FUNCTION(Toupcam_Open);
    TOUPCAM_FUNCTION(Toupcam_Close);
    TOUPCAM_FUNCTION(Toupcam_get_SerialNumber);
    TOUPCAM_FUNCTION(Toupcam_get_Resolution);
    TOUPCAM_FUNCTION(Toupcam_get_RawFormat);
    TOUPCAM_FUNCTION(Toupcam_get_MaxBitDepth);
    TOUPCAM_FUNCTION(Toupcam_get_MaxSpeed);
    TOUPCAM_FUNCTION(Toupcam_get_Speed);
    TOUPCAM_FUNCTION(Toupcam_put_Speed);
    TOUPCAM_FUNCTION(Toupcam_get_Roi);
    TOUPCAM_FUNCTION(Toupcam_put_Roi);
    TOUPCAM_FUNCTION(Toupcam_get_Option);
    TOUPCAM_FUNCTION(Toupcam_put_Option);
    TOUPCAM_FUNCTION(Toupcam_put_AutoExpoEnable);
    TOUPCAM_FUNCTION(Toupcam_get_ExpoTime);
    TOUPCAM_FUNCTION(Toupcam_put_ExpoTime);
    TOUPCAM_FUNCTION(Toupcam_get_ExpTimeRange);
    TOUPCAM_FUNCTION(Toupcam_get_ExpoAGain);
    TOUPCAM_FUNCTION(Toupcam_put_ExpoAGain);
    TOUPCAM_FUNCTION(Toupcam_get_ExpoAGainRange);
    TOUPCAM_FUNCTION(Toupcam_put_HFlip);
    TOUPCAM_FUNCTION(Toupcam_put_VFlip);
    TOUPCAM_FUNCTION(Toupcam_StartPullModeWithCallback);
    // Feature-30: optional, so a runtime without it is still accepted.
    decltype(&::Toupcam_get_PixelSize) Toupcam_get_PixelSize{
        optionalExport<decltype(&::Toupcam_get_PixelSize)>(module, "Toupcam_get_PixelSize")};
    TOUPCAM_FUNCTION(Toupcam_WaitImageV4);
    TOUPCAM_FUNCTION(Toupcam_Stop);
#undef TOUPCAM_FUNCTION
};

const VendorRuntime toupTekRuntime{"ToupTek", "toupcam.dll",
    "Install the camera's Windows driver from ToupTek (or the brand that sold it). Then put the x64 toupcam.dll from "
    "ToupTek's camera SDK (win\\x64) next to SeleneWatch.exe, or select it. SeleneWatch also finds the copy that "
    "N.I.N.A., 64-bit SharpCap or ToupTek's ASCOM driver installs."};

// Beside SeleneWatch.exe first, the observer's own placement; then the
// copies N.I.N.A., 64-bit SharpCap and ToupTek's ASCOM driver install, the
// newest version first. The ASCOM driver's folder is an observer's report
// (astronomie-va.com forum, November 2023), not a ToupTek statement.
QStringList automaticPaths()
{
    QStringList copies = copiesInCapturePrograms("toupcam.dll", "ToupTek");
    if (const auto common = qEnvironmentVariable("CommonProgramFiles(x86)"); !common.isEmpty())
        copies << QDir(common).filePath("ASCOM/x64/toupcam.dll");
    return QStringList{runtimeBesideProgram("toupcam.dll")} + newestFirst(copies);
}

constexpr HRESULT kTimeout = static_cast<HRESULT>(0x8001011FL); // E_TIMEOUT in toupcam.h
constexpr HRESULT kNotImplemented = static_cast<HRESULT>(0x80004001L);

void check(HRESULT code, const char* operation)
{
    if (SUCCEEDED(code)) return;
    throw std::runtime_error(std::format("{} failed (ToupTek status 0x{:08X})", operation, static_cast<uint32_t>(code)));
}

constexpr unsigned fourCC(char a, char b, char c, char d)
{
    return static_cast<unsigned>(static_cast<unsigned char>(a)) | (static_cast<unsigned>(static_cast<unsigned char>(b)) << 8)
        | (static_cast<unsigned>(static_cast<unsigned char>(c)) << 16) | (static_cast<unsigned>(static_cast<unsigned char>(d)) << 24);
}

std::string utf8(const wchar_t* text) { return QString::fromWCharArray(text).toStdString(); }

// Runtime-loaded ToupTek SDK adapter behind ToupTekApi. The only translation
// unit that includes toupcam.h; loads the observer's x64 toupcam.dll
// dynamically via vendor_runtime.
class ToupTekSdk final : public ToupTekApi
{
public:
    ~ToupTekSdk() override
    {
        close();
        unloadRuntime();
    }
    void setRuntimeOverride(const QString& path) override
    {
        if (path == runtimeOverride_) return;
        if (handle_) throw std::logic_error("Stop the ToupTek stream before changing its runtime");
        unloadRuntime();
        runtimeOverride_ = path;
    }
    QString loadedRuntimePath() const override { return loadedRuntimePath_; }
    std::string version() override
    {
        const wchar_t* version = functions().Toupcam_Version();
        return version ? utf8(version) : std::string{};
    }
    std::vector<ToupTekDevice> devices() override
    {
        std::vector<ToupTekDevice> result;
        for (const auto& device : enumerate()) result.push_back({utf8(device.id), utf8(device.displayname)});
        return result;
    }
    void open(const std::string& id) override
    {
        if (handle_) throw std::logic_error("A ToupTek connection is already open");
        const std::wstring wide = QString::fromStdString(id).toStdWString();
        handle_ = functions().Toupcam_Open(wide.c_str());
        if (!handle_) throw std::runtime_error("Opening ToupTek camera failed");
        id_ = id;
        check(functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_UPSIDE_DOWN, 0), "Turning the ToupTek image the right way up");
    }
    ToupTekProfile profile() override
    {
        ToupTekProfile p;
        p.sdkVersion = version();
        const auto device = enumerated(id_);
        p.camera.device = {id_, utf8(device.displayname)};
        const auto flags = device.model ? device.model->flag : 0ULL;
        p.camera.usb3 = (flags & TOUPCAM_FLAG_USB30) != 0;
        int width = 0, height = 0;
        check(functions().Toupcam_get_Resolution(handle_, 0, &width, &height), "Reading ToupTek sensor size");
        p.camera.width = width;
        p.camera.height = height;
        const HRESULT bits = functions().Toupcam_get_MaxBitDepth(handle_);
        check(bits, "Reading ToupTek bit depth");
        p.camera.sensorBits = static_cast<int>(bits);
        // Feature-30: optional; a runtime without it leaves the pixel size unknown.
        float pixelX = 0, pixelY = 0;
        if (functions().Toupcam_get_PixelSize && SUCCEEDED(functions().Toupcam_get_PixelSize(handle_, 0, &pixelX, &pixelY)))
            p.camera.pixelSizeUm = pixelX;
        p.camera.formats = {PixelFormat::Mono8};
        if (p.camera.sensorBits > 8) p.camera.formats.push_back(PixelFormat::Mono16);
        unsigned code = 0, bitsPerPixel = 0;
        check(functions().Toupcam_get_RawFormat(handle_, &code, &bitsPerPixel), "Reading ToupTek raw format");
        if (code == fourCC('R', 'G', 'G', 'B')) p.camera.bayer = BayerPattern::RGGB;
        else if (code == fourCC('G', 'R', 'B', 'G')) p.camera.bayer = BayerPattern::GRBG;
        else if (code == fourCC('G', 'B', 'R', 'G')) p.camera.bayer = BayerPattern::GBRG;
        else if (code == fourCC('B', 'G', 'G', 'R')) p.camera.bayer = BayerPattern::BGGR;
        else if (code != fourCC('Y', 'Y', 'Y', 'Y')) throw std::runtime_error("ToupTek camera reported a raw format other than Bayer or mono");

        char serial[32]{};
        if (SUCCEEDED(functions().Toupcam_get_SerialNumber(handle_, serial)))
            p.serial = std::string(serial, strnlen(serial, sizeof serial));

        unsigned x = 0, y = 0, w = 0, h = 0;
        check(functions().Toupcam_get_Roi(handle_, &x, &y, &w, &h), "Reading ToupTek ROI");
        p.current.x = static_cast<int>(x);
        p.current.y = static_cast<int>(y);
        p.current.width = static_cast<int>(w);
        p.current.height = static_cast<int>(h);
        int depth = 0;
        check(functions().Toupcam_get_Option(handle_, TOUPCAM_OPTION_BITDEPTH, &depth), "Reading ToupTek bit depth mode");
        p.current.format = depth ? PixelFormat::Mono16 : PixelFormat::Mono8;
        const int modeBits = depth ? p.camera.sensorBits : 8;

        unsigned minimum = 0, maximum = 0, standard = 0, time = 0;
        check(functions().Toupcam_get_ExpTimeRange(handle_, &minimum, &maximum, &standard), "Reading ToupTek exposure range");
        check(functions().Toupcam_get_ExpoTime(handle_, &time), "Reading ToupTek exposure");
        p.controls.push_back({ToupTekControl::Exposure, "Exposure", minimum, maximum, time});
        unsigned short gainMinimum = 0, gainMaximum = 0, gainStandard = 0, gain = 0;
        check(functions().Toupcam_get_ExpoAGainRange(handle_, &gainMinimum, &gainMaximum, &gainStandard), "Reading ToupTek gain range");
        check(functions().Toupcam_get_ExpoAGain(handle_, &gain), "Reading ToupTek gain");
        p.controls.push_back({ToupTekControl::Gain, "Gain (%)", gainMinimum, gainMaximum, gain});
        if (flags & TOUPCAM_FLAG_BLACKLEVEL)
        {
            int level = 0;
            check(functions().Toupcam_get_Option(handle_, TOUPCAM_OPTION_BLACKLEVEL, &level), "Reading ToupTek black level");
            // The black level's range scales with the bit depth of the mode: 31 at 8 bits.
            p.controls.push_back({ToupTekControl::BlackLevel, "Black level", TOUPCAM_BLACKLEVEL_MIN,
                static_cast<int64_t>(TOUPCAM_BLACKLEVEL8_MAX) << (modeBits - 8), level});
        }
        int bandwidth = 0;
        const HRESULT bandwidthStatus = functions().Toupcam_get_Option(handle_, TOUPCAM_OPTION_BANDWIDTH, &bandwidth);
        if (SUCCEEDED(bandwidthStatus))
            p.controls.push_back({ToupTekControl::UsbBandwidth, "USB bandwidth (%)", TOUPCAM_BANDWIDTH_MIN, TOUPCAM_BANDWIDTH_MAX, bandwidth});
        else if (bandwidthStatus != kNotImplemented) check(bandwidthStatus, "Reading ToupTek bandwidth");
        const HRESULT maxSpeed = functions().Toupcam_get_MaxSpeed(handle_);
        if (SUCCEEDED(maxSpeed) && maxSpeed > 0)
        {
            unsigned short speed = 0;
            check(functions().Toupcam_get_Speed(handle_, &speed), "Reading ToupTek speed");
            p.controls.push_back({ToupTekControl::Speed, "Speed level", 0, maxSpeed, speed});
        }
        for (const auto& c : p.controls) p.current.controls[c.kind] = c.value;
        return p;
    }
    void configureRaw(const ToupTekSettings& s) override
    {
        check(functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_TRIGGER, 0), "Setting ToupTek video mode");
        check(functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_RAW, 1), "Setting ToupTek raw mode");
        check(functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_BITDEPTH, s.format == PixelFormat::Mono16 ? 1 : 0),
            "Setting ToupTek raw depth");
        check(functions().Toupcam_put_AutoExpoEnable(handle_, 0), "Turning ToupTek automatic exposure off");
        check(functions().Toupcam_put_HFlip(handle_, 0), "Turning ToupTek horizontal flip off");
        check(functions().Toupcam_put_VFlip(handle_, 0), "Turning ToupTek vertical flip off");
        const HRESULT binning = functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_BINNING, 1);
        if (binning != kNotImplemented) check(binning, "Turning ToupTek digital binning off");
        check(functions().Toupcam_put_Roi(handle_, static_cast<unsigned>(s.x), static_cast<unsigned>(s.y),
            static_cast<unsigned>(s.width), static_cast<unsigned>(s.height)), "Setting ToupTek ROI");
        for (const auto& [kind, value] : s.controls) setControl(kind, value);
        int raw = 0;
        check(functions().Toupcam_get_Option(handle_, TOUPCAM_OPTION_RAW, &raw), "Reading ToupTek raw mode");
        if (raw != 1) throw std::runtime_error("ToupTek camera did not read back as raw mode");
    }
    void setControl(ToupTekControl kind, int64_t value) override
    {
        switch (kind)
        {
        case ToupTekControl::Exposure:
            check(functions().Toupcam_put_ExpoTime(handle_, static_cast<unsigned>(value)), "Setting ToupTek exposure"); break;
        case ToupTekControl::Gain:
            check(functions().Toupcam_put_ExpoAGain(handle_, static_cast<unsigned short>(value)), "Setting ToupTek gain"); break;
        case ToupTekControl::BlackLevel:
            check(functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_BLACKLEVEL, static_cast<int>(value)), "Setting ToupTek black level"); break;
        case ToupTekControl::UsbBandwidth:
            check(functions().Toupcam_put_Option(handle_, TOUPCAM_OPTION_BANDWIDTH, static_cast<int>(value)), "Setting ToupTek bandwidth"); break;
        case ToupTekControl::Speed:
            check(functions().Toupcam_put_Speed(handle_, static_cast<unsigned short>(value)), "Setting ToupTek speed"); break;
        }
    }
    void startVideo() override
    {
        check(functions().Toupcam_StartPullModeWithCallback(handle_, nullptr, nullptr), "Starting ToupTek video");
    }
    ToupTekFrame read(std::span<uint8_t> destination, int waitMs) override
    {
        ToupcamFrameInfoV4 info{};
        // In raw mode the bits argument is ignored and the default row pitch has no padding.
        const HRESULT status = functions().Toupcam_WaitImageV4(handle_, static_cast<unsigned>(waitMs), destination.data(), 0, 0, 0, &info);
        if (status == kTimeout) return {};
        check(status, "Reading ToupTek frame");
        ToupTekFrame frame{true, static_cast<int>(info.v3.width), static_cast<int>(info.v3.height), std::nullopt};
        if (info.v3.flag & TOUPCAM_FRAMEINFO_FLAG_SEQ) frame.sequence = info.v3.seq;
        return frame;
    }
    void stopVideo() noexcept override
    {
        if (handle_ && functions_) (void)functions_->Toupcam_Stop(handle_);
    }
    void close() noexcept override
    {
        if (handle_ && functions_) functions_->Toupcam_Close(handle_);
        handle_ = nullptr;
    }

private:
    std::vector<ToupcamDeviceV2> enumerate()
    {
        std::vector<ToupcamDeviceV2> list(TOUPCAM_MAX);
        const unsigned count = functions().Toupcam_EnumV2(list.data());
        list.resize(std::min<size_t>(count, TOUPCAM_MAX));
        return list;
    }
    ToupcamDeviceV2 enumerated(const std::string& id)
    {
        for (const auto& device : enumerate())
            if (utf8(device.id) == id) return device;
        throw std::runtime_error("The ToupTek camera is no longer listed; refresh the camera list");
    }
    const ToupTekFunctions& functions()
    {
        if (functions_) return *functions_;
        // The module is kept in functions_, which resolved every export from it.
        (void)loadVendorRuntime(toupTekRuntime, runtimeOverride_, automaticPaths(),
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
    std::optional<ToupTekFunctions> functions_;
    HToupcam handle_{nullptr};
    std::string id_;
};

} // namespace

std::shared_ptr<ToupTekApi> makeToupTekApi() { return std::make_shared<ToupTekSdk>(); }
#else
std::shared_ptr<ToupTekApi> makeToupTekApi() { return {}; }
#endif
