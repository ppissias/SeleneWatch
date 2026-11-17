#include "cameras/touptek_source.h"
#include "cameras/raw_camera_layout.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace {
void findDevice(ToupTekApi& api, const std::string& id)
{
    const auto devices = api.devices();
    if (std::none_of(devices.begin(), devices.end(), [&](const auto& d) { return d.id == id; }))
        throw std::runtime_error("Selected ToupTek camera is not connected; refresh the camera list");
}
std::string controlText(const ToupTekSettings& s, ToupTekControl kind, const char* units = "")
{
    const auto found = s.controls.find(kind);
    return found == s.controls.end() ? std::string{} : std::format("{}{}", found->second, units);
}
double exposureSeconds(const ToupTekSettings& s)
{
    const auto found = s.controls.find(ToupTekControl::Exposure);
    return found == s.controls.end() ? 1.0 : static_cast<double>(found->second) / 1e6;
}
void validateControls(const ToupTekProfile& profile, const std::map<ToupTekControl,int64_t>& controls)
{
    for (const auto& [kind, value] : controls)
    {
        const auto cap = std::find_if(profile.controls.begin(), profile.controls.end(),
            [kind](const auto& control) { return control.kind == kind; });
        if (cap == profile.controls.end() || value < cap->minimum || value > cap->maximum)
            throw std::invalid_argument("Requested ToupTek control is unavailable or outside its range");
    }
}
StreamInfo describeStream(const ToupTekProfile& profile)
{
    const auto& s = profile.current;
    StreamInfo info;
    info.source = profile.camera.device.name;
    info.camera = info.source;
    info.deviceId = profile.camera.device.id;
    info.serial = profile.serial;
    info.sdkVersion = profile.sdkVersion;
    info.exposure = controlText(s, ToupTekControl::Exposure, " us");
    info.gain = controlText(s, ToupTekControl::Gain, " (ToupTek gain %, 100 = unity)");
    info.offset = controlText(s, ToupTekControl::BlackLevel, " (ToupTek black level)");
    info.usbBandwidth = controlText(s, ToupTekControl::UsbBandwidth, " (ToupTek bandwidth %)");
    info.transferSpeed = controlText(s, ToupTekControl::Speed, " (ToupTek speed level)");
    info.binning = "1; digital binning off";
    info.roi = std::format("x={} y={} width={} height={} in sensor pixels, SDK readback", s.x, s.y, s.width, s.height);
    info.sensorBits = profile.camera.sensorBits > 0 ? std::to_string(profile.camera.sensorBits) : "";
    info.sampleLayout = s.format == PixelFormat::Mono16
        ? "RAW16 delivered words treated as 16 bits, low aligned; sensor ADC packing unverified; no bits removed or rescaled"
        : "RAW8 delivered bytes; no rescaling";
    info.frameTime = "host UTC immediately after the ToupTek wait for a frame returns; not exposure start; SDK/USB latency unknown";
    info.frameIndex = "camera frame sequence number counted from the first frame, when the SDK reports it; otherwise host delivery ordinal with losses unknown";
    info.conversion = "none; raw mono/Bayer; SDK upside-down, flips, automatic exposure and digital binning off; hardware qualification pending";
    return info;
}
} // namespace

ToupTekSource::ToupTekSource(std::shared_ptr<ToupTekApi> api, ToupTekProfile selected, ToupTekSettings settings)
    : api_(std::move(api)), selected_(std::move(selected)), requested_(std::move(settings)) {}
ToupTekSource::~ToupTekSource() { stop(); }

void ToupTekSource::validateSettings(const ToupTekCameraInfo& camera, const ToupTekSettings& s)
{
    if (camera.width <= 0 || camera.height <= 0 || s.width < 8 || s.height < 8 || s.x < 0 || s.y < 0
        || s.width % 2 || s.height % 2 || s.x % 2 || s.y % 2
        || static_cast<int64_t>(s.x) + s.width > camera.width || static_cast<int64_t>(s.y) + s.height > camera.height
        || (s.format != PixelFormat::Mono8 && s.format != PixelFormat::Mono16)
        || std::find(camera.formats.begin(), camera.formats.end(), s.format) == camera.formats.end())
        throw std::invalid_argument("ToupTek raw format or ROI is unsupported: even origin and size of at least 8, inside the sensor");
}

ToupTekProfile ToupTekSource::inspect(const std::shared_ptr<ToupTekApi>& api, const std::string& id)
{
    if (!api) throw std::runtime_error("ToupTek support is not enabled in this build");
    findDevice(*api, id);
    api->open(id);
    try
    {
        auto result = api->profile();
        api->close();
        return result;
    }
    catch (...) { api->close(); throw; }
}

bool ToupTekSource::open(std::string& error)
{
    stop();
    try
    {
        if (!api_) throw std::runtime_error("ToupTek support is not enabled in this build");
        findDevice(*api_, selected_.camera.device.id);
        api_->open(selected_.camera.device.id);
        opened_ = true;
        const auto initial = api_->profile();
        if (initial.camera.device.name != selected_.camera.device.name || initial.serial != selected_.serial)
            throw std::runtime_error("ToupTek camera identity changed; refresh the camera list");
        validateSettings(initial.camera, requested_);
        // Configure without the controls first: the black level's range depends on the raw depth.
        auto layout = requested_;
        layout.controls.clear();
        api_->configureRaw(layout);
        validateControls(api_->profile(), requested_.controls);
        for (const auto& [kind, value] : requested_.controls) api_->setControl(kind, value);
        actual_ = api_->profile();
        validateSettings(actual_.camera, actual_.current);
        if (actual_.current.format != requested_.format)
            throw std::runtime_error("ToupTek raw depth did not read back as requested");
        publishProfile();
        return true;
    }
    catch (const std::exception& ex) { error = ex.what(); release(); return false; }
}

void ToupTekSource::start(FrameSourceListener& listener)
{
    if (!opened_ || worker_.joinable()) throw std::logic_error("ToupTek source must be opened before start");
    worker_ = std::jthread([this, &listener](std::stop_token stop) { run(stop, listener); });
}
void ToupTekSource::stop()
{
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    release();
    std::lock_guard lock(settingsMutex_);
    liveRequest_.reset();
}
void ToupTekSource::release() noexcept
{
    if (capturing_) { api_->stopVideo(); capturing_ = false; }
    if (opened_) { api_->close(); opened_ = false; }
}
void ToupTekSource::publishProfile()
{
    std::lock_guard lock(settingsMutex_);
    profileSnapshot_ = actual_;
}
StreamInfo ToupTekSource::streamInfo() const { return describeStream(actualProfile()); }
ToupTekProfile ToupTekSource::actualProfile() const
{
    std::lock_guard lock(settingsMutex_);
    return profileSnapshot_;
}
void ToupTekSource::requestLiveControls(std::map<ToupTekControl,int64_t> controls)
{
    if (!worker_.joinable()) throw std::logic_error("Start Preview before changing ToupTek live controls");
    std::lock_guard lock(settingsMutex_);
    validateControls(profileSnapshot_, controls);
    liveRequest_ = std::move(controls);
}

std::optional<double> ToupTekSource::applyLiveControls(FrameSourceListener& listener)
{
    std::optional<std::map<ToupTekControl,int64_t>> request;
    {
        std::lock_guard lock(settingsMutex_);
        request.swap(liveRequest_);
    }
    if (!request) return {};
    const auto previous = actual_.current;
    bool changed = false;
    for (const auto& [kind, value] : *request)
    {
        if (previous.controls.at(kind) == value) continue;
        api_->setControl(kind, value);
        changed = true;
    }
    if (!changed) return {};
    auto next = api_->profile();
    const auto& s = next.current;
    if (s.width != previous.width || s.height != previous.height || s.x != previous.x || s.y != previous.y
        || s.format != previous.format || next.camera.bayer != actual_.camera.bayer)
        throw std::runtime_error("ToupTek frame layout changed during live tuning; restart Preview");
    const double longestExposure = std::max(exposureSeconds(previous), exposureSeconds(s));
    actual_ = std::move(next);
    publishProfile();
    // Frames carry no settings identity of their own; this estimate is for
    // Preview feedback only. Monitoring always starts with a fresh open.
    listener.onSettingsChanged(describeStream(actual_), std::chrono::duration<double>{2 * longestExposure + 1});
    return 2 * longestExposure + 5;
}

void ToupTekSource::run(std::stop_token stop, FrameSourceListener& listener)
{
    try
    {
        api_->startVideo();
        capturing_ = true;
        const auto s = actual_.current;
        const auto bayer = roiBayerPattern(actual_.camera.bayer, s.x, s.y);
        const auto pixelBytes = frameBytes(s.width, s.height, s.format);
        auto last = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point controlDeadline{};
        uint64_t delivered = 0;
        std::optional<uint32_t> previousSequence;
        uint64_t index = 0;
        while (!stop.stop_requested())
        {
            if (const auto allowance = applyLiveControls(listener))
            {
                last = std::chrono::steady_clock::now();
                controlDeadline = std::max(controlDeadline, last + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>{*allowance}));
            }
            auto f = pool_.acquire(pixelBytes);
            const auto result = api_->read(std::span<uint8_t>(f->data.data(), f->data.size()), 100);
            if (!result.ready)
            {
                listener.onIdle();
                const auto now = std::chrono::steady_clock::now();
                if (now >= controlDeadline && std::chrono::duration<double>(now - last).count() > 2 * exposureSeconds(actual_.current) + 5)
                    throw std::runtime_error("ToupTek returned no frame within twice its exposure plus five seconds. Check the camera connection.");
                continue;
            }
            f->timestamp = std::chrono::system_clock::now();
            f->steadyTimestamp = std::chrono::steady_clock::now();
            last = f->steadyTimestamp;
            if (result.width != s.width || result.height != s.height)
                throw std::runtime_error("ToupTek delivered an unexpected frame size; frame not published");
            f->width = s.width;
            f->height = s.height;
            f->format = s.format;
            f->significantBits = s.format == PixelFormat::Mono16 ? 16 : 8;
            f->alignment = BitAlignment::Low;
            f->bayer = bayer;
            f->testSource = actual_.camera.testSource;
            f->syntheticFlash = false;
            f->blip.reset();
            f->sourceDroppedFrames.reset();
            if (result.sequence)
            {
                // Counted from the first frame, by the step from the frame
                // before: unsigned arithmetic carries the 32-bit counter's
                // wrap, and a step of zero or of more than half the counter
                // is a repeat or a step back.
                if (previousSequence)
                {
                    const uint32_t step = *result.sequence - *previousSequence;
                    if (step == 0 || step > 0x80000000u)
                        throw std::runtime_error("ToupTek frame sequence went backwards; loss attribution is unknown");
                    index += step;
                }
                previousSequence = result.sequence;
                f->frameIndex = index;
                f->sourceDroppedFrames = index - delivered;
            }
            else
            {
                f->frameIndex = delivered;
            }
            ++delivered;
            listener.onFrame(std::move(f));
        }
    }
    catch (const std::exception& ex) { listener.onFailed(ex.what()); }
    catch (...) { listener.onFailed("Unexpected ToupTek acquisition failure"); }
    release();
}
