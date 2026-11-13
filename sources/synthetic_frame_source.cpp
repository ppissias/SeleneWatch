#include "sources/synthetic_frame_source.h"

#include "sources/fine_sleep.h"
#include "sources/synthetic_scene.h"

#include <algorithm>
#include <chrono>
#include <exception>

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

void SyntheticFrameSource::setTargetFps(double fps)
{
    targetFps_.store(std::max(1.0, std::min(fps, kMaxTargetFps)));
}

void SyntheticFrameSource::requestFlash()
{
    flashFramesRemaining_.store(kFlashFrames);
}

bool SyntheticFrameSource::open(std::string& /*error*/)
{
    return true;
}

StreamInfo SyntheticFrameSource::streamInfo() const
{
    StreamInfo info;
    info.source = "synthetic";
    info.camera = "none (painted test image)";
    info.exposure = "none (painted test image)";
    info.gain = "none (painted test image)";
    info.frameRate = "the target rate set in the window, which may change while running";
    info.frameTime = "the host's UTC clock when the frame was painted";
    info.frameIndex = "a counter kept by the generator, from 0 at start";
    info.conversion = "none";
    return info;
}

void SyntheticFrameSource::start(FrameSourceListener& listener)
{
    if (workerThread_.joinable())
    {
        return;
    }

    flashFramesRemaining_.store(0);
    workerThread_ = std::jthread([this, &listener](std::stop_token stopToken) {
        run(stopToken, listener);
    });
}

void SyntheticFrameSource::stop()
{
    if (workerThread_.joinable())
    {
        workerThread_.request_stop();
        workerThread_.join();
    }
}

size_t SyntheticFrameSource::liveFrameBytes() const
{
    return pool_.allocatedBytes();
}

void SyntheticFrameSource::run(std::stop_token stopToken, FrameSourceListener& listener)
{
    try
    {
        const FineSleepResolution fineSleep;

        using clock = std::chrono::steady_clock;
        uint64_t frameIndex = 0;
        auto nextFrameTime = clock::now();
        const size_t pixelBytes = frameBytes(width_, height_, PixelFormat::Mono8);

        while (!stopToken.stop_requested())
        {
            const double fps = targetFps_.load();
            const auto frameInterval = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / fps));

            // A recycled buffer once the stream is warm. This thread is its
            // only owner until the listener takes it.
            std::shared_ptr<Frame> frame = pool_.acquire(pixelBytes);
            generateFrame(*frame, frameIndex++);
            listener.onFrame(std::move(frame));

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
        listener.onFailed(ex.what());
    }
    catch (...)
    {
        listener.onFailed("Frame source failed");
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
