#include "recording/event_writer.h"
#include "preview_image.h"
#include "recording/ser_writer.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QTextStream>
#include <QtCore/QTimeZone>
#include <QtCore/QUuid>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

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
    return QString("%1").arg(static_cast<int>(index), 4, 10, QChar('0'));
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

bool containsTestSignal(const EventClip& clip)
{
    return std::any_of(clip.frames.begin(), clip.frames.end(), [](const FramePtr& frame) {
        return frame->blip.has_value() || frame->syntheticFlash || frame->testSource;
    });
}

// Per-injection settings and per-frame marks live beside the evidence. A
// clip may start halfway through an injection, or hold several injections.
void writeTestSignalLines(QTextStream& out, const EventClip& clip)
{
    out << "contains_test_signal: " << (containsTestSignal(clip) ? "yes" : "no") << "\n";
    std::vector<uint64_t> written;
    for (const FramePtr& frame : clip.frames)
    {
        if (!frame->blip)
        {
            continue;
        }
        const BlipMark& mark = *frame->blip;
        if (std::find(written.begin(), written.end(), mark.firstFrameIndex) != written.end())
        {
            continue;
        }
        written.push_back(mark.firstFrameIndex);
        out << "blip: first_frame_index=" << mark.firstFrameIndex
            << " centre_x=" << mark.settings.x << " centre_y=" << mark.settings.y
            << " fwhm_px=" << QString::number(mark.settings.fwhm, 'g', 17)
            << " amplitude_adu=" << QString::number(mark.settings.amplitude, 'g', 17)
            << " duration_frames=" << mark.settings.frames << "\n";
    }
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
    const bool testSignal = containsTestSignal(clip);
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
            job.containsTestSignal = testSignal;
            job.recordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
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

    // Flush discovery before touching clip data. A killed write leaves this
    // pending record, with no outcome, rather than erasing the candidate.
    result.recordFailed = !appendCandidates(job, result, true);

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

    if (!appendCandidates(job, result, false))
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
        out << "software: SeleneWatch " << SELENEWATCH_VERSION << "\n";
        writeStreamLines(out, clip);
        writeTestSignalLines(out, clip);
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
            const Frame& frame = *clip.frames[i];
            out << frameFileName(i) << " " << frame.frameIndex << " " << utcText(frame.timestamp)
                << " ser_utc_ticks=" << timeToSerTicks(frame.timestamp);
            if (frame.blip)
            {
                out << " test_blip=" << frame.blip->firstFrameIndex << " blip_frame=" << frame.blip->frameNumber;
            }
            if (frame.syntheticFlash)
            {
                out << " synthetic_flash";
            }
            if (frame.testSource) out << " test_source";
            out << "\n";
            if (i > 0 && frame.frameIndex > clip.frames[i - 1]->frameIndex + 1)
            {
                out << "gap: after=" << clip.frames[i - 1]->frameIndex << " before=" << frame.frameIndex
                    << " missing=" << frame.frameIndex - clip.frames[i - 1]->frameIndex - 1 << "\n";
            }
        }
        out.flush();
        if (out.status() != QTextStream::Ok || !note.flush())
        {
            error = "could not write clip metadata";
            return {};
        }
    }
    note.close();

    if (clip.frames.empty())
    {
        error = "the clip has no frames";
        return {};
    }
    QFile evidence(dir.filePath("event.ser"));
    if (!evidence.open(QIODevice::WriteOnly))
    {
        error = "could not open event.ser";
        return {};
    }
    SerWriter ser(evidence);
    if (!ser.start(*clip.frames.front(), clip.stream.camera, error, containsTestSignal(clip))) return {};
    for (const FramePtr& frame : clip.frames)
    {
        if (!ser.append(*frame, error)) return {};
    }
    if (!ser.finish(error) || !evidence.flush())
    {
        if (error.isEmpty()) error = "could not flush event.ser";
        return {};
    }
    evidence.close();

    const uint64_t onset = clip.triggers.front().onsetFrameIndex;
    FramePtr baseline;
    for (const FramePtr& frame : clip.frames)
    {
        if (frame->frameIndex < onset) baseline = frame;
    }
    const auto peakTrigger = std::max_element(clip.triggers.begin(), clip.triggers.end(),
        [](const ClipTrigger& a, const ClipTrigger& b) { return a.brightPixelCount < b.brightPixelCount; });
    const auto peakFrame = std::find_if(clip.frames.begin(), clip.frames.end(), [&](const FramePtr& frame) {
        return frame->frameIndex == peakTrigger->triggerFrameIndex;
    });
    const FramePtr peak = peakFrame == clip.frames.end() ? clip.frames.front() : *peakFrame;
    QImage baselineImage;
    if (baseline)
    {
        baselineImage = frameToPreviewImage(*baseline);
    }
    else
    {
        baselineImage = QImage(peak->width, peak->height, QImage::Format_RGB32);
        baselineImage.fill(Qt::black);
        QPainter painter(&baselineImage);
        painter.setPen(Qt::yellow);
        painter.drawText(baselineImage.rect(), Qt::AlignCenter, "No pre-onset baseline frame");
    }
    if (!baselineImage.save(dir.filePath("01_baseline.png"), "PNG")
        || !frameToPreviewImage(*peak).save(dir.filePath("02_event_peak.png"), "PNG"))
    {
        error = "could not write clip preview images";
        return {};
    }

    // Record precisely which frames the display previews represent.
    if (!note.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        error = "could not finish clip metadata";
        return {};
    }
    {
        QTextStream out(&note);
        out << "baseline_frame_index: " << (baseline ? QString::number(baseline->frameIndex) : "unavailable") << "\n";
        out << "peak_preview_frame_index: " << peak->frameIndex << "\n";
        out.flush();
        if (out.status() != QTextStream::Ok || !note.flush())
        {
            error = "could not finish clip metadata";
            return {};
        }
    }
    note.close();

    // The completion marker. A folder without it is a partial clip.
    if (!note.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        error = QString("could not finish event.txt in %1").arg(folder);
        return {};
    }
    QTextStream done(&note);
    done << "complete: yes\n";
    done.flush();
    if (done.status() != QTextStream::Ok || !note.flush())
    {
        error = QString("could not finish event.txt in %1").arg(folder);
        return {};
    }
    return folder;
}

// One line per candidate, saved or not. False when the log cannot be written.
bool EventWriter::appendCandidates(const Job& job, const ClipWriteResult& result, bool pending) const
{
    const EventClip& clip = job.clip;
    // The outcome comes last on the line, because a reason can contain spaces.
    const QString outcome = result.folder.isEmpty()
        ? QString("not_saved=%1").arg(result.notSaved)
        : QString("clip=%1").arg(QDir(result.folder).dirName());

    QString lines;
    QTextStream out(&lines);
    for (const ClipTrigger& trigger : clip.triggers)
    {
        out << utcText(trigger.triggerTime) << " ";
        if (pending) out << "candidate_pending ";
        out << triggerFields(trigger)
            << " delta_threshold=" << clip.deltaThreshold
            << " min_bright_pixels=" << clip.minBrightPixels
            << (job.containsTestSignal ? " contains_test_signal=yes" : "")
            << " record_id=" << job.recordId;
        if (!pending) out << " " << outcome;
        out << "\n";
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
