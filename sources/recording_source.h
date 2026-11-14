#pragma once

#include "frame_pool.h"
#include "sources/frame_source.h"
#include "recording/recording_reader.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <thread>

// Plays a recording as a frame source: the whole file once, or a loop over
// its first seconds (Feature-6, Playback). A .ser file is read by SerReader
// and an .avi file by AviReader.
//
// Frame times are the reader's: stored in the file, or nominal. The frame
// index is the position in the file. In a loop both keep increasing across
// laps, and at each lap the listener is told that the next frame does not
// follow the previous one. A frame's monotonic time is when it was played.
//
// At real-time speed the source waits out the intervals between the file's
// frame times, never more than one second for a single interval. At fast
// speed it does not wait.
class RecordingSource final : public FrameSource
{
public:
    enum class Speed
    {
        RealTime,
        Fast
    };

    explicit RecordingSource(std::filesystem::path path);
    ~RecordingSource() override;

    RecordingSource(const RecordingSource&) = delete;
    RecordingSource& operator=(const RecordingSource&) = delete;

    // Before open(). Zero plays the whole file once. Otherwise the frames in
    // the first loopSeconds of the file play again and again.
    void setLoopSeconds(double seconds);

    // Before open(). The frame rate assumed for a file that stores neither
    // frame times nor a rate: a SER file without a trailer, or an AVI file
    // whose header states no rate.
    void setNominalFps(double fps);

    // Any time, also while running.
    void setSpeed(Speed speed);

    bool open(std::string& error) override;
    [[nodiscard]] StreamInfo streamInfo() const override;
    void start(FrameSourceListener& listener) override;
    void stop() override;

private:
    void run(std::stop_token stopToken, FrameSourceListener& listener);

    // The time of a frame in the first lap.
    [[nodiscard]] std::chrono::system_clock::time_point fileTime(int position) const;

    [[nodiscard]] StreamInfo describe() const;

    const std::filesystem::path path_;
    double loopSeconds_{0.0};
    double nominalFps_{30.0};
    std::atomic<Speed> speed_{Speed::RealTime};

    // Set by open(), then read by the worker.
    std::unique_ptr<RecordingReader> reader_;
    int framesPerLap_{0};

    // Frame numbers a lap spans, gaps included.
    uint64_t numbersPerLap_{0};
    std::chrono::system_clock::duration lapLength_{};
    StreamInfo info_;

    std::jthread worker_;
};
