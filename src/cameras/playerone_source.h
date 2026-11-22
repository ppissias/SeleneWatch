#pragma once

#include "cameras/playerone_api.h"
#include "sources/frame_source.h"

#include <mutex>
#include <optional>
#include <thread>

// Feature-16. Owns one Player One connection, as ZwoSource owns an ASI one:
// inspection and configuration on the caller's thread while stopped, then
// every SDK call on its worker until stop joins it. Pixels go directly into
// the shared budgeted pool; every published frame is native raw data.
class PlayerOneSource final : public FrameSource
{
public:
    PlayerOneSource(std::shared_ptr<PlayerOneApi> api, PlayerOneProfile profile, PlayerOneSettings settings);
    ~PlayerOneSource() override;
    static PlayerOneProfile inspect(const std::shared_ptr<PlayerOneApi>& api, int id);
    static void validateSettings(const PlayerOneCameraInfo& camera, const PlayerOneSettings& settings);
    bool open(std::string& error) override;
    StreamInfo streamInfo() const override;
    void start(FrameSourceListener& listener) override;
    void stop() override;
    [[nodiscard]] PlayerOneProfile actualProfile() const;
    // Context thread, Preview only. The worker applies the latest request
    // between bounded reads. ROI, format and bin need a new open.
    void requestLiveControls(std::map<PlayerOneControl,int64_t> controls);

private:
    void run(std::stop_token stop, FrameSourceListener& listener);
    void release() noexcept;
    void updateInfo();
    void publishProfile();
    std::optional<double> applyLiveControls(FrameSourceListener& listener);
    std::shared_ptr<PlayerOneApi> api_;
    PlayerOneProfile selected_, actual_;
    PlayerOneSettings requested_;
    StreamInfo info_;
    mutable std::mutex settingsMutex_;
    PlayerOneProfile profileSnapshot_;
    StreamInfo infoSnapshot_;
    std::optional<std::map<PlayerOneControl,int64_t>> liveRequest_;
    bool opened_{false}, capturing_{false};
    std::jthread worker_;
};
