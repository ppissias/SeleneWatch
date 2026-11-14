#include "cameras/qhy_source.h"
#include "cameras/raw_camera_layout.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace {
void findDevice(QhyApi& api, const std::string& id)
{
    const auto devices=api.devices();
    if (std::none_of(devices.begin(),devices.end(),[&](const auto& d){return d.id==id;}))
        throw std::runtime_error("Selected QHY camera is not connected; refresh the device list");
}
std::string controlText(const QhySettings& settings, QhyControl kind, const char* units="")
{
    const auto found=settings.controls.find(kind);
    return found==settings.controls.end()?std::string{}:std::format("{}{}",found->second,units);
}
}

QhySource::QhySource(std::shared_ptr<QhyApi> api,QhyProfile selected,QhySettings settings)
    : api_(std::move(api)),selected_(std::move(selected)),requested_(std::move(settings)) {}
QhySource::~QhySource() { stop(); }

void QhySource::validateSettings(const QhyCameraInfo& camera,const QhySettings& s)
{
    if (s.bin!=1) throw std::invalid_argument("QHY binning beyond 1 awaits hardware and readback qualification");
    if (camera.width<=0 || camera.height<=0 || s.width<=0 || s.height<=0 || s.x<0 || s.y<0
        || static_cast<int64_t>(s.x)+s.width>camera.width || static_cast<int64_t>(s.y)+s.height>camera.height
        || (s.format!=PixelFormat::Mono8 && s.format!=PixelFormat::Mono16)
        || std::find(camera.formats.begin(),camera.formats.end(),s.format)==camera.formats.end())
        throw std::invalid_argument("QHY raw format or ROI is unsupported/outside the sensor");
    for (const auto& [kind,value]:s.controls)
    {
        (void)kind;
        if (!std::isfinite(value)) throw std::invalid_argument("QHY control must be finite");
    }
}
QhyProfile QhySource::inspect(const std::shared_ptr<QhyApi>& api,const std::string& id)
{
    if (!api) throw std::runtime_error("QHY development binding is not configured");
    findDevice(*api,id); api->open(id);
    try
    {
        api->initializeLive(); auto result=api->profile(); api->close(); return result;
    }
    catch (...) { api->close(); throw; }
}
bool QhySource::open(std::string& error)
{
    stop();
    try
    {
        if (!api_) throw std::runtime_error("QHY development binding is not configured");
        findDevice(*api_,selected_.camera.device.id);
        api_->open(selected_.camera.device.id); opened_=true; api_->initializeLive();
        const auto initial=api_->profile();
        if (initial.camera.device.id!=selected_.camera.device.id || initial.camera.device.name!=selected_.camera.device.name)
            throw std::runtime_error("QHY camera identity changed; refresh its profile");
        validateSettings(initial.camera,requested_);
        for (const auto& [kind,value]:requested_.controls)
        {
            const auto cap=std::find_if(initial.controls.begin(),initial.controls.end(),[&](const auto& c){return c.kind==kind;});
            if (cap==initial.controls.end() || value<cap->minimum || value>cap->maximum)
                throw std::invalid_argument("Requested QHY control is unavailable or outside its range");
        }
        api_->configureRaw(requested_); actual_=api_->profile();
        validateSettings(actual_.camera,actual_.current);
        bufferBytes_=api_->bufferBytes();
        if (bufferBytes_<frameBytes(actual_.current.width,actual_.current.height,actual_.current.format))
            throw std::runtime_error("QHY SDK buffer length is smaller than its native image");
        const auto& s=actual_.current;
        info_={}; info_.source=actual_.camera.device.name; info_.camera=info_.source;
        info_.deviceId=actual_.camera.device.id; // Opaque SDK ID; not asserted to be a serial number.
        info_.sdkVersion=actual_.sdkVersion;
        info_.exposure=controlText(s,QhyControl::Exposure," us"); info_.gain=controlText(s,QhyControl::Gain);
        info_.offset=controlText(s,QhyControl::Offset); info_.usbBandwidth=controlText(s,QhyControl::UsbTraffic," (QHY USB traffic units)");
        info_.transferSpeed=controlText(s,QhyControl::TransferSpeed," (QHY transfer-speed units)");
        info_.binning="1 commanded; no independent SDK bin getter; delivered geometry checked";
        info_.roi=std::format("x={} y={} width={} height={} at bin 1, SDK readback",s.x,s.y,s.width,s.height);
        info_.sensorBits=actual_.camera.sensorBits>0?std::to_string(actual_.camera.sensorBits):"";
        info_.sampleLayout=s.format==PixelFormat::Mono16
            ?"RAW16 full delivered words interpreted as 16 bits low; ADC packing unverified; no rescale or bits removed"
            :"RAW8 delivered bytes; no rescaling";
        info_.frameTime="host UTC immediately after QHY live read; host steady sampled alongside UTC; not exposure start; SDK/USB latency unknown";
        info_.frameIndex="host delivery ordinal only; hardware counter not decoded; dropped frames unknown; arrival intervals do not establish losses";
        info_.conversion="none; raw mono/Bayer, SDK debayer disabled; SDK buffer tail excluded from image payload; hardware qualification pending";
        return true;
    }
    catch (const std::exception& ex) { error=ex.what(); release(); return false; }
}
void QhySource::start(FrameSourceListener& listener)
{
    if (!opened_ || worker_.joinable()) throw std::logic_error("QHY source must be opened before start");
    worker_=std::jthread([this,&listener](std::stop_token stop){run(stop,listener);});
}
void QhySource::stop()
{
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    release();
}
void QhySource::release() noexcept
{
    if (capturing_) { api_->stopVideo(); capturing_=false; }
    if (opened_) { api_->close(); opened_=false; }
}
StreamInfo QhySource::streamInfo() const { return info_; }
const QhyProfile& QhySource::actualProfile() const { return actual_; }
void QhySource::run(std::stop_token stop,FrameSourceListener& listener)
{
    try
    {
        api_->startVideo(); capturing_=true;
        const auto& s=actual_.current;
        const auto exposure=s.controls.find(QhyControl::Exposure);
        const double exposureSeconds=exposure==s.controls.end()?1:exposure->second/1e6;
        auto last=std::chrono::steady_clock::now(); uint64_t delivered=0;
        while (!stop.stop_requested())
        {
            auto f=pool_.acquire(bufferBytes_);
            const auto result=api_->read(std::span<uint8_t>(f->data.data(),f->data.size()));
            if (!result.ready)
            {
                if (std::chrono::duration<double>(std::chrono::steady_clock::now()-last).count()>2*exposureSeconds+5)
                    throw std::runtime_error("QHY returned no frame within twice its exposure plus five seconds; loss count unknown");
                std::this_thread::sleep_for(std::chrono::milliseconds{2}); continue;
            }
            f->timestamp=std::chrono::system_clock::now(); f->steadyTimestamp=std::chrono::steady_clock::now(); last=f->steadyTimestamp;
            if (result.width!=s.width || result.height!=s.height || result.channels!=1
                || result.bits!=(s.format==PixelFormat::Mono16?16:8))
                throw std::runtime_error("QHY delivered an unexpected geometry/depth/channel count; frame not published");
            f->width=s.width; f->height=s.height; f->format=s.format;
            f->significantBits=result.bits; f->alignment=BitAlignment::Low;
            f->bayer=roiBayerPattern(actual_.camera.bayer,s.x,s.y);
            f->frameIndex=delivered++; f->sourceDroppedFrames.reset();
            f->testSource=actual_.camera.testSource; f->syntheticFlash=false; f->blip.reset();
            f->data.resize(frameBytes(s.width,s.height,s.format)); // Capacity/SDK padding stays charged until final release.
            listener.onFrame(std::move(f));
        }
    }
    catch (const std::exception& ex) { listener.onFailed(ex.what()); }
    catch (...) { listener.onFailed("Unexpected QHY acquisition failure"); }
    release();
}
