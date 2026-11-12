#pragma once

#include "frame.h"

#include <atomic>
#include <functional>
#include <stop_token>
#include <string>
#include <thread>

// Generates simulated video frames at a configurable rate and resolution.
// Runs on an internal worker thread and emits frames via a callback.
// Set the callbacks before start(). Both run on the worker thread.
class SyntheticFrameSource
{
public:
    using FrameCallback = std::function<void(Frame&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    explicit SyntheticFrameSource(int width = 640, int height = 480, double targetFps = 30.0);
    ~SyntheticFrameSource();

    SyntheticFrameSource(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource& operator=(const SyntheticFrameSource&) = delete;
    SyntheticFrameSource(SyntheticFrameSource&&) = delete;
    SyntheticFrameSource& operator=(SyntheticFrameSource&&) = delete;

    void setFrameCallback(FrameCallback callback);

    // Reports an exception that ended the worker loop. The callback must not
    // call stop(); that would join the worker from itself.
    void setErrorCallback(ErrorCallback callback);

    void setTargetFps(double fps);

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    // Paints a bright spot for the next few frames. Safe to call while running.
    void requestFlash();

private:
    void run(std::stop_token stopToken);
    void generateFrame(Frame& frame, uint64_t index);
    void reportFailure(const char* message);
    void acquireFineSleep();
    void releaseFineSleep();

    int width_;
    int height_;
    std::atomic<double> targetFps_;

    FrameCallback callback_;
    ErrorCallback errorCallback_;
    std::atomic<bool> isRunning_{false};
    std::atomic<int> flashFramesRemaining_{0};
    bool fineSleepHeld_{false};
    std::jthread workerThread_;
};
