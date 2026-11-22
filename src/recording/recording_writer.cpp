#include "recording/recording_writer.h"

#include "recording/frame_timing.h"
#include "recording/metadata_text.h"
#include "recording/ser_writer.h"
#include "selenewatch_version.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTextStream>

#include <memory>
#include <utility>

namespace {

constexpr auto kUpdateInterval = std::chrono::seconds{1};
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

// A new folder for one recording; a taken name gets -2, -3, ...
QString createFolder(const QString& root, const QString& stamp, QString& error)
{
    // The clip writer may be making the same parent at the same moment, and
    // QDir::mkpath can fail when another thread makes a folder of the path
    // between its look and its make; a second try finds it made.
    bool made = false;
    for (int attempt = 0; attempt < 5 && !made; ++attempt)
    {
        made = QDir().mkpath(root) || QDir(root).exists();
        if (!made) std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!made)
    {
        error = QString("could not create %1").arg(root);
        return {};
    }
    const QDir parent(root);
    for (int attempt = 1; attempt <= 999; ++attempt)
    {
        const QString name = attempt == 1 ? stamp : QString("%1-%2").arg(stamp).arg(attempt);
        if (parent.mkdir(name)) return parent.filePath(name);
        if (!parent.exists(name)) break;
    }
    error = QString("could not create a folder for %1 in %2").arg(stamp, root);
    return {};
}

} // namespace

// The recording being written, on the writer thread.
struct RecordingWriter::Open
{
    explicit Open(bool hostClock) : timing(hostClock) {}
    RecordingWriter::Start start;
    uint64_t generation{0};
    QString folder;
    QFile serFile;
    std::unique_ptr<SerWriter> ser;
    QFile framesFile;
    QFile noteFile;
    FrameTimingMeter timing;
    uint64_t position{0};
    std::optional<uint64_t> lastIndex;
    std::chrono::system_clock::time_point firstTime{}, lastTime{};
    uint64_t gaps{0}, missing{0}, bytes{0};
    double writeSeconds{0};
    std::chrono::steady_clock::time_point nextUpdate{};
    bool failed{false};
};

RecordingWriter::RecordingWriter(Ended ended)
    : ended_(std::move(ended)), freeSpace_(freeBytesAt)
{
    thread_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
}

RecordingWriter::~RecordingWriter()
{
    stop("the program closed");
    thread_.request_stop();
    ready_.notify_all();
    thread_.join();
}

bool RecordingWriter::start(Start start, const FramePtr& first)
{
    std::lock_guard lock(mutex_);
    if (accepting_ || !first) return false;
    accepting_ = true;
    allowance_ = start.queueBytes;
    status_ = {};
    status_.active = true;
    status_.sessionMode = start.sessionMode;
    status_.parentFolder = start.folder;
    status_.frameLimit = start.maxFrames;
    Job job;
    job.generation = ++generation_;
    job.start = std::move(start);
    job.frame = first;
    queuedBytes_ += first->data.capacity();
    queue_.push_back(std::move(job));
    ready_.notify_one();
    return true;
}

bool RecordingWriter::push(const FramePtr& frame)
{
    std::lock_guard lock(mutex_);
    if (!accepting_) return false;
    const size_t bytes = frame->data.capacity();
    if (queuedBytes_ + bytes > allowance_)
    {
        // The disk is not keeping up. Stop now rather than drop frames.
        accepting_ = false;
        Job end;
        end.end = QString("disk too slow: %1 MB of frames waited to be written")
                      .arg(static_cast<double>(queuedBytes_) / 1e6, 0, 'f', 0).toStdString();
        queue_.push_back(std::move(end));
        ready_.notify_one();
        return false;
    }
    queuedBytes_ += bytes;
    Job job;
    job.frame = frame;
    queue_.push_back(std::move(job));
    ready_.notify_one();
    return true;
}

void RecordingWriter::stop(const std::string& reason)
{
    std::lock_guard lock(mutex_);
    if (!accepting_) return;
    accepting_ = false;
    Job end;
    end.end = reason;
    queue_.push_back(std::move(end));
    ready_.notify_one();
}

void RecordingWriter::endItself(uint64_t generation, const std::string& reason)
{
    std::lock_guard lock(mutex_);
    if (!accepting_ || generation != generation_) return;
    accepting_ = false;
    Job end;
    end.end = reason;
    queue_.push_back(std::move(end));
}

RecordingWriter::Status RecordingWriter::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

bool RecordingWriter::active() const
{
    std::lock_guard lock(mutex_);
    return accepting_;
}

void RecordingWriter::setFreeSpace(FreeSpace freeSpace)
{
    std::lock_guard lock(mutex_);
    freeSpace_ = std::move(freeSpace);
}

void RecordingWriter::setWriteDelay(std::chrono::milliseconds delay)
{
    std::lock_guard lock(mutex_);
    delay_ = delay;
}

void RecordingWriter::afterQueued(std::function<void()> callback)
{
    std::lock_guard lock(mutex_);
    Job job;
    job.callback = std::move(callback);
    queue_.push_back(std::move(job));
    ready_.notify_one();
}

void RecordingWriter::waitIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void RecordingWriter::run(std::stop_token stopToken)
{
    std::unique_ptr<Open> open;

    // Ends the recording being written: its trailer, its summary, its marker.
    const auto finish = [&](const std::string& reason) {
        Status ended;
        if (open)
        {
            QString error;
            const bool serDone = !open->failed && open->ser && open->ser->frames() > 0 && open->ser->finish(error)
                && open->serFile.flush();
            open->serFile.close();
            open->framesFile.close();
            if (open->noteFile.isOpen())
            {
                QTextStream out(&open->noteFile);
                out << "last_frame_index: " << (open->lastIndex ? QString::number(*open->lastIndex) : QString("none")) << "\n";
                out << "last_frame_utc: " << utcText(open->lastTime) << "\n";
                out << "frames_written: " << open->position << "\n";
                out << "dropped_frame_gaps: " << open->gaps << " missing_frames=" << open->missing << "\n";
                out << "frame_intervals: " << timingText(open->timing.result()) << "\n";
                out << "bytes_written: " << open->bytes << "\n";
                out << "write_seconds: " << QString::number(open->writeSeconds, 'f', 3) << "\n";
                out << "write_MB_per_second: "
                    << QString::number(open->writeSeconds > 0 ? static_cast<double>(open->bytes) / open->writeSeconds / 1e6 : 0, 'f', 1) << "\n";
                out << "ended: " << QString::fromStdString(reason) << "\n";
                if (!serDone) out << "ser_finish: failed" << (error.isEmpty() ? QString() : QString(": %1").arg(error)) << "\n";
                // The completion marker: a recording without it is partial.
                if (serDone) out << "complete: yes\n";
                out.flush();
                open->noteFile.close();
            }
        }
        {
            std::lock_guard lock(mutex_);
            ended.endReason = reason;
            if (open)
            {
                ended.sessionMode = open->start.sessionMode;
                ended.parentFolder = open->start.folder;
                ended.name = open->folder.isEmpty() ? QString() : QDir(open->folder).dirName();
                ended.frames = open->position;
                ended.bytes = open->bytes;
                ended.streamSeconds = std::chrono::duration<double>(open->lastTime - open->firstTime).count();
                ended.frameLimit = open->start.maxFrames;
                ended.megabytesPerSecond = open->writeSeconds > 0 ? static_cast<double>(open->bytes) / open->writeSeconds / 1e6 : 0;
                if (generation_ == open->generation) status_ = ended;
            }
        }
        open.reset();
        if (ended_) ended_(ended);
    };

    for (;;)
    {
        Job job;
        std::chrono::milliseconds delay{0};
        FreeSpace freeSpace;
        {
            std::unique_lock lock(mutex_);
            busy_ = false;
            if (queue_.empty()) idle_.notify_all();
            ready_.wait(lock, stopToken, [this] { return !queue_.empty(); });
            if (queue_.empty())
            {
                lock.unlock();
                if (open) finish("the program closed");
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            if (job.frame) queuedBytes_ -= job.frame->data.capacity();
            busy_ = true;
            delay = delay_;
            freeSpace = freeSpace_;
        }

        if (job.callback) { job.callback(); continue; }
        if (job.end)
        {
            finish(*job.end);
            continue;
        }
        if (job.start)
        {
            // A new recording; one left open had its end queued before.
            if (open) finish("a new recording started");
            open = std::make_unique<Open>(!job.start->stream.sdkVersion.empty());
            open->generation = job.generation;
            open->start = *job.start;
            const Frame& first = *job.frame;
            QString error;
            open->folder = createFolder(open->start.folder, utcStamp(first.timestamp), error);
            const QDir dir(open->folder);
            open->serFile.setFileName(dir.filePath("recording.ser"));
            open->framesFile.setFileName(dir.filePath("frames.txt"));
            open->noteFile.setFileName(dir.filePath("recording.txt"));
            open->ser = std::make_unique<SerWriter>(open->serFile);
            if (open->folder.isEmpty() || !open->serFile.open(QIODevice::ReadWrite | QIODevice::NewOnly)
                || !open->framesFile.open(QIODevice::WriteOnly | QIODevice::Text)
                || !open->noteFile.open(QIODevice::WriteOnly | QIODevice::Text)
                || !open->ser->start(first, open->start.stream.camera, error, false, open->start.site.telescope))
            {
                open->failed = true;
                endItself(open->generation, QString("the recording could not be created: %1").arg(error.isEmpty() ? "file error" : error).toStdString());
                continue;
            }
            QTextStream out(&open->noteFile);
            out << "software: SeleneWatch " << SELENEWATCH_VERSION << "\n";
            out << "kind: continuous recording\n";
            out << "session_mode: " << QString::fromStdString(open->start.sessionMode) << "\n";
            out << "mode: " << targetName(open->start.target) << "\n";
            if (open->start.maxFrames > 0) out << "frame_limit: " << open->start.maxFrames << "\n";
            out << "started_by: " << QString::fromStdString(open->start.startedBy) << "\n";
            writeSiteLines(out, open->start.site);
            writeStreamLines(out, open->start.stream, &first);
            out << "clock_check: " << (open->start.clockRecord.empty() ? QString("not made") : QString::fromStdString(open->start.clockRecord)) << "\n";
            out << "disk_reserve_bytes: " << open->start.diskReserveBytes << "\n";
            out << "queue_allowance_bytes: " << open->start.queueBytes << "\n";
            out << "first_frame_index: " << first.frameIndex << "\n";
            out << "first_frame_utc: " << utcText(first.timestamp) << "\n";
            out.flush();
            open->noteFile.flush();
            open->firstTime = first.timestamp;
            open->nextUpdate = std::chrono::steady_clock::now() + kUpdateInterval;
            {
                std::lock_guard lock(mutex_);
                if (generation_ == open->generation) status_.name = dir.dirName();
            }
        }
        if (!open || open->failed) continue;

        // A frame of the recording.
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        const Frame& frame = *job.frame;
        const auto writeStarted = std::chrono::steady_clock::now();
        QString error;
        if (!open->ser->append(frame, error))
        {
            open->failed = true;
            endItself(open->generation, QString("write error: %1").arg(error).toStdString());
            continue;
        }
        QTextStream lines(&open->framesFile);
        if (open->lastIndex && frame.frameIndex > *open->lastIndex + 1)
        {
            ++open->gaps;
            open->missing += frame.frameIndex - *open->lastIndex - 1;
            lines << "gap: after=" << *open->lastIndex << " before=" << frame.frameIndex
                  << " missing=" << frame.frameIndex - *open->lastIndex - 1 << "\n";
        }
        lines << open->position << " " << frame.frameIndex << " " << utcText(frame.timestamp)
              << " ser_utc_ticks=" << timeToSerTicks(frame.timestamp);
        if (frame.blip) lines << " test_blip=" << frame.blip->firstFrameIndex << " blip_frame=" << frame.blip->frameNumber;
        if (frame.syntheticFlash) lines << " synthetic_flash";
        if (frame.testSource) lines << " test_source";
        if (frame.sourceDroppedFrames) lines << " sdk_dropped_frames=" << *frame.sourceDroppedFrames;
        lines << "\n";
        lines.flush();
        open->timing.add(frame);
        ++open->position;
        open->lastIndex = frame.frameIndex;
        open->lastTime = frame.timestamp;
        open->bytes += frame.data.size();
        const auto now = std::chrono::steady_clock::now();
        open->writeSeconds += std::chrono::duration<double>(now - writeStarted).count();
        {
            std::lock_guard lock(mutex_);
            if (generation_ == open->generation)
            {
                status_.frames = open->position;
                status_.bytes = open->bytes;
                status_.streamSeconds = std::chrono::duration<double>(open->lastTime - open->firstTime).count();
                status_.megabytesPerSecond = open->writeSeconds > 0 ? static_cast<double>(open->bytes) / open->writeSeconds / 1e6 : 0;
            }
        }

        // Once a second: a readable file so far, and room left on the drive.
        if (now >= open->nextUpdate)
        {
            open->nextUpdate = now + kUpdateInterval;
            if (!open->ser->updateCount(error) || !open->serFile.flush() || !open->framesFile.flush())
            {
                open->failed = true;
                endItself(open->generation, QString("write error: %1").arg(error.isEmpty() ? "flush failed" : error).toStdString());
                continue;
            }
            const uint64_t reserve = open->start.diskReserveBytes;
            const auto free = reserve > 0 && freeSpace ? freeSpace(open->folder) : std::nullopt;
            if (free && *free < reserve)
                endItself(open->generation, QString("disk reserve: %1 GiB free, below the reserve of %2 GiB")
                    .arg(static_cast<double>(*free) / kGiB, 0, 'f', 1).arg(static_cast<double>(reserve) / kGiB, 0, 'f', 0).toStdString());
        }
    }
}
