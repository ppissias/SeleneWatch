#include "event_writer.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QTextStream>
#include <QtCore/QTimeZone>
#include <QtGui/QImage>

#include <bit>
#include <exception>
#include <utility>

namespace {

// Mono16 samples are stored little-endian. QImage's 16-bit formats hold
// native-endian words, so the two agree only on a little-endian machine.
static_assert(std::endian::native == std::endian::little);

QDateTime utcDateTime(std::chrono::system_clock::time_point time)
{
    const qint64 epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC);
}

QString utcStamp(std::chrono::system_clock::time_point time)
{
    return utcDateTime(time).toString("yyyyMMdd-HHmmss-zzz");
}

QString utcText(std::chrono::system_clock::time_point time)
{
    return utcDateTime(time).toString(Qt::ISODateWithMs);
}

QString frameFileName(size_t index)
{
    return QString("%1.png").arg(static_cast<int>(index), 4, 10, QChar('0'));
}

// The writer's own conversion, separate from the preview's. It wraps the
// stored samples for the encoder without changing or copying them, so the
// frame must outlive the image. A frame it cannot write exactly is an
// error, never a fallback to another format.
QImage frameToFileImage(const Frame& frame, QString& error)
{
    QImage::Format imageFormat = QImage::Format_Invalid;
    switch (frame.format)
    {
    case PixelFormat::Mono8:
        imageFormat = QImage::Format_Grayscale8;
        break;
    case PixelFormat::Mono16:
        // The samples are written as they are, wherever their significant
        // bits sit. A 12-bit sample in the top bits stays in the top bits.
        imageFormat = QImage::Format_Grayscale16;
        break;
    case PixelFormat::RGB888:
        imageFormat = QImage::Format_RGB888;
        break;
    }

    if (imageFormat == QImage::Format_Invalid)
    {
        error = QString("frame %1 has a pixel format that cannot be written losslessly").arg(frame.frameIndex);
        return {};
    }

    const qsizetype bytesPerLine = static_cast<qsizetype>(frame.width) * bytesPerPixel(frame.format);
    const qsizetype expected = bytesPerLine * frame.height;
    if (frame.width <= 0 || frame.height <= 0 || static_cast<qsizetype>(frame.data.size()) != expected)
    {
        error = QString("frame %1 is %2x%3 but holds %4 bytes")
            .arg(frame.frameIndex).arg(frame.width).arg(frame.height).arg(frame.data.size());
        return {};
    }

    return QImage(frame.data.data(), frame.width, frame.height, bytesPerLine, imageFormat);
}

// Makes a new folder for one clip. A folder is never reused: when the name
// is taken, -2, -3, ... is added.
QString createClipFolder(const QString& outputRoot, const QString& stamp, QString& error)
{
    if (!QDir().mkpath(outputRoot))
    {
        error = QString("could not create %1").arg(outputRoot);
        return {};
    }

    const QDir root(outputRoot);
    for (int attempt = 1; attempt <= 999; ++attempt)
    {
        const QString name = attempt == 1 ? stamp : QString("%1-%2").arg(stamp).arg(attempt);
        if (root.mkdir(name))
        {
            return root.filePath(name);
        }
        if (!root.exists(name))
        {
            break;
        }
    }

    error = QString("could not create a folder for %1 in %2").arg(stamp, outputRoot);
    return {};
}

QString orUnknown(const std::string& text)
{
    return text.empty() ? QString("unknown") : QString::fromStdString(text);
}

// The stream and the frame layout, at the top of event.txt.
void writeStreamLines(QTextStream& out, const EventClip& clip)
{
    out << "source: " << orUnknown(clip.stream.source) << "\n";
    out << "camera: " << orUnknown(clip.stream.camera) << "\n";
    out << "exposure: " << orUnknown(clip.stream.exposure) << "\n";
    out << "gain: " << orUnknown(clip.stream.gain) << "\n";
    out << "frame_rate: " << orUnknown(clip.stream.frameRate) << "\n";
    out << "frame_time: " << orUnknown(clip.stream.frameTime) << "\n";
    out << "frame_index: " << orUnknown(clip.stream.frameIndex) << "\n";
    out << "conversion: " << orUnknown(clip.stream.conversion) << "\n";
    if (!clip.stream.playback.empty())
    {
        out << "playback: " << QString::fromStdString(clip.stream.playback) << "\n";
    }

    if (clip.frames.empty())
    {
        return;
    }
    const Frame& first = *clip.frames.front();
    out << "frame_size: " << first.width << "x" << first.height << "\n";
    out << "pixel_format: " << pixelFormatName(first.format) << "\n";
    out << "significant_bits: " << first.significantBits << "\n";
    out << "bit_alignment: " << (first.alignment == BitAlignment::High ? "high" : "low") << "\n";
    out << "bayer_pattern: " << bayerPatternName(first.bayer) << "\n";
}

QString triggerFields(const ClipTrigger& trigger)
{
    QString text = QString("onset_frame_index=%1 trigger_frame_index=%2 bright_pixel_count=%3")
        .arg(trigger.onsetFrameIndex).arg(trigger.triggerFrameIndex).arg(trigger.brightPixelCount);
    if (trigger.continuesPreviousClip)
    {
        text += " continues_previous_clip";
    }
    return text;
}

} // namespace

EventWriter::EventWriter(QObject* context, QString outputRoot, size_t maxQueuedFrames, Finished finished)
    : context_(context)
    , outputRoot_(std::move(outputRoot))
    , maxQueuedFrames_(maxQueuedFrames)
    , finished_(std::move(finished))
{
    thread_ = std::jthread([this](std::stop_token stopToken) {
        run(stopToken);
    });
}

EventWriter::~EventWriter()
{
    shutdown();
}

void EventWriter::submit(EventClip clip)
{
    // Frames of a refused clip are released after the lock is dropped.
    std::vector<FramePtr> dropped;
    bool ended = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_)
        {
            ended = true;
            dropped.swap(clip.frames);
        }
        else
        {
            Job job;
            if (queuedFrames_ + clip.frames.size() > maxQueuedFrames_)
            {
                job.notSavedReason = "writer busy";
                dropped.swap(clip.frames);
            }
            queuedFrames_ += clip.frames.size();
            job.clip = std::move(clip);
            queue_.push_back(std::move(job));
        }
    }

    if (ended)
    {
        // Nothing can be written or logged any more. The window is told.
        deliver(ClipWriteResult{{}, "the writer has stopped", true});
        return;
    }
    ready_.notify_one();
}

void EventWriter::logSessionStart(const StreamInfo& stream)
{
    bool ended = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_)
        {
            ended = true;
        }
        else
        {
            Job job;
            job.sessionStart = SessionStart{std::chrono::system_clock::now(), stream};
            queue_.push_back(std::move(job));
        }
    }

    if (ended)
    {
        deliver(ClipWriteResult{{}, {}, true});
        return;
    }
    ready_.notify_one();
}

void EventWriter::shutdown()
{
    if (!thread_.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        thread_.request_stop();
    }
    ready_.notify_all();
    thread_.join();
}

void EventWriter::deliver(const ClipWriteResult& result)
{
    QMetaObject::invokeMethod(context_, [done = finished_, result] {
        done(result);
    }, Qt::QueuedConnection);
}

void EventWriter::run(std::stop_token stopToken)
{
    // An exception here is outside the writing of any one clip. It ends the
    // thread, and the window is told that the record has stopped.
    try
    {
        writeQueuedJobs(stopToken);
    }
    catch (const std::exception& ex)
    {
        try
        {
            deliver(ClipWriteResult{{}, QString::fromUtf8(ex.what()), true});
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        try
        {
            deliver(ClipWriteResult{{}, "the event writer failed", true});
        }
        catch (...)
        {
        }
    }

    // From here on submit refuses. What is still queued is let go.
    std::deque<Job> abandoned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
        abandoned.swap(queue_);
        queuedFrames_ = 0;
    }
}

void EventWriter::writeQueuedJobs(std::stop_token stopToken)
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [&] {
                return stopToken.stop_requested() || !queue_.empty();
            });
            if (queue_.empty())
            {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            queuedFrames_ -= job.clip.frames.size();
        }

        // A session_start line is reported only when it could not be written.
        const ClipWriteResult result = process(job);
        if (!job.sessionStart || result.recordFailed)
        {
            deliver(result);
        }
    }
}

ClipWriteResult EventWriter::process(Job& job)
{
    ClipWriteResult result;

    if (job.sessionStart)
    {
        result.recordFailed = !appendSessionStart(*job.sessionStart);
        return result;
    }

    if (!job.notSavedReason.isEmpty())
    {
        result.notSaved = job.notSavedReason;
    }
    else
    {
        // One clip that cannot be written is one failed clip. The writer goes on.
        try
        {
            QString error;
            const QString folder = writeClip(job.clip, error);
            if (error.isEmpty())
            {
                result.folder = folder;
            }
            else
            {
                result.notSaved = error;
            }
        }
        catch (const std::exception& ex)
        {
            result.notSaved = QString::fromUtf8(ex.what());
        }
        catch (...)
        {
            result.notSaved = "unknown error while writing the clip";
        }
    }

    // The frames are not needed for the log line.
    job.clip.frames.clear();

    if (!appendCandidates(job.clip, result))
    {
        result.recordFailed = true;
    }
    return result;
}

QString EventWriter::writeClip(const EventClip& clip, QString& error) const
{
    if (clip.triggers.empty())
    {
        error = "the clip has no trigger";
        return {};
    }

    const QString folder = createClipFolder(outputRoot_, utcStamp(clip.triggers.front().triggerTime), error);
    if (folder.isEmpty())
    {
        return {};
    }

    const QDir dir(folder);
    QFile note(dir.filePath("event.txt"));
    if (!note.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        error = QString("could not write event.txt in %1").arg(folder);
        return {};
    }

    {
        QTextStream out(&note);
        writeStreamLines(out, clip);
        out << "triggers: " << clip.triggers.size() << "\n";
        for (const ClipTrigger& trigger : clip.triggers)
        {
            out << "trigger: " << triggerFields(trigger)
                << " trigger_time=" << utcText(trigger.triggerTime) << "\n";
        }
        out << "delta_threshold: " << clip.deltaThreshold << "\n";
        out << "min_bright_pixels: " << clip.minBrightPixels << "\n";
        out << "frames_before: " << clip.framesBefore << "\n";
        out << "frames_after: " << clip.framesAfter << "\n";
        out << "frames:\n";
        for (size_t i = 0; i < clip.frames.size(); ++i)
        {
            out << frameFileName(i) << " " << clip.frames[i]->frameIndex << " "
                << utcText(clip.frames[i]->timestamp) << "\n";
        }
    }
    note.close();

    for (size_t i = 0; i < clip.frames.size(); ++i)
    {
        const QString path = dir.filePath(frameFileName(i));
        const QImage image = frameToFileImage(*clip.frames[i], error);
        if (image.isNull())
        {
            error = QString("%1 (clip left incomplete in %2)").arg(error, folder);
            return {};
        }
        if (!image.save(path, "PNG"))
        {
            error = QString("could not write %1").arg(path);
            return {};
        }
    }

    // The completion marker. A folder without it is a partial clip.
    if (!note.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        error = QString("could not finish event.txt in %1").arg(folder);
        return {};
    }
    QTextStream done(&note);
    done << "complete: yes\n";
    done.flush();
    if (done.status() != QTextStream::Ok)
    {
        error = QString("could not finish event.txt in %1").arg(folder);
        return {};
    }
    return folder;
}

// One line per candidate, saved or not. False when the log cannot be written.
bool EventWriter::appendCandidates(const EventClip& clip, const ClipWriteResult& result) const
{
    // The outcome comes last on the line, because a reason can contain spaces.
    const QString outcome = result.folder.isEmpty()
        ? QString("not_saved=%1").arg(result.notSaved)
        : QString("clip=%1").arg(QDir(result.folder).dirName());

    QString lines;
    QTextStream out(&lines);
    for (const ClipTrigger& trigger : clip.triggers)
    {
        out << utcText(trigger.triggerTime) << " " << triggerFields(trigger)
            << " delta_threshold=" << clip.deltaThreshold
            << " min_bright_pixels=" << clip.minBrightPixels
            << " " << outcome << "\n";
    }
    out.flush();
    return appendLog(lines);
}

// The line that opens each stream's entries. Its time is the host's clock
// at Start; the candidate lines that follow carry the frames' own times.
// The values are quoted, since a path or a description holds spaces.
bool EventWriter::appendSessionStart(const SessionStart& start) const
{
    QString line = utcText(start.time) + " session_start";
    line += QString(" camera=\"%1\"").arg(orUnknown(start.stream.camera));
    if (!start.stream.playback.empty())
    {
        line += QString(" playback=\"%1\"").arg(QString::fromStdString(start.stream.playback));
    }
    line += QString(" source=\"%1\"\n").arg(orUnknown(start.stream.source));
    return appendLog(line);
}

// Appends to candidates.txt. False when the log cannot be written.
bool EventWriter::appendLog(const QString& lines) const
{
    if (!QDir().mkpath(outputRoot_))
    {
        return false;
    }

    QFile log(QDir(outputRoot_).filePath("candidates.txt"));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return false;
    }

    QTextStream out(&log);
    out << lines;
    out.flush();
    return out.status() == QTextStream::Ok && log.flush();
}
