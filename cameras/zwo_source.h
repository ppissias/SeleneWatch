#pragma once

#include "cameras/zwo_api.h"
#include "sources/frame_source.h"

#include <thread>
#include <mutex>
#include <optional>

// Owns one ASI connection: stopped inspection/configuration on the caller's
// thread, then all SDK calls on its worker until stop joins it. Pixels go
// directly into the shared budgeted pool. Every delivered frame is native.
class ZwoSource final : public FrameSource
{
public:
    ZwoSource(std::shared_ptr<ZwoApi> api,ZwoProfile profile,ZwoSettings settings);
    ~ZwoSource() override;
    static ZwoProfile inspect(const std::shared_ptr<ZwoApi>& api,int id);
    static void validateSettings(const ZwoCameraInfo& camera,const ZwoSettings& settings);
    bool open(std::string& error) override;
    StreamInfo streamInfo() const override;
    void start(FrameSourceListener& listener) override;
    void stop() override;
    [[nodiscard]] ZwoProfile actualProfile() const;
    // Context thread, Preview only. The worker coalesces requests and applies
    // every writable native control between bounded SDK reads. ROI/format/bin
    // require a new open. Readbacks are snapshots, never per-frame guarantees.
    void requestLiveControls(std::map<ZwoControl,int64_t> controls);
private:
    void run(std::stop_token stop,FrameSourceListener& listener);
    void release() noexcept;
    void updateInfo();
    void publishProfile();
    std::optional<double> applyLiveControls(FrameSourceListener& listener);
    std::shared_ptr<ZwoApi> api_;
    ZwoProfile selected_, actual_;
    ZwoSettings requested_;
    StreamInfo info_;
    mutable std::mutex settingsMutex_;
    ZwoProfile profileSnapshot_;
    StreamInfo infoSnapshot_;
    std::optional<std::map<ZwoControl,int64_t>> liveRequest_;
    bool opened_{false}, capturing_{false};
    std::jthread worker_;
};
