#pragma once

#include "event_clip.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

// What became of one clip handed to the writer.
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
// saved or not. One clip that fails to write is one failed clip; the
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

    // Writes what is queued, then stops the thread.
    void shutdown();

private:
    // A clip waiting for the writer. A job with a reason has no frames; it
    // only waits to be logged.
    struct Job
    {
        EventClip clip;
        QString notSavedReason;
    };

    void run(std::stop_token stopToken);
    void writeQueuedJobs(std::stop_token stopToken);
    ClipWriteResult process(Job& job);
    void deliver(const ClipWriteResult& result);

    QString writeClip(const EventClip& clip, QString& error) const;
    bool appendCandidates(const EventClip& clip, const ClipWriteResult& result) const;

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
