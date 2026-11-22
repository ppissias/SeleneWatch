#include "cameras/playerone_source.h"
#include "cameras/raw_camera_layout.h"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace {
PlayerOneCameraInfo findCamera(PlayerOneApi& api, int id)
{
    const auto cameras = api.cameras(); // The SDK needs a scan before an ID is used.
    const auto found = std::find_if(cameras.begin(), cameras.end(), [&](const auto& c) { return c.id == id; });
    if (found == cameras.end()) throw std::runtime_error("Selected Player One camera is not connected; refresh the camera list");
    return *found;
}
void validate(const PlayerOneCameraInfo& camera, const PlayerOneSettings& s)
{
    if (s.bin <= 0 || camera.width <= 0 || camera.height <= 0
        || (s.format != PixelFormat::Mono8 && s.format != PixelFormat::Mono16)
        || std::find(camera.bins.begin(), camera.bins.end(), s.bin) == camera.bins.end()
        || std::find(camera.formats.begin(), camera.formats.end(), s.format) == camera.formats.end()
        || s.width <= 0 || s.height <= 0 || s.width % 4 || s.height % 2 || s.x < 0 || s.y < 0
        || static_cast<int64_t>(s.x) + s.width > camera.width / s.bin
        || static_cast<int64_t>(s.y) + s.height > camera.height / s.bin)
        throw std::invalid_argument("Unsupported Player One format/bin or ROI: width must be a multiple of 4, height of 2, inside the binned sensor");
    if (camera.bayer != BayerPattern::None && s.bin != 1)
        throw std::invalid_argument("Colour binning needs camera-specific Bayer verification; use bin 1 for raw colour");
    if (frameBytes(s.width, s.height, s.format) > static_cast<size_t>(std::numeric_limits<long>::max()))
        throw std::invalid_argument("Player One frame exceeds the SDK buffer-size range");
}
std::string controlText(const PlayerOneSettings& s, PlayerOneControl kind, const char* units = "")
{
    const auto i = s.controls.find(kind);
    return i == s.controls.end() ? std::string{} : std::format("{}{}", i->second, units);
}
double exposureSeconds(const PlayerOneSettings& s)
{
    const auto i = s.controls.find(PlayerOneControl::Exposure);
    return i == s.controls.end() ? 1.0 : static_cast<double>(i->second) / 1e6;
}
} // namespace

PlayerOneSource::PlayerOneSource(std::shared_ptr<PlayerOneApi> api, PlayerOneProfile profile, PlayerOneSettings settings)
    : api_(std::move(api)), selected_(std::move(profile)), requested_(std::move(settings)) {}
PlayerOneSource::~PlayerOneSource() { stop(); }
void PlayerOneSource::validateSettings(const PlayerOneCameraInfo& camera, const PlayerOneSettings& settings) { validate(camera, settings); }

PlayerOneProfile PlayerOneSource::inspect(const std::shared_ptr<PlayerOneApi>& api, int id)
{
    if (!api) throw std::runtime_error("Player One support is not enabled in this build");
    PlayerOneProfile p;
    p.camera = findCamera(*api, id);
    p.sdkVersion = api->version();
    api->open(id);
    try
    {
        api->initialize(id);
        p.controls = api->controls(id);
        p.current = api->roi(id);
        for (const auto& c : p.controls) p.current.controls[c.kind] = c.value;
        api->close(id);
        return p;
    }
    catch (...) { api->close(id); throw; }
}

bool PlayerOneSource::open(std::string& error)
{
    stop();
    try
    {
        if (!api_) throw std::runtime_error("Player One support is not enabled in this build");
        actual_.camera = findCamera(*api_, selected_.camera.id);
        if (actual_.camera.name != selected_.camera.name || actual_.camera.serial != selected_.camera.serial)
            throw std::runtime_error("Selected Player One camera identity changed; refresh the camera list");
        validate(actual_.camera, requested_);
        const int id = actual_.camera.id;
        api_->open(id);
        opened_ = true;
        api_->initialize(id);
        actual_.sdkVersion = api_->version();
        api_->nativeOrientation(id);
        const auto caps = api_->controls(id);
        for (const auto& [kind, value] : requested_.controls)
        {
            const auto found = std::find_if(caps.begin(), caps.end(), [&](const auto& c) { return c.kind == kind; });
            if (found == caps.end() || (!found->writable && found->value != value))
                throw std::invalid_argument("Requested Player One control is unavailable or read only");
            if (!found->writable) continue;
            if (value < found->minimum || value > found->maximum)
                throw std::invalid_argument("Player One control outside its reported range: " + found->name);
            api_->setControl(id, kind, value);
        }
        api_->setRoi(id, requested_);
        actual_.current = api_->roi(id);
        validate(actual_.camera, actual_.current);
        actual_.controls = api_->controls(id);
        for (const auto& c : actual_.controls)
        {
            if (c.automatic) throw std::runtime_error("Player One automatic control remains enabled; a fixed evidence settings snapshot is required");
            actual_.current.controls[c.kind] = c.value;
        }
        updateInfo();
        publishProfile();
        return true;
    }
    catch (const std::exception& ex) { error = ex.what(); release(); return false; }
}

void PlayerOneSource::updateInfo()
{
    const auto& s = actual_.current;
    info_ = {};
    info_.source = actual_.camera.name;
    info_.camera = actual_.camera.name;
    info_.serial = actual_.camera.serial;
    info_.sdkVersion = actual_.sdkVersion;
    info_.exposure = controlText(s, PlayerOneControl::Exposure, " us");
    info_.gain = controlText(s, PlayerOneControl::Gain);
    info_.offset = controlText(s, PlayerOneControl::Offset);
    info_.usbBandwidth = controlText(s, PlayerOneControl::UsbBandwidth, " (Player One USB bandwidth limit)");
    info_.binning = s.bin == 1 ? "1" : std::format("{}, pixels averaged", s.bin);
    info_.roi = std::format("x={} y={} width={} height={} in output pixels after binning", s.x, s.y, s.width, s.height);
    info_.sensorBits = actual_.camera.sensorBits > 0 ? std::to_string(actual_.camera.sensorBits) : "";
    info_.sampleLayout = s.format == PixelFormat::Mono16
        ? "RAW16 delivered words treated as 16 bits, low aligned; sensor ADC packing unverified; no bits removed or rescaled"
        : "RAW8 delivered bytes; no rescaling";
    info_.frameTime = "host UTC immediately after POAGetImageData returns; includes readout/USB/buffering latency, not exposure start; host steady clock sampled alongside UTC";
    info_.frameIndex = "host delivery ordinal plus observed SDK drop total; derived, not hardware; gap attribution approximate because the SDK buffers frames";
    info_.conversion = "none; raw mono/Bayer; flip disabled; hardware qualification pending";
}
void PlayerOneSource::publishProfile()
{
    std::lock_guard lock(settingsMutex_);
    profileSnapshot_ = actual_;
    infoSnapshot_ = info_;
}
StreamInfo PlayerOneSource::streamInfo() const { std::lock_guard lock(settingsMutex_); return infoSnapshot_; }
PlayerOneProfile PlayerOneSource::actualProfile() const { std::lock_guard lock(settingsMutex_); return profileSnapshot_; }

void PlayerOneSource::requestLiveControls(std::map<PlayerOneControl,int64_t> controls)
{
    if (!worker_.joinable()) throw std::logic_error("Start Preview before changing live Player One controls");
    std::lock_guard lock(settingsMutex_);
    for (const auto& [kind, value] : controls)
    {
        const auto& caps = profileSnapshot_.controls;
        const auto found = std::find_if(caps.begin(), caps.end(), [&](const auto& c) { return c.kind == kind; });
        if (found == caps.end() || value < found->minimum || value > found->maximum || (!found->writable && value != found->value))
            throw std::invalid_argument("Player One live control is unavailable, read only, or outside its reported range");
    }
    liveRequest_ = std::move(controls);
}

std::optional<double> PlayerOneSource::applyLiveControls(FrameSourceListener& listener)
{
    std::optional<std::map<PlayerOneControl,int64_t>> request;
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
        api_->setControl(actual_.camera.id, kind, value);
        changed = true;
    }
    if (!changed) return {};
    const auto roi = api_->roi(actual_.camera.id);
    if (roi.width != previous.width || roi.height != previous.height || roi.x != previous.x || roi.y != previous.y
        || roi.bin != previous.bin || roi.format != previous.format)
        throw std::runtime_error("Player One frame layout changed during a live control update; restart Preview");
    actual_.controls = api_->controls(actual_.camera.id);
    actual_.current.controls.clear();
    for (const auto& c : actual_.controls)
    {
        if (c.automatic) throw std::runtime_error("Player One live readback turned an automatic control on; restart Preview");
        actual_.current.controls[c.kind] = c.value;
    }
    updateInfo();
    publishProfile();
    // Frames carry no settings of their own. This is a tuning estimate only;
    // Monitoring always reopens the camera.
    const double longestExposure = std::max(exposureSeconds(previous), exposureSeconds(actual_.current));
    listener.onSettingsChanged(info_, std::chrono::duration<double>{2 * longestExposure + 1});
    return 2 * longestExposure + 5;
}

void PlayerOneSource::start(FrameSourceListener& listener)
{
    if (!opened_ || worker_.joinable()) throw std::logic_error("Player One source must be opened once before start");
    worker_ = std::jthread([this, &listener](std::stop_token stop) { run(stop, listener); });
}
void PlayerOneSource::stop()
{
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    release();
    std::lock_guard lock(settingsMutex_);
    liveRequest_.reset();
}
void PlayerOneSource::release() noexcept
{
    if (capturing_) { api_->stopVideo(actual_.camera.id); capturing_ = false; }
    if (opened_) { api_->close(actual_.camera.id); opened_ = false; }
}

void PlayerOneSource::run(std::stop_token stop, FrameSourceListener& listener)
{
    try
    {
        const int id = actual_.camera.id;
        const auto s = actual_.current;
        api_->startVideo(id);
        capturing_ = true;
        uint64_t delivered = 0;
        int previousDrops = 0;
        auto lastFrame = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point controlDeadline{};
        double quietLimit = 2 * exposureSeconds(s) + 5;
        while (!stop.stop_requested())
        {
            if (const auto changed = applyLiveControls(listener))
            {
                lastFrame = std::chrono::steady_clock::now();
                quietLimit = *changed;
                controlDeadline = std::max(controlDeadline, lastFrame
                    + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>{*changed}));
            }
            auto f = pool_.acquire(frameBytes(s.width, s.height, s.format));
            if (!api_->read(id, std::span<uint8_t>(f->data.data(), f->data.size()), 100))
            {
                listener.onIdle();
                const auto now = std::chrono::steady_clock::now();
                if (now >= controlDeadline && std::chrono::duration<double>(now - lastFrame).count() > quietLimit)
                    throw std::runtime_error("Player One produced no frame within twice the exposure plus five seconds");
                continue;
            }
            f->timestamp = std::chrono::system_clock::now();
            f->steadyTimestamp = std::chrono::steady_clock::now();
            lastFrame = f->steadyTimestamp;
            quietLimit = 2 * exposureSeconds(actual_.current) + 5;
            f->width = s.width;
            f->height = s.height;
            f->format = s.format;
            f->significantBits = s.format == PixelFormat::Mono16 ? 16 : 8;
            f->alignment = BitAlignment::Low;
            f->bayer = roiBayerPattern(actual_.camera.bayer, s.x, s.y);
            f->testSource = actual_.camera.testSource;
            f->syntheticFlash = false;
            f->blip.reset();
            f->sourceDroppedFrames.reset();
            std::string telemetryError;
            try
            {
                const int drops = api_->dropped(id);
                if (drops < previousDrops) throw std::runtime_error("Player One drop counter moved backwards; loss attribution is unknown");
                previousDrops = drops;
                f->sourceDroppedFrames = static_cast<uint64_t>(drops);
            }
            catch (const std::exception& ex) { telemetryError = ex.what(); }
            f->frameIndex = delivered++ + static_cast<uint64_t>(previousDrops);
            listener.onFrame(std::move(f)); // A received frame is kept even if its telemetry failed.
            if (!telemetryError.empty()) throw std::runtime_error(telemetryError);
        }
    }
    catch (const std::exception& ex) { listener.onFailed(ex.what()); }
    catch (...) { listener.onFailed("Unexpected Player One acquisition failure"); }
    release();
}
