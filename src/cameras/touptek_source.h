#pragma once

#include "cameras/touptek_api.h"
#include "sources/frame_source.h"

#include <mutex>
#include <optional>
#include <thread>

// Feature-16. Owns one ToupTek camera and its worker, as QhySource owns a
// QHY one: Preview controls travel through one request slot; Monitoring
// reopens the camera for a fixed settings readback. Frame indices come from
// the camera's own sequence numbers when the SDK reports them, so a gap is a
// counted loss at the right place.
class ToupTekSource final : public FrameSource
{
public:
    ToupTekSource(std::shared_ptr<ToupTekApi> api, ToupTekProfile selected, ToupTekSettings settings);
    ~ToupTekSource() override;
    static ToupTekProfile inspect(const std::shared_ptr<ToupTekApi>& api, const std::string& id);
    static void validateSettings(const ToupTekCameraInfo& camera, const ToupTekSettings& settings);
    bool open(std::string& error) override;
    void start(FrameSourceListener& listener) override;
    void stop() override;
    StreamInfo streamInfo() const override;
    [[nodiscard]] ToupTekProfile actualProfile() const;
    void requestLiveControls(std::map<ToupTekControl,int64_t> controls);

private:
    void run(std::stop_token stop, FrameSourceListener& listener);
    void release() noexcept;
    void publishProfile();
    std::optional<double> applyLiveControls(FrameSourceListener& listener);
    std::shared_ptr<ToupTekApi> api_;
    ToupTekProfile selected_, actual_;
    ToupTekSettings requested_;
    bool opened_{false}, capturing_{false};
    mutable std::mutex settingsMutex_;
    ToupTekProfile profileSnapshot_;
    std::optional<std::map<ToupTekControl,int64_t>> liveRequest_;
    std::jthread worker_;
};
