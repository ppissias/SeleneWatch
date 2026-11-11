#include "synthetic_frame_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

SyntheticFrameSource::SyntheticFrameSource(int width, int height, double targetFps)
    : width_(width)
    , height_(height)
    , targetFps_(targetFps)
{
}

SyntheticFrameSource::~SyntheticFrameSource()
{
    stop();
}

void SyntheticFrameSource::setFrameCallback(FrameCallback callback)
{
    callback_ = std::move(callback);
}

void SyntheticFrameSource::setTargetFps(double fps)
{
    targetFps_.store(std::max(1.0, std::min(fps, 240.0)));
}

double SyntheticFrameSource::targetFps() const
{
    return targetFps_.load();
}

void SyntheticFrameSource::setResolution(int width, int height)
{
    if (!isRunning_)
    {
        width_ = width;
        height_ = height;
    }
}

int SyntheticFrameSource::width() const
{
    return width_;
}

int SyntheticFrameSource::height() const
{
    return height_;
}

bool SyntheticFrameSource::start()
{
    if (isRunning_.exchange(true))
    {
        return true; // Already running
    }

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

void SyntheticFrameSource::run(std::stop_token stopToken)
{
    using clock = std::chrono::steady_clock;
    uint64_t frameIndex = 0;
    auto nextFrameTime = clock::now();

    Frame currentFrame;

    while (!stopToken.stop_requested() && isRunning_.load())
    {
        const double fps = targetFps_.load();
        const auto frameInterval = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / fps));

        generateFrame(currentFrame, frameIndex++);

        if (callback_)
        {
            callback_(currentFrame);
        }

        nextFrameTime += frameInterval;
        const auto now = clock::now();

        if (nextFrameTime > now)
        {
            std::this_thread::sleep_until(nextFrameTime);
        }
        else
        {
            // If fallen behind, resync to avoid bursts
            nextFrameTime = now;
        }
    }
}

void SyntheticFrameSource::generateFrame(Frame& frame, uint64_t index)
{
    frame.width = width_;
    frame.height = height_;
    frame.format = PixelFormat::Mono8;
    frame.frameIndex = index;
    frame.timestamp = std::chrono::system_clock::now();
    frame.steadyTimestamp = std::chrono::steady_clock::now();

    const size_t targetSize = static_cast<size_t>(width_ * height_);
    if (frame.data.size() != targetSize)
    {
        frame.data.resize(targetSize);
    }

    const float cx = width_ * 0.5f;
    const float cy = height_ * 0.5f;
    const float radius = std::min(width_, height_) * 0.38f;
    const float radiusSq = radius * radius;

    // Moving test feature: drifts horizontally across the frame
    const int movingX = static_cast<int>((index * 3) % width_);
    const int movingY = static_cast<int>(height_ * 0.45f);
    const int movingRadius = 4;

    for (int y = 0; y < height_; ++y)
    {
        const float dy = y - cy;
        const float dySq = dy * dy;
        uint8_t* row = &frame.data[y * width_];

        for (int x = 0; x < width_; ++x)
        {
            const float dx = x - cx;
            const float distSq = dx * dx + dySq;

            if (distSq <= radiusSq)
            {
                // Simulated lunar disk with limb darkening and crater-like variations
                const float normDist = std::sqrt(distSq) / radius;
                const float limbShading = std::clamp(std::cos(normDist * 1.57079f), 0.0f, 1.0f);

                // Subtle textural details (sinusoidal pattern simulating terrain / craters)
                const float texture = 0.85f + 0.15f * std::sin(x * 0.08f) * std::cos(y * 0.08f)
                                      + 0.08f * std::sin((x + y) * 0.03f);

                float intensity = 170.0f * limbShading * texture;
                row[x] = static_cast<uint8_t>(std::clamp(intensity, 15.0f, 240.0f));
            }
            else
            {
                // Deep sky background with slight sensor noise
                const uint8_t noise = static_cast<uint8_t>((x * 17 + y * 31 + index) % 7);
                row[x] = 10 + noise;
            }

            // Draw moving test marker
            const int testDx = x - movingX;
            const int testDy = y - movingY;
            if (testDx * testDx + testDy * testDy <= movingRadius * movingRadius)
            {
                row[x] = 255;
            }
        }
    }
}
