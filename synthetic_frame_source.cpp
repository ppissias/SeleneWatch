#include "synthetic_frame_source.h"

#include "synthetic_scene.h"

#include <algorithm>
#include <chrono>
#include <exception>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

namespace {

// The target-FPS spin box stops at 120.
constexpr double kMaxTargetFps = 120.0;

} // namespace

SyntheticFrameSource::SyntheticFrameSource(int width, int height, double targetFps)
    : width_(width)
    , height_(height)
    , targetFps_(1.0)
{
    setTargetFps(targetFps);
}

SyntheticFrameSource::~SyntheticFrameSource()
{
    stop();
}

void SyntheticFrameSource::setFrameCallback(FrameCallback callback)
{
    callback_ = std::move(callback);
}

void SyntheticFrameSource::setErrorCallback(ErrorCallback callback)
{
    errorCallback_ = std::move(callback);
}

void SyntheticFrameSource::setTargetFps(double fps)
{
    targetFps_.store(std::max(1.0, std::min(fps, kMaxTargetFps)));
}

bool SyntheticFrameSource::start()
{
    if (isRunning_.exchange(true))
    {
        return true;
    }

    flashFramesRemaining_.store(0);
    acquireFineSleep();
    try
    {
        workerThread_ = std::jthread([this](std::stop_token stopToken) {
            run(stopToken);
        });
    }
    catch (...)
    {
        releaseFineSleep();
        isRunning_.store(false);
        throw;
    }

    return true;
}

void SyntheticFrameSource::stop()
{
    if (isRunning_.exchange(false))
    {
        if (workerThread_.joinable())
        {
            workerThread_.request_stop();
            workerThread_.join();
        }
        releaseFineSleep();
    }
}

void SyntheticFrameSource::acquireFineSleep()
{
    // sleep_until waits on the Windows timer. 1 ms resolution lets an 8–12 ms
    // frame interval wake on time. Held only while this source is running.
    if (timeBeginPeriod(1) == TIMERR_NOERROR)
    {
        fineSleepHeld_ = true;
    }
}

void SyntheticFrameSource::releaseFineSleep()
{
    if (fineSleepHeld_)
    {
        timeEndPeriod(1);
        fineSleepHeld_ = false;
    }
}

bool SyntheticFrameSource::isRunning() const
{
    return isRunning_.load();
}

void SyntheticFrameSource::requestFlash()
{
    flashFramesRemaining_.store(kFlashFrames);
}

size_t SyntheticFrameSource::liveFrameBytes() const
{
    return pool_.allocatedBytes();
}

void SyntheticFrameSource::run(std::stop_token stopToken)
{
    try
    {
        using clock = std::chrono::steady_clock;
        uint64_t frameIndex = 0;
        auto nextFrameTime = clock::now();
        const size_t pixelBytes = static_cast<size_t>(width_) * static_cast<size_t>(height_);

        while (!stopToken.stop_requested() && isRunning_.load())
        {
            const double fps = targetFps_.load();
            const auto frameInterval = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / fps));

            // A recycled buffer once the stream is warm. This thread is its
            // only owner until the callback takes it.
            std::shared_ptr<Frame> frame = pool_.acquire(pixelBytes);
            generateFrame(*frame, frameIndex++);

            if (callback_)
            {
                callback_(std::move(frame));
            }

            nextFrameTime += frameInterval;
            const auto now = clock::now();

            if (nextFrameTime > now)
            {
                std::this_thread::sleep_until(nextFrameTime);
            }
            else
            {
                // If fallen behind, resync to avoid bursts.
                nextFrameTime = now;
            }
        }
    }
    catch (const std::exception& ex)
    {
        reportFailure(ex.what());
    }
    catch (...)
    {
        reportFailure("Frame source failed");
    }
}

void SyntheticFrameSource::reportFailure(const char* message)
{
    if (!errorCallback_)
    {
        return;
    }

    try
    {
        errorCallback_(message);
    }
    catch (...)
    {
    }
}

void SyntheticFrameSource::generateFrame(Frame& frame, uint64_t index)
{
    frame.timestamp = std::chrono::system_clock::now();
    frame.steadyTimestamp = std::chrono::steady_clock::now();

    int framesLeft = flashFramesRemaining_.load();
    while (framesLeft > 0
        && !flashFramesRemaining_.compare_exchange_weak(framesLeft, framesLeft - 1))
    {
    }

    paintSyntheticFrame(frame, width_, height_, index, framesLeft > 0);
}
