#include "cameras/zwo_source.h"
#include "cameras/raw_camera_layout.h"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace {
ZwoCameraInfo findCamera(ZwoApi& api,int id)
{
    const auto cameras=api.cameras(); // Required SDK discovery before using an ID.
    const auto found=std::find_if(cameras.begin(),cameras.end(),[&](const auto& c){return c.id==id;});
    if (found==cameras.end()) throw std::runtime_error("Selected ASI camera is not connected; refresh the camera list");
    return *found;
}
void validate(const ZwoCameraInfo& camera,const ZwoSettings& s)
{
    if (s.bin<=0 || camera.width<=0 || camera.height<=0
        || (s.format!=PixelFormat::Mono8 && s.format!=PixelFormat::Mono16)
        || std::find(camera.bins.begin(),camera.bins.end(),s.bin)==camera.bins.end()
        || std::find(camera.formats.begin(),camera.formats.end(),s.format)==camera.formats.end()
        || s.width<=0 || s.height<=0 || s.width%8 || s.height%2 || s.x<0 || s.y<0
        || static_cast<int64_t>(s.x)+s.width>camera.width/s.bin
        || static_cast<int64_t>(s.y)+s.height>camera.height/s.bin)
        throw std::invalid_argument("Unsupported ASI format/bin or ROI: width must be a multiple of 8, height of 2, inside the binned sensor");
    if (!camera.usb3 && camera.name.find("ASI120")!=std::string::npos && (static_cast<int64_t>(s.width)*s.height)%1024)
        throw std::invalid_argument("ASI120 USB2 ROI pixel count must be a multiple of 1024");
    if (camera.bayer!=BayerPattern::None && s.bin!=1)
        throw std::invalid_argument("Colour binning needs camera-specific Bayer verification; use bin 1 for raw colour");
    if (frameBytes(s.width,s.height,s.format)>static_cast<size_t>(std::numeric_limits<long>::max()))
        throw std::invalid_argument("ASI frame exceeds the SDK buffer-size range");
}
std::string controlText(const ZwoSettings& s,ZwoControl kind,const char* units="")
{
    const auto i=s.controls.find(kind);
    return i==s.controls.end()?std::string{}:std::format("{}{}",i->second,units);
}
}

ZwoSource::ZwoSource(std::shared_ptr<ZwoApi> api,ZwoProfile profile,ZwoSettings settings)
    : api_(std::move(api)),selected_(std::move(profile)),requested_(std::move(settings)) {}
ZwoSource::~ZwoSource() { stop(); }
void ZwoSource::validateSettings(const ZwoCameraInfo& camera,const ZwoSettings& settings) { validate(camera,settings); }

ZwoProfile ZwoSource::inspect(const std::shared_ptr<ZwoApi>& api,int id)
{
    if (!api) throw std::runtime_error("ZWO SDK is not enabled in this build");
    ZwoProfile p; p.camera=findCamera(*api,id); p.sdkVersion=api->version();
    api->open(id);
    try
    {
        api->initialize(id); p.serial=api->serial(id); p.controls=api->controls(id); p.current=api->roi(id);
        for (const auto& c:p.controls) p.current.controls[c.kind]=c.value;
        api->close(id); return p;
    }
    catch (...) { api->close(id); throw; }
}

bool ZwoSource::open(std::string& error)
{
    stop();
    try
    {
        if (!api_) throw std::runtime_error("ZWO SDK is not enabled in this build");
        actual_.camera=findCamera(*api_,selected_.camera.id);
        if (actual_.camera.name!=selected_.camera.name) throw std::runtime_error("Selected ASI camera identity changed; refresh the camera list");
        validate(actual_.camera,requested_);
        const int id=actual_.camera.id;
        api_->open(id); opened_=true; api_->initialize(id);
        actual_.serial=api_->serial(id); actual_.sdkVersion=api_->version();
        if (!selected_.serial.empty() && actual_.serial!=selected_.serial)
            throw std::runtime_error("Selected ASI serial number changed; refresh the camera list");
        api_->disableDarkSubtract(id);
        if (actual_.camera.triggerCamera) api_->normalMode(id);
        const auto caps=api_->controls(id);
        for (const auto& c:caps)
        {
            const auto wanted=requested_.controls.find(c.kind);
            if (c.writable && (wanted!=requested_.controls.end() || c.kind==ZwoControl::Flip))
            {
                const auto value=c.kind==ZwoControl::Flip?0:wanted->second;
                if (value<c.minimum || value>c.maximum) throw std::invalid_argument("ASI control outside reported range: "+c.name);
                api_->setControl(id,c.kind,value);
            }
        }
        for (const auto& [kind,value]:requested_.controls)
        {
            const auto found=std::find_if(caps.begin(),caps.end(),[&](const auto& c){return c.kind==kind;});
            if (found==caps.end() || (!found->writable && found->value!=value)) throw std::invalid_argument("Requested ASI control is unavailable or read only");
        }
        api_->setRoi(id,requested_);
        actual_.current=api_->roi(id); validate(actual_.camera,actual_.current);
        actual_.controls=api_->controls(id);
        for (const auto& c:actual_.controls)
        {
            if (c.automatic) throw std::runtime_error("ASI automatic control remains enabled; a fixed evidence settings snapshot is required");
            actual_.current.controls[c.kind]=c.value;
            if (c.kind==ZwoControl::Flip && c.value!=0) throw std::runtime_error("ASI flip could not be disabled for native sample orientation");
        }
        const auto& s=actual_.current;
        info_={}; info_.source=actual_.camera.name; info_.camera=actual_.camera.name;
        info_.serial=actual_.serial; info_.sdkVersion=actual_.sdkVersion;
        info_.exposure=controlText(s,ZwoControl::Exposure," us"); info_.gain=controlText(s,ZwoControl::Gain);
        info_.offset=controlText(s,ZwoControl::Offset); info_.usbBandwidth=controlText(s,ZwoControl::Bandwidth);
        info_.highSpeed=controlText(s,ZwoControl::HighSpeed); info_.binning=std::to_string(s.bin);
        info_.roi=std::format("x={} y={} width={} height={} in output pixels after binning",s.x,s.y,s.width,s.height);
        info_.sensorBits=actual_.camera.sensorBits>0?std::to_string(actual_.camera.sensorBits):"";
        info_.sampleLayout=s.format==PixelFormat::Mono16
            ? "RAW16 delivered words treated as 16 bits, low aligned; sensor ADC packing unverified; no bits removed or rescaled"
            : "RAW8 delivered bytes; no rescaling";
        info_.frameTime="host UTC immediately after ASIGetVideoData returns; includes readout/USB/buffering latency, not exposure start; host steady clock sampled alongside UTC";
        info_.frameIndex="host delivery ordinal plus observed SDK drop total; derived, not hardware; gap attribution approximate because the SDK buffers frames";
        info_.conversion="none; raw mono/Bayer; SDK dark subtraction and flip disabled";
        return true;
    }
    catch (const std::exception& ex) { error=ex.what(); release(); return false; }
}
StreamInfo ZwoSource::streamInfo() const { return info_; }
const ZwoProfile& ZwoSource::actualProfile() const { return actual_; }
void ZwoSource::start(FrameSourceListener& listener)
{
    if (!opened_ || worker_.joinable()) throw std::logic_error("ASI source must be opened once before start");
    worker_=std::jthread([this,&listener](std::stop_token stop){run(stop,listener);});
}
void ZwoSource::stop()
{
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    release();
}
void ZwoSource::release() noexcept
{
    if (capturing_) { api_->stopVideo(actual_.camera.id); capturing_=false; }
    if (opened_) { api_->close(actual_.camera.id); opened_=false; }
}
void ZwoSource::run(std::stop_token stop,FrameSourceListener& listener)
{
    try
    {
        const int id=actual_.camera.id; const auto s=actual_.current;
        api_->startVideo(id); capturing_=true;
        uint64_t delivered=0; int previousDrops=0;
        auto lastFrame=std::chrono::steady_clock::now();
        const auto exposure=s.controls.find(ZwoControl::Exposure);
        const double exposureSeconds=exposure==s.controls.end()?1:exposure->second/1e6;
        while (!stop.stop_requested())
        {
            auto f=pool_.acquire(frameBytes(s.width,s.height,s.format));
            if (!api_->read(id,std::span<uint8_t>(f->data.data(),f->data.size()),100))
            {
                if (std::chrono::duration<double>(std::chrono::steady_clock::now()-lastFrame).count()>2*exposureSeconds+5)
                    throw std::runtime_error("ASI produced no frame within twice the exposure plus five seconds");
                continue;
            }
            f->timestamp=std::chrono::system_clock::now(); f->steadyTimestamp=std::chrono::steady_clock::now(); lastFrame=f->steadyTimestamp;
            f->width=s.width; f->height=s.height; f->format=s.format;
            f->significantBits=s.format==PixelFormat::Mono16?16:8; f->alignment=BitAlignment::Low;
            f->bayer=roiBayerPattern(actual_.camera.bayer,s.x,s.y);
            f->testSource=actual_.camera.testSource; f->syntheticFlash=false; f->blip.reset(); f->sourceDroppedFrames.reset();
            std::string telemetryError;
            try
            {
                const int drops=api_->dropped(id);
                if (drops<previousDrops) throw std::runtime_error("ASI drop counter moved backwards; loss attribution is unknown");
                previousDrops=drops; f->sourceDroppedFrames=static_cast<uint64_t>(drops);
            }
            catch (const std::exception& ex) { telemetryError=ex.what(); }
            f->frameIndex=delivered++ +static_cast<uint64_t>(previousDrops);
            listener.onFrame(std::move(f)); // Preserve a received frame even if its telemetry failed.
            if (!telemetryError.empty()) throw std::runtime_error(telemetryError);
        }
    }
    catch (const std::exception& ex) { listener.onFailed(ex.what()); }
    catch (...) { listener.onFailed("Unexpected ASI acquisition failure"); }
    release();
}
