#include "recording_source.h"

#include "fine_sleep.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {

using namespace std::chrono;

// The longest single interval waited for at real-time speed, so that a long
// gap in a file does not stall playback.
constexpr seconds kMaxWait{1};

// Waits until the deadline or a stop request. False when stopped.
bool waitUntil(steady_clock::time_point deadline, const std::stop_token& stopToken)
{
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait_until(lock, stopToken, deadline, [] { return false; });
    return !stopToken.stop_requested();
}

} // namespace

RecordingSource::RecordingSource(std::filesystem::path path)
    : path_(std::move(path))
{
}

RecordingSource::~RecordingSource()
{
    stop();
}

void RecordingSource::setLoopSeconds(double seconds)
{
    loopSeconds_ = std::max(0.0, seconds);
}

void RecordingSource::setNominalFps(double fps)
{
    nominalFps_ = std::max(1.0, fps);
}

void RecordingSource::setSpeed(Speed speed)
{
    speed_.store(speed);
}

bool RecordingSource::open(std::string& error)
{
    reader_ = SerReader{};
    if (!reader_.open(path_, error))
    {
        return false;
    }

    const int count = reader_.frameCount();
    framesPerLap_ = count;
    if (loopSeconds_ > 0.0)
    {
        const duration<double> limit(loopSeconds_);
        int frames = 1;
        while (frames < count && fileTime(frames) - fileTime(0) < limit)
        {
            ++frames;
        }
        // A lap of one frame would restart the detector on every frame.
        framesPerLap_ = std::max(frames, std::min(count, 2));
    }

    // A lap lasts its span plus one mean interval, so the times of the next
    // lap continue smoothly from the last frame of this one.
    const auto nominalInterval = duration_cast<system_clock::duration>(duration<double>(1.0 / nominalFps_));
    const auto span = fileTime(framesPerLap_ - 1) - fileTime(0);
    lapLength_ = framesPerLap_ > 1 ? span + span / (framesPerLap_ - 1) : nominalInterval;
    if (lapLength_ <= system_clock::duration::zero())
    {
        // Frame times that do not increase. Count the lap at the nominal rate.
        lapLength_ = nominalInterval * framesPerLap_;
    }

    info_ = describe();
    return true;
}

StreamInfo RecordingSource::describe() const
{
    const bool looping = loopSeconds_ > 0.0;
    const double lapSeconds = duration<double>(lapLength_).count();

    StreamInfo info;
    info.source = utf8Path(path_);
    info.camera = reader_.instrument();

    if (reader_.hasFrameTimes())
    {
        info.frameTime = "UTC from the file's trailer, as the capture program wrote it";
    }
    else
    {
        info.frameTime = std::format("nominal, not measured: {} plus the frame number at {:.3f} fps",
            reader_.startTime() ? "the header's start time" : "an unknown start time counted from zero",
            nominalFps_);
    }

    info.frameIndex = "the position in the file, from 0";
    if (reader_.frameCount() < reader_.headerFrameCount())
    {
        info.frameIndex += std::format("; the file holds {} of the {} frames its header names",
            reader_.frameCount(), reader_.headerFrameCount());
    }

    if (looping)
    {
        info.playback = std::format("loop over frames 0 to {} of {} (the first {:.3f} s); "
            "each later lap adds {} to the frame index and {:.6f} s to the frame time",
            framesPerLap_ - 1, reader_.frameCount(), loopSeconds_, framesPerLap_, lapSeconds);
        info.frameTime += "; shifted in later laps of the loop";
        info.frameIndex += "; shifted in later laps of the loop";
    }
    else
    {
        info.playback = std::format("whole file once, frames 0 to {}", reader_.frameCount() - 1);
    }
    return info;
}

StreamInfo RecordingSource::streamInfo() const
{
    return info_;
}

void RecordingSource::start(FrameSourceListener& listener)
{
    if (worker_.joinable())
    {
        return;
    }
    worker_ = std::jthread([this, &listener](std::stop_token stopToken) {
        run(stopToken, listener);
    });
}

void RecordingSource::stop()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
}

size_t RecordingSource::liveFrameBytes() const
{
    return pool_.allocatedBytes();
}

system_clock::time_point RecordingSource::fileTime(int position) const
{
    if (reader_.hasFrameTimes())
    {
        return reader_.frameTime(position);
    }
    const system_clock::time_point start = reader_.startTime().value_or(system_clock::time_point{});
    return start + duration_cast<system_clock::duration>(duration<double>(position / nominalFps_));
}

void RecordingSource::run(std::stop_token stopToken, FrameSourceListener& listener)
{
    try
    {
        const FineSleepResolution fineSleep;
        const bool looping = loopSeconds_ > 0.0;

        int64_t lap = 0;
        int position = 0;
        std::string error;

        // Real-time pacing: when the previous frame was due, and its time.
        bool paced = false;
        steady_clock::time_point due{};
        system_clock::time_point previousTime{};

        while (!stopToken.stop_requested())
        {
            if (position == framesPerLap_)
            {
                if (!looping)
                {
                    listener.onEnded();
                    return;
                }
                position = 0;
                ++lap;
                listener.onDiscontinuity();
            }

            const system_clock::time_point time = fileTime(position) + lapLength_ * lap;

            if (speed_.load() == Speed::RealTime)
            {
                const steady_clock::time_point now = steady_clock::now();
                if (paced)
                {
                    const auto interval = std::clamp<system_clock::duration>(
                        time - previousTime, system_clock::duration::zero(), kMaxWait);
                    due += duration_cast<steady_clock::duration>(interval);
                    // Fallen behind: carry on from now, without a burst to catch up.
                    due = std::max(due, now);
                }
                else
                {
                    due = now;
                }
                if (!waitUntil(due, stopToken))
                {
                    return;
                }
                paced = true;
            }
            else
            {
                paced = false;
            }

            // A recycled buffer once the stream is warm. This thread is its
            // only owner until the listener takes it.
            std::shared_ptr<Frame> frame = pool_.acquire(reader_.frameBytes());
            if (!reader_.readFrame(position, *frame, error))
            {
                throw std::runtime_error(error);
            }
            frame->frameIndex = static_cast<uint64_t>(lap) * static_cast<uint64_t>(framesPerLap_)
                + static_cast<uint64_t>(position);
            frame->timestamp = time;
            frame->steadyTimestamp = steady_clock::now();
            listener.onFrame(std::move(frame));

            previousTime = time;
            ++position;
        }
    }
    catch (const std::exception& ex)
    {
        listener.onFailed(ex.what());
    }
    catch (...)
    {
        listener.onFailed("Recording playback failed");
    }
}
