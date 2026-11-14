#include "sources/synthetic_frame_source.h"

#include "sources/fine_sleep.h"
#include "sources/synthetic_scene.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>

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

void SyntheticFrameSource::setPixelLayout(PixelFormat format, int bits, BitAlignment alignment, BayerPattern bayer)
{
    if (workerThread_.joinable() || (format != PixelFormat::Mono8 && format != PixelFormat::Mono16)
        || bits < 8 || bits > 16 || (format == PixelFormat::Mono8 && (bits != 8 || bayer != BayerPattern::None))
        || (bayer != BayerPattern::None && bayer != BayerPattern::RGGB))
        throw std::invalid_argument("invalid synthetic pixel layout, or source is running");
    format_ = format; bits_ = bits; alignment_ = alignment; bayer_ = bayer;
}

bool SyntheticFrameSource::open(std::string& error)
{
    if (width_ <= 0 || height_ <= 0) { error = "synthetic dimensions must be positive"; return false; }
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

void SyntheticFrameSource::run(std::stop_token stopToken, FrameSourceListener& listener)
{
    try
    {
        const FineSleepResolution fineSleep;

        using clock = std::chrono::steady_clock;
        uint64_t frameIndex = 0;
        auto nextFrameTime = clock::now();
        const size_t pixelBytes = frameBytes(width_, height_, format_);

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
    frame.testSource = true;
    if (format_ == PixelFormat::Mono16)
    {
        const size_t pixels = static_cast<size_t>(width_) * height_;
        frame.data.resize(2 * pixels);
        const int shift = alignment_ == BitAlignment::High ? 16 - bits_ : 0;
        const int maximum = (1 << bits_) - 1;
        const LunarDisk disk = LunarDisk::centered(width_, height_);
        for (size_t i = pixels; i-- > 0;)
        {
            const int x = static_cast<int>(i % width_), y = static_cast<int>(i / width_);
            const int original = frame.data[i];
            int value = original * maximum / 255;
            if (original < 250 && !disk.contains(x, y)) value = 1 + original % 7;
            if (original < 250 && disk.contains(x, y) && x > disk.centerX)
                value = 4 + original % 20 + (x + y * 3) % 32;
            if (bayer_ != BayerPattern::None && original < 250)
            {
                if ((x & 1) != (y & 1)) value = value * 7 / 10;
                else if (y & 1) value = value * 4 / 10;
            }
            value <<= shift;
            frame.data[2 * i] = static_cast<uint8_t>(value & 255);
            frame.data[2 * i + 1] = static_cast<uint8_t>(value >> 8);
        }
        frame.format = format_; frame.significantBits = bits_;
        frame.alignment = alignment_; frame.bayer = bayer_;
    }
}
