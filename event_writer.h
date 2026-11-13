#pragma once

#include "event_clip.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

// What became of one clip handed to the writer.
//
// A result with neither a folder nor a reason is about the record alone: the
// session_start line could not be written.
struct ClipWriteResult
{
    // The clip folder. Empty when the clip was not saved.
    QString folder;

    // Why the clip was not saved. Empty when it was.
    QString notSaved;

    // The session's record is broken: candidates.txt could not be written,
    // or the writer thread has ended. Later candidates are not on record.
    bool recordFailed{false};
};

// Writes finished clips and the candidate log on a background thread.
//
// submit never blocks and never touches the disk. The writer owns its
// backlog: clips wait in a queue bounded by a number of frames, and a clip
// that does not fit is dropped at once and logged as not saved.
//
// Every clip gets one line per candidate in <outputRoot>/candidates.txt,
// saved or not. Each start of a stream gets a session_start line there
// first, naming the source, so a candidate that was not saved still says
// where it came from. One clip that fails to write is one failed clip; the
// writer goes on to the next. Each result is reported through Finished on
// the context object's thread.
class EventWriter
{
public:
    using Finished = std::function<void(const ClipWriteResult& result)>;

    // context and finished must be valid for the writer's lifetime.
    EventWriter(QObject* context, QString outputRoot, size_t maxQueuedFrames, Finished finished);
    ~EventWriter();

    EventWriter(const EventWriter&) = delete;
    EventWriter& operator=(const EventWriter&) = delete;

    // Any thread. Takes the clip, or drops its frames and logs it as not saved.
    void submit(EventClip clip);

    // Any thread. Queues the session_start line for a stream that starts now.
    // Only a failure to write it is reported.
    void logSessionStart(const StreamInfo& stream);

    // Writes what is queued, then stops the thread.
    void shutdown();

private:
    // The start of a stream, for its session_start line.
    struct SessionStart
    {
        std::chrono::system_clock::time_point time;
        StreamInfo stream;
    };

    // Work waiting for the writer: a session_start line, a clip, or a clip
    // refused with a reason (it has no frames and only waits to be logged).
    struct Job
    {
        std::optional<SessionStart> sessionStart;
        EventClip clip;
        QString notSavedReason;
        bool containsTestSignal{false}; // Kept even when a refused clip drops its frames.
    };

    void run(std::stop_token stopToken);
    void writeQueuedJobs(std::stop_token stopToken);
    ClipWriteResult process(Job& job);
    void deliver(const ClipWriteResult& result);

    QString writeClip(const EventClip& clip, QString& error) const;
    bool appendCandidates(const EventClip& clip, const ClipWriteResult& result, bool containsTestSignal) const;
    bool appendSessionStart(const SessionStart& start) const;
    bool appendLog(const QString& lines) const;

    QObject* context_;
    const QString outputRoot_;
    const size_t maxQueuedFrames_;
    Finished finished_;

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> queue_;
    size_t queuedFrames_{0};
    bool ended_{false};
    std::jthread thread_;
};
