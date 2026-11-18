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
#include <string>
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
    bool memoryLimited{false};

    // The clip was not written because the output drive had less free space
    // than the clip's disk reserve (Feature-11). freeBytes is what was free.
    bool diskReserve{false};
    uint64_t freeBytes{0};

    // The overview's suggested kind of the clip (Feature-15), when made.
    QString suggestedKind;
};

// A guide pulse, kept by the writer to mark candidates that fall inside a
// mount correction and to list the pulses inside each clip (Evidence rule 9).
struct GuidePulseRecord
{
    std::chrono::system_clock::time_point start{};
    int milliseconds{0};
    // The image is still settling for this long after the pulse ends.
    double settleSeconds{0};
    std::string direction;
    bool calibration{false};
};

// The guiding state at one moment, from the guider's once-per-second line.
struct GuideStateRecord
{
    std::chrono::system_clock::time_point time{};
    // The lock state, and "guiding" or "calibrating" when the mount is being corrected.
    std::string state;
    double dx{0}, dy{0};
};

// Queued and active clip payload, outstanding clip count and cumulative write measurements.
struct WriterUsage
{
    size_t retainedBytes{0}, outstandingClips{0};
    uint64_t writtenBytes{0};
    double writeSeconds{0};
    // Batches of guiding.txt lines that could not be written.
    uint64_t guidingWriteFailures{0};
};

// Writes finished clips, the candidate log, and the guiding log on a
// background thread.
//
// submit never blocks and never touches the disk. The writer owns its
// backlog: queued and active clips share a byte allowance. A clip that does
// not fit is dropped at once and its candidates are logged as not saved.
//
// Every clip gets one line per candidate in <outputRoot>/candidates.txt,
// saved or not. Each entry to Monitoring gets a session_start line there
// first, naming the source, so a candidate that was not saved still says
// where it came from. One clip that fails to write is one failed clip; the
// writer goes on to the next. Each result is reported through Finished on
// the context object's thread.
//
// Before a clip is written, the free space on the output drive is checked
// against the clip's disk reserve. Below it, the clip is not written and its
// candidates are logged with not_saved=disk reserve.
class EventWriter
{
public:
    using Finished = std::function<void(const ClipWriteResult& result)>;
    // The free bytes on the drive holding a folder, or none when unknown.
    using FreeSpace = std::function<std::optional<uint64_t>(const QString& folder)>;

    // context and finished must be valid for the writer's lifetime.
    EventWriter(QObject* context, QString outputRoot, size_t maxQueuedBytes, Finished finished);
    ~EventWriter();

    EventWriter(const EventWriter&) = delete;
    EventWriter& operator=(const EventWriter&) = delete;

    // Any thread. Takes the clip, or drops its frames and logs it as not saved.
    void submit(EventClip clip);

    // Any thread. Queues the session_start line for a stream that starts now,
    // with the site and the last clock check (Feature-14). Only a failure to
    // write it is reported.
    void logSessionStart(const StreamInfo& stream, const ObservingSite& site = {}, const std::string& clockRecord = {},
        bool testingTools = false);

    // Any thread. Queues finished lines for candidates.txt that are not
    // about a clip: a recording's start and stop, a clock check, an exposure
    // change (Features 14 and 15). Only a failure to write them is reported.
    void logRecord(QString lines);

    // Any thread. Queues finished lines for <outputRoot>/guiding.txt (none
    // when empty) and the records that go with them. The records are kept in
    // memory, in order with the clips, so each clip can say whether its
    // triggers fell inside a mount correction. A failure to write is counted
    // in usage(), not reported as a clip result.
    void logGuiding(QString lines, std::optional<GuidePulseRecord> pulse = {}, std::optional<GuideStateRecord> state = {});

    // Writes what is queued, then stops the thread.
    void shutdown();
    void setByteLimit(size_t bytes);
    // Any thread. Queues a folder switch so previously queued clips finish
    // writing to the old folder first.
    void setOutputRoot(QString outputRoot);
    // Before the first submit. Replaces the drive query, for tests.
    void setFreeSpace(FreeSpace freeSpace);
    [[nodiscard]] WriterUsage usage() const;

private:
    // The start of a stream, for its session_start line.
    struct SessionStart
    {
        std::chrono::system_clock::time_point time;
        StreamInfo stream;
        ObservingSite site;
        std::string clockRecord;
        bool testingTools{false};
    };

    // Work waiting for the writer: a session_start line, guiding lines, a
    // clip, or a clip refused with a reason (it has no frames and only waits
    // to be logged).
    struct Job
    {
        std::optional<QString> setOutputRoot;
        std::optional<SessionStart> sessionStart;
        bool guiding{false};
        QString guidingLines;
        QString recordLines;
        std::optional<GuidePulseRecord> pulse;
        std::optional<GuideStateRecord> guideState;
        EventClip clip;
        QString notSavedReason;
        bool containsTestSignal{false}; // Kept even when a refused clip drops its frames.
        QString recordId;
        size_t payloadBytes{0};
    };

    void run(std::stop_token stopToken);
    void writeQueuedJobs(std::stop_token stopToken);
    ClipWriteResult process(Job& job);
    void deliver(const ClipWriteResult& result);

    QString writeClip(EventClip& clip, QString& error, QString& suggestedKind);
    // Writer thread. Saves the clip's overview (Feature-12) in its folder and
    // returns its metadata line, or why it was not written. Gives the
    // suggested kind and its reason (Feature-15) when it could measure them.
    QString writeOverview(const EventClip& clip, const QString& folder, QString& suggestedKind, QString& kindReason) const;
    // Writer thread. True, with the result filled in, when writing the clip
    // would go below its disk reserve.
    bool belowDiskReserve(const Job& job, ClipWriteResult& result) const;
    bool appendCandidates(const Job& job, const ClipWriteResult& result, bool pending) const;
    bool appendSessionStart(const SessionStart& start) const;
    bool appendLog(const QString& lines, const QString& fileName = "candidates.txt") const;
    // Writer thread. Whether a trigger at this time falls inside a pulse or its settling.
    [[nodiscard]] bool duringMountCorrection(std::chrono::system_clock::time_point time) const;

    QObject* context_;
    QString outputRoot_;
    size_t maxQueuedBytes_;
    Finished finished_;
    FreeSpace freeSpace_;

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> queue_;
    WriterUsage usage_;
    bool ended_{false};

    // Writer thread only: the guiding records of the last minutes.
    std::deque<GuidePulseRecord> pulses_;
    std::deque<GuideStateRecord> guideStates_;

    std::jthread thread_;
};
