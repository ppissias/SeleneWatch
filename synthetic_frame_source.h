#pragma once

#include "frame.h"
#include "frame_pool.h"
#include "frame_source.h"

#include <atomic>
#include <stop_token>
#include <thread>

// Generates a painted lunar disk at a configurable rate, with a moving
// marker and, on request, a fixed flash. It is the check that the whole
// frame path works without a recording or a camera.
//
// Each frame comes from the source's pool and is handed to the listener as
// its only owner, still writable.
class SyntheticFrameSource final : public FrameSource
{
public:
    explicit SyntheticFrameSource(int width = 640, int height = 480, double targetFps = 30.0);
    ~SyntheticFrameSource() override;

    SyntheticFrameSource(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource& operator=(const SyntheticFrameSource&) = delete;

    // Safe to call while running.
    void setTargetFps(double fps);

    // Paints a bright spot for the next few frames. Safe to call while running.
    void requestFlash();

    bool open(std::string& error) override;
    [[nodiscard]] StreamInfo streamInfo() const override;
    void start(FrameSourceListener& listener) override;
    void stop() override;
    [[nodiscard]] size_t liveFrameBytes() const override;

private:
    void run(std::stop_token stopToken, FrameSourceListener& listener);
    void generateFrame(Frame& frame, uint64_t index);

    int width_;
    int height_;
    std::atomic<double> targetFps_;
    std::atomic<int> flashFramesRemaining_{0};
    FramePool pool_;
    std::jthread workerThread_;
};
