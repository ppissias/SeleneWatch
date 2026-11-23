#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cameras/qhy_api.h"

#ifdef SELENEWATCH_WITH_QHY
#include "cameras/vendor_runtime.h"
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <Windows.h>
// These declarations are consumed through GetProcAddress, not exported by us.
#define EXPORTFUNC extern "C"
#include SELENEWATCH_QHY_HEADER
#undef EXPORTFUNC
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {
void check(uint32_t code,const char* operation)
{
    if (code!=QHYCCD_SUCCESS)
        throw std::runtime_error(std::format("{} failed (QHY status 0x{:08X})",operation,code));
}
CONTROL_ID native(QhyControl kind)
{
    switch (kind)
    {
    case QhyControl::Exposure:return CONTROL_EXPOSURE;
    case QhyControl::Gain:return CONTROL_GAIN;
    case QhyControl::Offset:return CONTROL_OFFSET;
    case QhyControl::UsbTraffic:return CONTROL_USBTRAFFIC;
    case QhyControl::TransferSpeed:return CONTROL_SPEED;
    }
    throw std::invalid_argument("Unknown QHY control");
}
std::string text(const std::array<char,256>& buffer)
{
    const auto end=std::find(buffer.begin(),buffer.end(),'\0');
    if (end==buffer.end()) throw std::runtime_error("Unterminated QHY device text");
    return {buffer.begin(),end};
}
int dimension(uint32_t value)
{
    if (value>static_cast<uint32_t>(std::numeric_limits<int>::max())) throw std::runtime_error("QHY dimension exceeds application range");
    return static_cast<int>(value);
}

template<class Function> Function resolve(void* library, const char* name)
{
    return reinterpret_cast<Function>(vendorExport(library,name));
}

// Resolve the complete API before choosing a runtime. An incompatible DLL
// in an automatic location can then be skipped in favor of the next one.
struct QhyFunctions
{
    explicit QhyFunctions(void* module) : module(module) {}
    void* module;

#define QHY_FUNCTION(name) decltype(&::name) name{resolve<decltype(&::name)>(module,#name)}
    QHY_FUNCTION(BeginQHYCCDLive);
    QHY_FUNCTION(CloseQHYCCD);
    QHY_FUNCTION(EnableQHYCCDLogFile);
    QHY_FUNCTION(EnableQHYCCDMessage);
    QHY_FUNCTION(GetQHYCCDChipInfo);
    QHY_FUNCTION(GetQHYCCDCurrentROI);
    QHY_FUNCTION(GetQHYCCDId);
    QHY_FUNCTION(GetQHYCCDLiveFrame);
    QHY_FUNCTION(GetQHYCCDMemLength);
    QHY_FUNCTION(GetQHYCCDModel);
    QHY_FUNCTION(GetQHYCCDParam);
    QHY_FUNCTION(GetQHYCCDParamMinMaxStep);
    QHY_FUNCTION(GetQHYCCDSDKVersion);
    QHY_FUNCTION(InitQHYCCD);
    QHY_FUNCTION(InitQHYCCDResource);
    QHY_FUNCTION(IsQHYCCDControlAvailable);
    QHY_FUNCTION(OpenQHYCCD);
    QHY_FUNCTION(ReleaseQHYCCDResource);
    QHY_FUNCTION(ScanQHYCCD);
    QHY_FUNCTION(SetQHYCCDBinMode);
    QHY_FUNCTION(SetQHYCCDBitsMode);
    QHY_FUNCTION(SetQHYCCDDebayerOnOff);
    QHY_FUNCTION(SetQHYCCDParam);
    QHY_FUNCTION(SetQHYCCDResolution);
    QHY_FUNCTION(SetQHYCCDStreamMode);
    QHY_FUNCTION(StopQHYCCDLive);
#undef QHY_FUNCTION
};

QStringList installedRuntimePaths()
{
    QStringList paths;
    if (const auto beside=runtimeBesideProgram("qhyccd.dll"); !beside.isEmpty()) paths << beside;

    for (const auto* variable : {"ProgramFiles","ProgramW6432","ProgramFiles(x86)"})
    {
        const auto dir=qEnvironmentVariable(variable);
        if (!dir.isEmpty()) paths << QDir(dir).filePath("QHYCCD/AllInOne/sdk/x64/qhyccd.dll");
    }
    for (const auto& desktop : QStandardPaths::standardLocations(QStandardPaths::DesktopLocation))
        paths << QDir(desktop).filePath("QHYCCD-SDK/x64/qhyccd.dll");
    paths.removeDuplicates();
    return paths;
}

const VendorRuntime qhyRuntime{"QHY","qhyccd.dll",
    "Install QHY's Windows All-In-One driver and SDK, or select its x64 qhyccd.dll."};

// Construction neither loads the SDK nor touches USB. Only an absolute
// installed path is loaded, with dependencies resolved beside it or in System32.
class QhySdk final : public QhyApi
{
public:
    ~QhySdk() override
    {
        close();
        unloadRuntime();
    }
    void setRuntimeOverride(const QString& path) override
    {
        if (path==runtimeOverride_) return;
        if (handle_) throw std::logic_error("Stop the QHY stream before changing its runtime");
        unloadRuntime();
        runtimeOverride_=path;
    }
    QString loadedRuntimePath() const override { return loadedRuntimePath_; }
    std::string version() override
    {
        uint32_t year=0,month=0,day=0,subday=0;
        check(functions().GetQHYCCDSDKVersion(&year,&month,&day,&subday),"Reading QHY SDK version");
        return std::format("{}.{}.{}.{}",year,month,day,subday);
    }
    std::vector<QhyDevice> devices() override
    {
        initializeResources();
        const auto count=functions().ScanQHYCCD();
        if (count>1024) throw std::runtime_error(std::format(
            "QHY camera scan failed (0x{:08X}). Check the QHY camera driver and connection, then refresh.",count));
        std::vector<QhyDevice> result;
        for (uint32_t i=0;i<count;++i)
        {
            std::array<char,256> id{},model{};
            check(functions().GetQHYCCDId(i,id.data()),"Reading QHY camera ID");
            check(functions().GetQHYCCDModel(id.data(),model.data()),"Reading QHY camera model");
            result.push_back({text(id),text(model)});
        }
        return result;
    }
    void open(const std::string& id) override
    {
        if (handle_) throw std::logic_error("A QHY connection is already open");
        initializeResources(); id_=id;
        handle_=functions().OpenQHYCCD(id_.data());
        if (!handle_) throw std::runtime_error("Opening QHY camera failed");
    }
    void initializeLive() override
    {
        check(functions().SetQHYCCDStreamMode(handle_,1),"Setting QHY live mode");
        check(functions().InitQHYCCD(handle_),"Initializing QHY camera");
        rawConfigured_=false;
    }
    QhyProfile profile() override
    {
        QhyProfile p; p.camera.device.id=id_; p.sdkVersion=version();
        std::array<char,256> model{};
        check(functions().GetQHYCCDModel(id_.data(),model.data()),"Reading QHY model");
        p.camera.device.name=text(model);
        uint32_t width=0,height=0,bits=0; double cw=0,ch=0,pw=0,ph=0;
        check(functions().GetQHYCCDChipInfo(handle_,&cw,&ch,&width,&height,&pw,&ph,&bits),"Reading QHY sensor geometry");
        p.camera.width=dimension(width); p.camera.height=dimension(height); p.camera.sensorBits=dimension(bits);
        p.camera.pixelSizeUm=pw>0 && pw<100 ? pw : 0;
        const auto colour=available(CAM_COLOR);
        switch (colour)
        {
        case BAYER_GB:p.camera.bayer=BayerPattern::GBRG;break;
        case BAYER_GR:p.camera.bayer=BayerPattern::GRBG;break;
        case BAYER_BG:p.camera.bayer=BayerPattern::BGGR;break;
        case BAYER_RG:p.camera.bayer=BayerPattern::RGGB;break;
        case QHYCCD_ERROR:case QHYCCD_SUCCESS:break;
        default:throw std::runtime_error("QHY camera reported an unknown colour pattern");
        }
        if (available(CAM_8BITS)==QHYCCD_SUCCESS) p.camera.formats.push_back(PixelFormat::Mono8);
        if (available(CAM_16BITS)==QHYCCD_SUCCESS) p.camera.formats.push_back(PixelFormat::Mono16);
        uint32_t x=0,y=0;
        check(functions().GetQHYCCDCurrentROI(handle_,&x,&y,&width,&height),"Reading QHY ROI");
        p.current.x=dimension(x); p.current.y=dimension(y); p.current.width=dimension(width); p.current.height=dimension(height);
        const double transferBits=value(CONTROL_TRANSFERBIT);
        if (transferBits!=8 && transferBits!=16) throw std::runtime_error("QHY SDK did not report an 8/16-bit transfer mode");
        p.current.format=transferBits==16?PixelFormat::Mono16:PixelFormat::Mono8;
        for (auto kind:{QhyControl::Exposure,QhyControl::Gain,QhyControl::Offset,QhyControl::UsbTraffic,QhyControl::TransferSpeed})
        {
            if (available(native(kind))!=QHYCCD_SUCCESS) continue;
            QhyControlInfo c; c.kind=kind;
            c.name=kind==QhyControl::Exposure?"Exposure":kind==QhyControl::Gain?"Gain":kind==QhyControl::Offset?"Offset":kind==QhyControl::UsbTraffic?"USB traffic":"Transfer speed";
            check(functions().GetQHYCCDParamMinMaxStep(handle_,native(kind),&c.minimum,&c.maximum,&c.step),"Reading QHY control range");
            c.value=value(native(kind));
            if (!std::isfinite(c.minimum) || !std::isfinite(c.maximum) || !std::isfinite(c.step) || c.step<=0
                || c.maximum<c.minimum || !std::isfinite(c.value))
                throw std::runtime_error("QHY control readback is outside a valid SDK range: "+c.name);
            // Some QHY cameras (e.g. QHY5LII-C) can read back a finite control slightly outside their own
            // declared [min, max] after reopen (such as Offset = 0 when min = 1); clamp to stay usable.
            c.value=std::clamp(c.value,c.minimum,c.maximum);
            p.controls.push_back(c); p.current.controls[kind]=c.value;
        }
        if (rawConfigured_ && available(CONTROL_CHANNELS)==QHYCCD_SUCCESS && value(CONTROL_CHANNELS)!=1)
            throw std::runtime_error("QHY raw mode did not read back as one channel");
        return p;
    }
    void configureRaw(const QhySettings& s) override
    {
        if (s.bin!=1) throw std::invalid_argument("QHY binning beyond 1 awaits hardware qualification");
        const auto colour=available(CAM_COLOR);
        if (colour>=BAYER_GB && colour<=BAYER_RG)
            check(functions().SetQHYCCDDebayerOnOff(handle_,false),"Disabling QHY SDK debayer");
        for (auto control:{CONTROL_ROWNOISERE,QHYCCD_3A_AUTOEXPOSURE})
        {
            if (available(control)!=QHYCCD_SUCCESS) continue;
            set(control,0);
            if (value(control)!=0) throw std::runtime_error("QHY processing/automatic exposure could not be disabled");
        }
        check(functions().SetQHYCCDBinMode(handle_,1,1),"Setting QHY bin 1");
        check(functions().SetQHYCCDBitsMode(handle_,s.format==PixelFormat::Mono16?16:8),"Setting QHY raw transfer depth");
        for (const auto& [kind,v]:s.controls) setControl(kind,v);
        check(functions().SetQHYCCDResolution(handle_,s.x,s.y,s.width,s.height),"Setting QHY ROI");
        rawConfigured_=true;
    }
    void setControl(QhyControl kind, double v) override { set(native(kind),v); }
    size_t bufferBytes() override
    {
        const auto bytes=functions().GetQHYCCDMemLength(handle_);
        if (bytes==0 || bytes==QHYCCD_ERROR) throw std::runtime_error("QHY SDK did not return a valid buffer length");
        return bytes;
    }
    void startVideo() override { check(functions().BeginQHYCCDLive(handle_),"Starting QHY live video"); }
    QhyRead read(std::span<uint8_t> destination) override
    {
        if (destination.size()<bufferBytes()) throw std::runtime_error("QHY SDK destination is too small");
        uint32_t width=0,height=0,bits=0,channels=0;
        const auto code=functions().GetQHYCCDLiveFrame(handle_,&width,&height,&bits,&channels,destination.data());
        if (code==QHYCCD_ERROR) return {}; // SDK conflates generic error/not-ready; source deadline reports persistent failure.
        check(code,"Reading QHY live frame");
        return {true,dimension(width),dimension(height),dimension(bits),dimension(channels)};
    }
    void stopVideo() override { check(functions().StopQHYCCDLive(handle_),"Stopping QHY live video"); }
    void close() noexcept override
    {
        if (handle_) (void)functions_->CloseQHYCCD(handle_);
        handle_=nullptr;
        rawConfigured_=false;
    }
private:
    const QhyFunctions& functions()
    {
        if (functions_) return *functions_;
        // The module is kept in functions_, which resolved every export from it.
        (void)loadVendorRuntime(qhyRuntime,runtimeOverride_,installedRuntimePaths(),
            [this](void* candidate) { functions_.emplace(candidate); },loadedRuntimePath_);
        return *functions_;
    }
    void unloadRuntime() noexcept
    {
        if (!functions_) return;
        if (resources_) (void)functions_->ReleaseQHYCCDResource();
        resources_=false;
        freeVendorRuntime(functions_->module);
        functions_.reset();
        loadedRuntimePath_.clear();
    }
    void initializeResources()
    {
        if (resources_) return;
        functions().EnableQHYCCDLogFile(false);
        functions().EnableQHYCCDMessage(false);
        const auto status=functions().InitQHYCCDResource();
        if (status!=QHYCCD_SUCCESS)
            throw std::runtime_error(std::format(
                "Could not initialize the QHY SDK (0x{:08X}). Check the QHY Windows driver installation, then refresh.",status));
        resources_=true;
    }
    uint32_t available(CONTROL_ID control) { return functions().IsQHYCCDControlAvailable(handle_,control); }
    double value(CONTROL_ID control)
    {
        const auto v=functions().GetQHYCCDParam(handle_,control);
        if (!std::isfinite(v) || v==QHYCCD_ERROR || v==-1) throw std::runtime_error("Reading QHY control failed");
        return v;
    }
    void set(CONTROL_ID control,double v)
    { check(functions().SetQHYCCDParam(handle_,control,v),"Setting QHY control"); }
    QString runtimeOverride_;
    QString loadedRuntimePath_;
    std::optional<QhyFunctions> functions_;
    qhyccd_handle* handle_{nullptr};
    std::string id_;
    bool resources_{false},rawConfigured_{false};
};
}
std::shared_ptr<QhyApi> makeQhyApi() { return std::make_shared<QhySdk>(); }
#else
std::shared_ptr<QhyApi> makeQhyApi() { return {}; }
#endif
