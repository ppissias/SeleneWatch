#pragma once
#include "cameras/qhy_api.h"
#include "sources/frame_source.h"
#include <mutex>
#include <optional>
#include <thread>

// Owns one QHY camera and its worker. Preview controls travel through one
// request slot; Monitoring reopens the camera for a fixed settings readback.
// Only checked native pixels are published from the SDK-sized pool buffers.
class QhySource final : public FrameSource
{
public:
    QhySource(std::shared_ptr<QhyApi> api, QhyProfile selected, QhySettings settings);
    ~QhySource() override;
    static QhyProfile inspect(const std::shared_ptr<QhyApi>& api, const std::string& id);
    static void validateSettings(const QhyCameraInfo& camera, const QhySettings& settings);
    bool open(std::string& error) override;
    void start(FrameSourceListener& listener) override;
    void stop() override;
    StreamInfo streamInfo() const override;
    QhyProfile actualProfile() const;
    void requestLiveControls(std::map<QhyControl,double> controls);
private:
    void run(std::stop_token stop, FrameSourceListener& listener);
    void release() noexcept;
    void publishProfile();
    std::optional<double> applyLiveControls(FrameSourceListener& listener);
    std::shared_ptr<QhyApi> api_;
    QhyProfile selected_, actual_;
    QhySettings requested_;
    size_t bufferBytes_{0};
    bool opened_{false}, capturing_{false};
    mutable std::mutex settingsMutex_;
    QhyProfile profileSnapshot_;
    std::optional<std::map<QhyControl,double>> liveRequest_;
    std::jthread worker_;
};
