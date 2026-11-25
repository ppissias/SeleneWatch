#pragma once

#include "core/frame.h"
#include "display/preview_image.h"
#include "recording/ffmpeg_video.h"

#include <QtCore/QString>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

// Feature-36: a time-lapse beside the slow sky watch (Feature-35).
//
// While Monitoring, the session offers every frame. Every interval of source
// time one is kept in <watch folder>/timelapse/:
// - as a PNG with the camera's samples unchanged: 16-bit words for a 16-bit
//   stream, the Bayer mosaic as delivered; frames.txt gives each one's time;
// - in timelapse.mp4, stretched for display as the preview's auto-stretch,
//   its levels eased from frame to frame so the video does not flicker.
// The video is written as the frames come (Mp4Stream), so it is complete as
// soon as Monitoring stops, and a crash leaves it playable.
//
// The source thread offers frames and never waits: a kept frame is copied
// into a queue of at most two, and this writer's own thread writes it. A
// frame due while two wait is skipped and counted. When the free space falls
// below the disk reserve, no more frames are kept and the reason is recorded.
// One time-lapse per watch folder: it spans the watch's automatic restarts
// (the exposure follower's) and ends when the folder closes. timelapse.txt
// records what was kept and how it ended. The Ended callback runs on the
// writer thread.
class SkyTimelapse
{
public:
    // Value snapshot of the sky time-lapse writer's progress and final outcome.
    struct Status
    {
        bool active{false};
        QString folder;
        double intervalSeconds{0};
        int frames{0};
        int skipped{0};
        // Why frames or the video stopped before the end, if they did.
        QString problem;
        // Set once it has ended.
        QString endReason;
        QString videoPath;
    };
    using Ended = std::function<void(const Status&)>;

    explicit SkyTimelapse(Ended ended = {});
    ~SkyTimelapse();
    SkyTimelapse(const SkyTimelapse&) = delete;
    SkyTimelapse& operator=(const SkyTimelapse&) = delete;

    // Any thread. Ignored while one is active.
    void start(const QString& watchFolder, double intervalSeconds, uint64_t diskReserveBytes);
    // Source thread. Keeps the frame when one is due; never waits.
    void offer(const Frame& frame);
    // Any thread. No more frames; the writer writes what waits, closes the
    // video and records the end.
    void finish(const QString& reason);
    [[nodiscard]] Status status() const;
    // Waits until everything queued is written (tests and shutdown).
    void waitIdle();

    static constexpr int kVideoFps = 25;
    static constexpr int kVideoLongEdge = 1920;

private:
    struct Item
    {
        std::shared_ptr<Frame> frame;
        std::optional<QString> finish;
    };
    void run(std::stop_token stop);
    void write(const Frame& frame);
    void close(const QString& reason);

    Ended ended_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::deque<Item> queue_;
    bool busy_{false};
    Status status_;
    bool keeping_{false};
    bool finishQueued_{false};
    std::optional<std::chrono::system_clock::time_point> nextDue_;
    uint64_t reserveBytes_{0};
    std::chrono::system_clock::time_point started_{};

    // Writer thread only.
    Mp4Stream video_;
    bool videoFailed_{false};
    std::optional<AutoStretchCurve> curve_;

    std::jthread thread_;
};
