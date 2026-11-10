#pragma once

#include "frame.h"

#include <atomic>
#include <functional>
#include <stop_token>
#include <thread>

// Generates simulated video frames at a configurable rate and resolution.
// Runs on an internal worker thread and emits frames via a callback.
class SyntheticFrameSource
{
public:
    using FrameCallback = std::function<void(const Frame&)>;

    explicit SyntheticFrameSource(int width = 640, int height = 480, double targetFps = 30.0);
    ~SyntheticFrameSource();

    SyntheticFrameSource(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource& operator=(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource(SyntheticFrameSource&&) = delete;
    SyntheticFrameSource& operator=(SyntheticFrameSource&&) = delete;

    void setFrameCallback(FrameCallback callback);

    void setTargetFps(double fps);
    [[nodiscard]] double targetFps() const;

    void setResolution(int width, int height);
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;

private:
    void run(std::stop_token stopToken);
    Frame generateFrame(uint64_t index);

    int width_;
    int height_;
    std::atomic<double> targetFps_;

    FrameCallback callback_;
    std::atomic<bool> isRunning_{false};
    std::jthread workerThread_;
};
