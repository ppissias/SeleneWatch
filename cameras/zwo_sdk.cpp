#include "cameras/zwo_api.h"

#ifdef SELENEWATCH_WITH_ZWO
// The vendor header marks declarations dllexport under _WINDOWS. Consumers
// link its import library; suppress that export marker without changing the SDK.
#ifdef _WINDOWS
#pragma push_macro("_WINDOWS")
#undef _WINDOWS
#define SELENEWATCH_RESTORE_WINDOWS
#endif
#include <ASICamera2.h>
#ifdef SELENEWATCH_RESTORE_WINDOWS
#pragma pop_macro("_WINDOWS")
#undef SELENEWATCH_RESTORE_WINDOWS
#endif

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace {
void check(ASI_ERROR_CODE code,const char* operation)
{
    if (code==ASI_SUCCESS) return;
    const char* reason=code==ASI_ERROR_CAMERA_REMOVED?"camera disconnected":
        code==ASI_ERROR_INVALID_ID?"camera ID unavailable":code==ASI_ERROR_CAMERA_CLOSED?"camera closed":
        code==ASI_ERROR_INVALID_SIZE?"ROI size rejected":code==ASI_ERROR_OUTOF_BOUNDARY?"ROI origin rejected":
        code==ASI_ERROR_INVALID_IMGTYPE?"format rejected":code==ASI_ERROR_TIMEOUT?"timed out":"SDK operation failed";
    throw std::runtime_error(std::format("{}: {} (ASI status {})",operation,reason,static_cast<int>(code)));
}
ASI_CONTROL_TYPE native(ZwoControl c)
{
    switch (c)
    {
    case ZwoControl::Exposure:return ASI_EXPOSURE;
    case ZwoControl::Gain:return ASI_GAIN;
    case ZwoControl::Offset:return ASI_OFFSET;
    case ZwoControl::Bandwidth:return ASI_BANDWIDTHOVERLOAD;
    case ZwoControl::HighSpeed:return ASI_HIGH_SPEED_MODE;
    case ZwoControl::Flip:return ASI_FLIP;
    }
    throw std::invalid_argument("Unknown ASI control");
}
PixelFormat format(ASI_IMG_TYPE type)
{
    if (type==ASI_IMG_RAW8) return PixelFormat::Mono8;
    if (type==ASI_IMG_RAW16) return PixelFormat::Mono16;
    return PixelFormat::RGB888; // An inspected processed mode, never accepted for capture.
}
// Maps ASI functions and status codes into the application's raw camera API.
// ZwoSource manages the open and streaming lifetime through this adapter.
class ZwoSdk final : public ZwoApi
{
public:
    std::string version() const override { const auto v=ASIGetSDKVersion(); return v?v:""; }
    std::vector<ZwoCameraInfo> cameras() override
    {
        const int count=ASIGetNumOfConnectedCameras(); std::vector<ZwoCameraInfo> result;
        for (int i=0;i<count;++i)
        {
            ASI_CAMERA_INFO info{}; check(ASIGetCameraProperty(&info,i),"Reading ASI camera information");
            ZwoCameraInfo c; c.id=info.CameraID; c.name=info.Name;
            c.width=static_cast<int>(info.MaxWidth); c.height=static_cast<int>(info.MaxHeight); c.sensorBits=info.BitDepth;
            c.usb3=info.IsUSB3Camera==ASI_TRUE; c.triggerCamera=info.IsTriggerCam==ASI_TRUE;
            if (info.IsColorCam)
            {
                switch (info.BayerPattern)
                {
                case ASI_BAYER_RG:c.bayer=BayerPattern::RGGB;break;
                case ASI_BAYER_GR:c.bayer=BayerPattern::GRBG;break;
                case ASI_BAYER_GB:c.bayer=BayerPattern::GBRG;break;
                case ASI_BAYER_BG:c.bayer=BayerPattern::BGGR;break;
                default:throw std::runtime_error("ASI colour camera reported an unknown Bayer pattern");
                }
            }
            for (const int bin:info.SupportedBins) { if (!bin) break; c.bins.push_back(bin); }
            for (const auto type:info.SupportedVideoFormat)
            {
                if (type==ASI_IMG_END) break;
                if (type==ASI_IMG_RAW8 || type==ASI_IMG_RAW16) c.formats.push_back(format(type));
            }
            result.push_back(std::move(c));
        }
        return result;
    }
    void open(int id) override { check(ASIOpenCamera(id),"Opening ASI camera"); }
    void initialize(int id) override { check(ASIInitCamera(id),"Initializing ASI camera"); }
    void close(int id) noexcept override { (void)ASICloseCamera(id); }
    std::string serial(int id) override
    {
        ASI_SN sn{};
        if (ASIGetSerialNumber(id,&sn)!=ASI_SUCCESS) return {};
        std::string result; bool nonzero=false;
        for (unsigned char byte:sn.id) { result+=std::format("{:02X}",byte); nonzero=nonzero || byte!=0; }
        return nonzero?result:std::string{};
    }
    std::vector<ZwoControlInfo> controls(int id) override
    {
        int count=0; check(ASIGetNumOfControls(id,&count),"Reading ASI control count");
        std::vector<ZwoControlInfo> result;
        for (int i=0;i<count;++i)
        {
            ASI_CONTROL_CAPS caps{}; check(ASIGetControlCaps(id,i,&caps),"Reading ASI control capabilities");
            for (auto kind:{ZwoControl::Exposure,ZwoControl::Gain,ZwoControl::Offset,ZwoControl::Bandwidth,ZwoControl::HighSpeed,ZwoControl::Flip})
            {
                if (caps.ControlType!=native(kind)) continue;
                long value=0; ASI_BOOL automatic=ASI_FALSE;
                check(ASIGetControlValue(id,caps.ControlType,&value,&automatic),"Reading ASI control value");
                result.push_back({kind,caps.Name,caps.MinValue,caps.MaxValue,value,caps.IsWritable==ASI_TRUE,automatic==ASI_TRUE});
            }
        }
        return result;
    }
    void setControl(int id,ZwoControl kind,int64_t value) override
    {
        if (value<std::numeric_limits<long>::min() || value>std::numeric_limits<long>::max()) throw std::invalid_argument("ASI control exceeds SDK integer range");
        check(ASISetControlValue(id,native(kind),static_cast<long>(value),ASI_FALSE),"Setting ASI manual control");
    }
    ZwoSettings roi(int id) override
    {
        ZwoSettings s; ASI_IMG_TYPE type=ASI_IMG_END;
        check(ASIGetROIFormat(id,&s.width,&s.height,&s.bin,&type),"Reading ASI ROI");
        check(ASIGetStartPos(id,&s.x,&s.y),"Reading ASI ROI origin"); s.format=format(type); return s;
    }
    void setRoi(int id,const ZwoSettings& s) override
    {
        const auto type=s.format==PixelFormat::Mono8?ASI_IMG_RAW8:ASI_IMG_RAW16;
        check(ASISetROIFormat(id,s.width,s.height,s.bin,type),"Setting ASI raw ROI");
        check(ASISetStartPos(id,s.x,s.y),"Setting ASI ROI origin");
    }
    void disableDarkSubtract(int id) override { check(ASIDisableDarkSubtract(id),"Disabling ASI dark subtraction"); }
    void normalMode(int id) override { check(ASISetCameraMode(id,ASI_MODE_NORMAL),"Setting ASI normal video mode"); }
    void startVideo(int id) override { check(ASIStartVideoCapture(id),"Starting ASI video"); }
    void stopVideo(int id) noexcept override { (void)ASIStopVideoCapture(id); }
    bool read(int id,std::span<uint8_t> data,int waitMs) override
    {
        if (data.size()>static_cast<size_t>(std::numeric_limits<long>::max())) throw std::invalid_argument("ASI frame buffer too large");
        const auto status=ASIGetVideoData(id,data.data(),static_cast<long>(data.size()),waitMs);
        if (status==ASI_ERROR_TIMEOUT) return false;
        check(status,"Reading ASI frame"); return true;
    }
    int dropped(int id) override
    {
        int count=0; check(ASIGetDroppedFrames(id,&count),"Reading ASI dropped-frame count"); return count;
    }
};
}
std::shared_ptr<ZwoApi> makeZwoApi() { return std::make_shared<ZwoSdk>(); }
#else
std::shared_ptr<ZwoApi> makeZwoApi() { return {}; }
#endif
