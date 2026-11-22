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
double exposureSeconds(const QhySettings& settings)
{
    const auto found=settings.controls.find(QhyControl::Exposure);
    return found==settings.controls.end()?1.0:found->second/1e6;
}
void validateControls(const QhyProfile& profile, const std::map<QhyControl,double>& controls)
{
    for (const auto& [kind,value]:controls)
    {
        const auto cap=std::find_if(profile.controls.begin(),profile.controls.end(),
            [kind](const auto& control) { return control.kind==kind; });
        if (cap==profile.controls.end() || !std::isfinite(value) || value<cap->minimum || value>cap->maximum)
            throw std::invalid_argument("Requested QHY control is unavailable or outside its range");
    }
}
StreamInfo describeStream(const QhyProfile& profile)
{
    const auto& s=profile.current;
    StreamInfo info;
    info.source=profile.camera.device.name;
    info.camera=info.source;
    info.deviceId=profile.camera.device.id;
    info.sdkVersion=profile.sdkVersion;
    info.exposure=controlText(s,QhyControl::Exposure," us");
    info.gain=controlText(s,QhyControl::Gain);
    info.offset=controlText(s,QhyControl::Offset);
    info.usbBandwidth=controlText(s,QhyControl::UsbTraffic," (QHY USB traffic units)");
    info.transferSpeed=controlText(s,QhyControl::TransferSpeed," (QHY transfer-speed units)");
    info.binning="1 commanded; no independent SDK bin getter; delivered geometry checked";
    info.roi=std::format("x={} y={} width={} height={} at bin 1, SDK readback",s.x,s.y,s.width,s.height);
    info.sensorBits=profile.camera.sensorBits>0?std::to_string(profile.camera.sensorBits):"";
    info.sampleLayout=s.format==PixelFormat::Mono16
        ?"RAW16 full delivered words interpreted as 16 bits low; ADC packing unverified; no rescale or bits removed"
        :"RAW8 delivered bytes; no rescaling";
    info.frameTime="host UTC immediately after QHY live read; host steady sampled alongside UTC; not exposure start; SDK/USB latency unknown";
    info.frameIndex="host delivery ordinal only; hardware counter not decoded; dropped frames unknown; arrival intervals do not establish losses";
    info.conversion="none; raw mono/Bayer, SDK debayer disabled; SDK buffer tail excluded from image payload; hardware qualification pending";
    return info;
}
size_t checkedBufferBytes(QhyApi& api, const QhySettings& settings)
{
    const auto bytes=api.bufferBytes();
    if (bytes<frameBytes(settings.width,settings.height,settings.format))
        throw std::runtime_error("QHY SDK buffer length is smaller than its native image");
    return bytes;
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
    if (!api) throw std::runtime_error("QHY support is not enabled in this build");
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
        if (!api_) throw std::runtime_error("QHY support is not enabled in this build");
        findDevice(*api_,selected_.camera.device.id);
        api_->open(selected_.camera.device.id); opened_=true; api_->initializeLive();
        const auto initial=api_->profile();
        if (initial.camera.device.id!=selected_.camera.device.id || initial.camera.device.name!=selected_.camera.device.name)
            throw std::runtime_error("QHY camera identity changed; refresh its profile");
        validateSettings(initial.camera,requested_);
        validateControls(initial,requested_.controls);
        api_->configureRaw(requested_); actual_=api_->profile();
        validateSettings(actual_.camera,actual_.current);
        bufferBytes_=checkedBufferBytes(*api_,actual_.current);
        publishProfile();
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
    std::lock_guard lock(settingsMutex_);
    liveRequest_.reset();
}
void QhySource::release() noexcept
{
    if (capturing_)
    {
        try { api_->stopVideo(); } catch (...) {} // Still close the device if a failed read or unplug prevented StopLive.
        capturing_=false;
    }
    if (opened_) { api_->close(); opened_=false; }
}
void QhySource::publishProfile()
{
    std::lock_guard lock(settingsMutex_);
    profileSnapshot_=actual_;
}
StreamInfo QhySource::streamInfo() const { return describeStream(actualProfile()); }
QhyProfile QhySource::actualProfile() const
{
    std::lock_guard lock(settingsMutex_);
    return profileSnapshot_;
}
void QhySource::requestLiveControls(std::map<QhyControl,double> controls)
{
    if (!worker_.joinable()) throw std::logic_error("Start Preview before changing QHY live controls");
    std::lock_guard lock(settingsMutex_);
    validateControls(profileSnapshot_,controls);
    liveRequest_=std::move(controls);
}
std::optional<double> QhySource::applyLiveControls(FrameSourceListener& listener)
{
    std::optional<std::map<QhyControl,double>> request;
    {
        std::lock_guard lock(settingsMutex_);
        request.swap(liveRequest_);
    }
    if (!request) return {};
    validateControls(actual_,*request);
    const auto& previous=actual_.current;
    const auto previousExposure=exposureSeconds(previous);
    bool changed=false;
    bool restartVideo=false;
    for (const auto& [kind,value]:*request)
    {
        if (previous.controls.at(kind)==value) continue;
        api_->setControl(kind,value);
        changed=true;
        // QHY's SDK guide recommends restarting long exposures in live
        // mode. Use one second as the cutoff, including changes from long to short.
        if (kind==QhyControl::Exposure)
            restartVideo=std::max(previousExposure,value/1e6)>=1.0;
    }
    if (!changed) return {};
    if (restartVideo)
    {
        api_->stopVideo();
        capturing_=false;
        api_->startVideo();
        capturing_=true;
    }
    auto next=api_->profile();
    const auto& s=next.current;
    if (s.width!=previous.width || s.height!=previous.height || s.x!=previous.x || s.y!=previous.y
        || s.bin!=previous.bin || s.format!=previous.format || next.camera.bayer!=actual_.camera.bayer)
        throw std::runtime_error("QHY frame layout changed during live tuning; restart Preview");
    bufferBytes_=checkedBufferBytes(*api_,s);
    const auto longestExposure=std::max(previousExposure,exposureSeconds(s));
    actual_=std::move(next);
    publishProfile();
    // The SDK has no per-frame settings identity. This estimate is used
    // only for Preview feedback. Monitoring always starts with a fresh open.
    listener.onSettingsChanged(describeStream(actual_),std::chrono::duration<double>{2*longestExposure+1});
    return 2*longestExposure+5;
}
void QhySource::run(std::stop_token stop,FrameSourceListener& listener)
{
    try
    {
        api_->startVideo(); capturing_=true;
        const auto s=actual_.current;
        const auto bayer=roiBayerPattern(actual_.camera.bayer,s.x,s.y);
        const auto pixelBytes=frameBytes(s.width,s.height,s.format);
        auto last=std::chrono::steady_clock::now(); uint64_t delivered=0;
        std::chrono::steady_clock::time_point controlDeadline{};
        while (!stop.stop_requested())
        {
            if (const auto allowance=applyLiveControls(listener))
            {
                last=std::chrono::steady_clock::now();
                controlDeadline=std::max(controlDeadline,last+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>{*allowance}));
            }
            auto f=pool_.acquire(bufferBytes_);
            const auto result=api_->read(std::span<uint8_t>(f->data.data(),f->data.size()));
            if (!result.ready)
            {
                listener.onIdle();
                const auto now=std::chrono::steady_clock::now();
                if (now>=controlDeadline && std::chrono::duration<double>(now-last).count()>2*exposureSeconds(actual_.current)+5)
                    throw std::runtime_error("QHY returned no frame within twice its exposure plus five seconds. Check the camera connection.");
                std::this_thread::sleep_for(std::chrono::milliseconds{2}); continue;
            }
            f->timestamp=std::chrono::system_clock::now(); f->steadyTimestamp=std::chrono::steady_clock::now(); last=f->steadyTimestamp;
            if (result.width!=s.width || result.height!=s.height || result.channels!=1
                || result.bits!=(s.format==PixelFormat::Mono16?16:8))
                throw std::runtime_error("QHY delivered an unexpected geometry/depth/channel count; frame not published");
            f->width=s.width; f->height=s.height; f->format=s.format;
            f->significantBits=result.bits; f->alignment=BitAlignment::Low;
            f->bayer=bayer;
            f->frameIndex=delivered++; f->sourceDroppedFrames.reset();
            f->testSource=actual_.camera.testSource; f->syntheticFlash=false; f->blip.reset();
            f->data.resize(pixelBytes); // Capacity/SDK padding stays charged until final release.
            listener.onFrame(std::move(f));
        }
    }
    catch (const std::exception& ex) { listener.onFailed(ex.what()); }
    catch (...) { listener.onFailed("Unexpected QHY acquisition failure"); }
    release();
}
