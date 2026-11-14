#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cameras/qhy_api.h"

#ifdef SELENEWATCH_WITH_QHY
#include <QtCore/QString>
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

// Development-only binding. The user-installed DLL remains outside the app
// directory and repository. Construction neither loads it nor touches USB.
class QhySdk final : public QhyApi
{
public:
    ~QhySdk() override
    {
        close();
        if (resources_)
        {
            try { (void)function<decltype(&ReleaseQHYCCDResource)>("ReleaseQHYCCDResource")(); } catch (...) {}
        }
        if (library_) FreeLibrary(library_);
    }
    std::string version() override
    {
        uint32_t year=0,month=0,day=0,subday=0;
        check(function<decltype(&GetQHYCCDSDKVersion)>("GetQHYCCDSDKVersion")(&year,&month,&day,&subday),"Reading QHY SDK version");
        return std::format("{}.{}.{}.{}",year,month,day,subday);
    }
    std::vector<QhyDevice> devices() override
    {
        initializeResources();
        const auto count=function<decltype(&ScanQHYCCD)>("ScanQHYCCD")();
        if (count>1024) throw std::runtime_error(std::format("QHY scan failed (status/count 0x{:08X})",count));
        std::vector<QhyDevice> result;
        for (uint32_t i=0;i<count;++i)
        {
            std::array<char,256> id{},model{};
            check(function<decltype(&GetQHYCCDId)>("GetQHYCCDId")(i,id.data()),"Reading QHY camera ID");
            check(function<decltype(&GetQHYCCDModel)>("GetQHYCCDModel")(id.data(),model.data()),"Reading QHY camera model");
            result.push_back({text(id),text(model)});
        }
        return result;
    }
    void open(const std::string& id) override
    {
        if (handle_) throw std::logic_error("A QHY connection is already open");
        initializeResources(); id_=id;
        handle_=function<decltype(&OpenQHYCCD)>("OpenQHYCCD")(id_.data());
        if (!handle_) throw std::runtime_error("Opening QHY camera failed");
    }
    void initializeLive() override
    {
        check(function<decltype(&SetQHYCCDStreamMode)>("SetQHYCCDStreamMode")(handle_,1),"Setting QHY live mode");
        check(function<decltype(&InitQHYCCD)>("InitQHYCCD")(handle_),"Initializing QHY camera");
        rawConfigured_=false;
    }
    QhyProfile profile() override
    {
        QhyProfile p; p.camera.device.id=id_; p.sdkVersion=version();
        std::array<char,256> model{};
        check(function<decltype(&GetQHYCCDModel)>("GetQHYCCDModel")(id_.data(),model.data()),"Reading QHY model");
        p.camera.device.name=text(model);
        uint32_t width=0,height=0,bits=0; double cw=0,ch=0,pw=0,ph=0;
        check(function<decltype(&GetQHYCCDChipInfo)>("GetQHYCCDChipInfo")(handle_,&cw,&ch,&width,&height,&pw,&ph,&bits),"Reading QHY sensor geometry");
        p.camera.width=dimension(width); p.camera.height=dimension(height); p.camera.sensorBits=dimension(bits);
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
        check(function<decltype(&GetQHYCCDCurrentROI)>("GetQHYCCDCurrentROI")(handle_,&x,&y,&width,&height),"Reading QHY ROI");
        p.current.x=dimension(x); p.current.y=dimension(y); p.current.width=dimension(width); p.current.height=dimension(height);
        const double transferBits=value(CONTROL_TRANSFERBIT);
        if (transferBits!=8 && transferBits!=16) throw std::runtime_error("QHY SDK did not report an 8/16-bit transfer mode");
        p.current.format=transferBits==16?PixelFormat::Mono16:PixelFormat::Mono8;
        for (auto kind:{QhyControl::Exposure,QhyControl::Gain,QhyControl::Offset,QhyControl::UsbTraffic,QhyControl::TransferSpeed})
        {
            if (available(native(kind))!=QHYCCD_SUCCESS) continue;
            QhyControlInfo c; c.kind=kind;
            c.name=kind==QhyControl::Exposure?"Exposure":kind==QhyControl::Gain?"Gain":kind==QhyControl::Offset?"Offset":kind==QhyControl::UsbTraffic?"USB traffic":"Transfer speed";
            check(function<decltype(&GetQHYCCDParamMinMaxStep)>("GetQHYCCDParamMinMaxStep")(handle_,native(kind),&c.minimum,&c.maximum,&c.step),"Reading QHY control range");
            c.value=value(native(kind));
            if (!std::isfinite(c.minimum) || !std::isfinite(c.maximum) || !std::isfinite(c.step) || c.step<=0
                || c.maximum<c.minimum || c.value<c.minimum || c.value>c.maximum)
                throw std::runtime_error("QHY control readback is outside a valid SDK range: "+c.name);
            p.controls.push_back(c); p.current.controls[kind]=c.value;
        }
        if (rawConfigured_ && available(CONTROL_CHANNELS)==QHYCCD_SUCCESS && value(CONTROL_CHANNELS)!=1)
            throw std::runtime_error("QHY raw mode did not read back as one channel");
        return p;
    }
    void configureRaw(const QhySettings& s) override
    {
        if (s.bin!=1) throw std::invalid_argument("QHY development binding only qualifies bin 1");
        const auto colour=available(CAM_COLOR);
        if (colour>=BAYER_GB && colour<=BAYER_RG)
            check(function<decltype(&SetQHYCCDDebayerOnOff)>("SetQHYCCDDebayerOnOff")(handle_,false),"Disabling QHY SDK debayer");
        for (auto control:{CONTROL_ROWNOISERE,QHYCCD_3A_AUTOEXPOSURE})
        {
            if (available(control)!=QHYCCD_SUCCESS) continue;
            set(control,0);
            if (value(control)!=0) throw std::runtime_error("QHY processing/automatic exposure could not be disabled");
        }
        check(function<decltype(&SetQHYCCDBinMode)>("SetQHYCCDBinMode")(handle_,1,1),"Setting QHY bin 1");
        check(function<decltype(&SetQHYCCDBitsMode)>("SetQHYCCDBitsMode")(handle_,s.format==PixelFormat::Mono16?16:8),"Setting QHY raw transfer depth");
        for (const auto& [kind,v]:s.controls) set(native(kind),v);
        check(function<decltype(&SetQHYCCDResolution)>("SetQHYCCDResolution")(handle_,s.x,s.y,s.width,s.height),"Setting QHY ROI");
        rawConfigured_=true;
    }
    size_t bufferBytes() override
    {
        const auto bytes=function<decltype(&GetQHYCCDMemLength)>("GetQHYCCDMemLength")(handle_);
        if (bytes==0 || bytes==QHYCCD_ERROR) throw std::runtime_error("QHY SDK did not return a valid buffer length");
        return bytes;
    }
    void startVideo() override { check(function<decltype(&BeginQHYCCDLive)>("BeginQHYCCDLive")(handle_),"Starting QHY live video"); }
    QhyRead read(std::span<uint8_t> destination) override
    {
        if (destination.size()<bufferBytes()) throw std::runtime_error("QHY SDK destination is too small");
        uint32_t width=0,height=0,bits=0,channels=0;
        const auto code=function<decltype(&GetQHYCCDLiveFrame)>("GetQHYCCDLiveFrame")(handle_,&width,&height,&bits,&channels,destination.data());
        if (code==QHYCCD_ERROR) return {}; // SDK conflates generic error/not-ready; source deadline reports persistent failure.
        check(code,"Reading QHY live frame");
        return {true,dimension(width),dimension(height),dimension(bits),dimension(channels)};
    }
    void stopVideo() noexcept override
    {
        try { if (handle_) (void)function<decltype(&StopQHYCCDLive)>("StopQHYCCDLive")(handle_); } catch (...) {}
    }
    void close() noexcept override
    {
        try { if (handle_) (void)function<decltype(&CloseQHYCCD)>("CloseQHYCCD")(handle_); } catch (...) {}
        handle_=nullptr; rawConfigured_=false;
    }
private:
    template<class Function> Function function(const char* name)
    {
        if (!library_)
        {
            const auto path=QString::fromUtf8(SELENEWATCH_QHY_DLL).toStdWString();
            // The inspected DLL imports Windows components only. Never stage
            // old SDK-bundled CRT/USB DLLs into the Qt application's directory.
            library_=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library_) throw std::runtime_error(std::format("Cannot load installed QHY development DLL (Windows error {})",GetLastError()));
        }
        const auto pointer=GetProcAddress(library_,name);
        if (!pointer) throw std::runtime_error(std::string("Installed QHY SDK is missing ")+name);
        return reinterpret_cast<Function>(pointer);
    }
    void initializeResources()
    {
        if (resources_) return;
        function<decltype(&EnableQHYCCDLogFile)>("EnableQHYCCDLogFile")(false);
        function<decltype(&EnableQHYCCDMessage)>("EnableQHYCCDMessage")(false);
        check(function<decltype(&InitQHYCCDResource)>("InitQHYCCDResource")(),"Initializing QHY SDK resources");
        resources_=true;
    }
    uint32_t available(CONTROL_ID control) { return function<decltype(&IsQHYCCDControlAvailable)>("IsQHYCCDControlAvailable")(handle_,control); }
    double value(CONTROL_ID control)
    {
        const auto v=function<decltype(&GetQHYCCDParam)>("GetQHYCCDParam")(handle_,control);
        if (!std::isfinite(v) || v==QHYCCD_ERROR || v==-1) throw std::runtime_error("Reading QHY control failed");
        return v;
    }
    void set(CONTROL_ID control,double v)
    { check(function<decltype(&SetQHYCCDParam)>("SetQHYCCDParam")(handle_,control,v),"Setting QHY control"); }
    HMODULE library_{nullptr};
    qhyccd_handle* handle_{nullptr};
    std::string id_;
    bool resources_{false},rawConfigured_{false};
};
}
std::shared_ptr<QhyApi> makeQhyApi() { return std::make_shared<QhySdk>(); }
#else
std::shared_ptr<QhyApi> makeQhyApi() { return {}; }
#endif
