#pragma once

#include "frame.h"
#include "guiding/lock_tracker.h"

#include <QtCore/QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

class EventWriter;

// Runs the feature lock on its own thread, on the newest frame offered.
//
// The source thread offers each published frame. The guider keeps only the
// newest and skips the rest, so it never holds up the frame path; with no
// lock and no lock request it does not even keep that one. The window sends
// requests (lock, unlock) and reads a copy of the status; it holds no lock
// state of its own.
//
// While the stream is Monitoring, the guider hands the writer one line per
// second of source time, and a line for each lock, refusal, loss, or
// release, for guiding.txt. The guide thread never touches the disk.
//
// The lock survives Stop and Start: the first frame of a new run begins a
// fresh estimate. A frame of another size or format releases it.
class Guider
{
public:
    // statusChanged runs on the guide thread and must only post. It is called
    // again only after status() has been read, so a slow reader is not flooded.
    Guider(EventWriter& writer, std::function<void()> statusChanged);
    ~Guider();

    Guider(const Guider&) = delete;
    Guider& operator=(const Guider&) = delete;

    // Source thread. recording is true while the stream is Monitoring.
    void offer(const FramePtr& frame, bool recording);
    // Any thread. The next frame does not follow the previous one.
    void discontinuity();

    // Any thread. The lock is taken on the next frame offered.
    void requestLock(LockRegion region);
    void requestUnlock();

    // Any thread.
    [[nodiscard]] LockStatus status() const;

    // Ends the thread. Called before the writer shuts down.
    void shutdown();

private:
    struct Requests
    {
        std::optional<LockRegion> lock;
        bool unlock{false};
        bool discontinuity{false};
    };

    void run(std::stop_token stopToken);
    // Guide thread. The lines for guiding.txt that this step produced.
    QString logLines(const LockStatus& before, const LockStatus& after, bool locked, bool unlocked, bool measured);
    void publish(const LockStatus& status);

    EventWriter& writer_;
    std::function<void()> statusChanged_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    FramePtr pending_;
    bool recording_{false};
    Requests requests_;
    // A lock is held or requested, so frames are worth keeping.
    std::atomic<bool> wanted_{false};

    mutable std::mutex statusMutex_;
    LockStatus status_;
    mutable std::atomic<bool> notifyPending_{false};

    // Guide thread only.
    LockTracker tracker_;
    std::optional<std::chrono::system_clock::time_point> lastLogTime_;

    std::jthread thread_;
};
