#pragma once

#include "capture_settings.h"
#include "frame.h"
#include "observing_site.h"
#include "stream_info.h"

#include <QtCore/QString>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// A continuous recording (Feature-14 Part A): every frame from its start to
// its end, unchanged, in one SER file, with frames.txt (each frame's index
// and time) and recording.txt (what the recording is, and how it ended).
//
// The source thread hands it shared frames and never waits: the frames wait
// in a queue with a byte allowance, and this writer's own thread writes
// them. A recording never loses a frame silently. When the queue is full the
// disk is not keeping up, and when the free space falls below the disk
// reserve there is no room; either way the recording stops at once, the
// frames already queued are written, the file is finished, and the reason
// is recorded. Once a second the SER header's frame count is brought up to
// date and the text files are flushed, so a crash leaves a file that can be
// read up to then.
//
// One recording at a time; a new one may start while the last is still
// being finished. The Ended callback runs on the writer thread.
class RecordingWriter
{
public:
    struct Start
    {
        // The recording goes in <folder>/<UTC stamp>/.
        QString folder;
        StreamInfo stream;
        ObservingSite site;
        Target target{Target::Moon};
        uint64_t diskReserveBytes{0};
        std::string clockRecord;
        // Who started it: the observer, or a schedule.
        std::string startedBy;
        // The queue's allowance.
        size_t queueBytes{0};
    };

    struct Status
    {
        // Taking frames.
        bool active{false};
        // The folder's name, once made.
        QString name;
        uint64_t frames{0};
        uint64_t bytes{0};
        double streamSeconds{0};
        double megabytesPerSecond{0};
        // Why the last recording ended; empty while one runs.
        std::string endReason;
    };

    using Ended = std::function<void(const Status& status)>;
    using FreeSpace = std::function<std::optional<uint64_t>(const QString& folder)>;

    explicit RecordingWriter(Ended ended = {});
    ~RecordingWriter();
    RecordingWriter(const RecordingWriter&) = delete;
    RecordingWriter& operator=(const RecordingWriter&) = delete;

    // Source thread. Begins a recording whose first frame is first. False
    // when one is already taking frames.
    bool start(Start start, const FramePtr& first);
    // Source thread. Queues a frame. False when the recording has stopped,
    // by a stop or by itself.
    bool push(const FramePtr& frame);
    // Any thread. Stops taking frames; those queued are still written.
    void stop(const std::string& reason);
    [[nodiscard]] Status status() const;
    [[nodiscard]] bool active() const;

    // Tests: the drive query, and a pause per frame for a slow disk.
    void setFreeSpace(FreeSpace freeSpace);
    void setWriteDelay(std::chrono::milliseconds delay);
    // Waits until everything queued is written and finished.
    void waitIdle();

private:
    struct Job
    {
        std::optional<Start> start;
        FramePtr frame;
        std::optional<std::string> end;
    };
    // Writer-thread state of the recording being written.
    struct Open;

    void run(std::stop_token stopToken);
    // Writer thread. Stops taking frames with this reason; the end follows the queue.
    void endItself(const std::string& reason);

    Ended ended_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::condition_variable idle_;
    std::deque<Job> queue_;
    size_t queuedBytes_{0};
    size_t allowance_{0};
    bool accepting_{false};
    bool busy_{false};
    Status status_;
    FreeSpace freeSpace_;
    std::chrono::milliseconds delay_{0};
    std::jthread thread_;
};
