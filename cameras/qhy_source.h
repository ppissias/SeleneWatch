#pragma once
#include "cameras/qhy_api.h"
#include "sources/frame_source.h"
#include <thread>

// Owns one QHY camera and its worker. SDK readbacks fix the stream metadata;
// the SDK-sized pool allocation is retained, but only native image bytes are
// published. No hardware loss counter or ADC packing is invented.
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
    const QhyProfile& actualProfile() const;
private:
    void run(std::stop_token stop, FrameSourceListener& listener);
    void release() noexcept;
    std::shared_ptr<QhyApi> api_;
    QhyProfile selected_, actual_;
    QhySettings requested_;
    StreamInfo info_;
    size_t bufferBytes_{0};
    bool opened_{false}, capturing_{false};
    std::jthread worker_;
};
