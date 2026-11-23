#include "recording/event_writer.h"
#include "recording/nightly_report.h"
#include "display/preview_image.h"
#include "recording/clip_overview.h"
#include "recording/frame_timing.h"
#include "recording/metadata_text.h"
#include "recording/ser_writer.h"
#include "selenewatch_version.h"
#include "recording/ffmpeg_video.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QStorageInfo>
#include <QtCore/QTextStream>
#include <QtCore/QTimeZone>
#include <QtCore/QUuid>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

QString frameFileName(size_t index)
{
    return QString("%1").arg(static_cast<int>(index), 4, 10, QChar('0'));
}

// Makes a new folder for one clip. A folder is never reused: when the name
// is taken, -2, -3, ... is added.
QString createClipFolder(const QString& outputRoot, const QString& stamp, QString& error)
{
    // The recording writer may be making the same folder at the same moment.
    if (!QDir().mkpath(outputRoot) && !QDir(outputRoot).exists())
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
            << " duration_frames=" << mark.settings.frames;
        if (mark.settings.kind != BlipKind::BrightFlash || mark.settings.vx != 0.0 || mark.settings.vy != 0.0)
        {
            out << " kind=" << blipKindName(mark.settings.kind)
                << " vx=" << QString::number(mark.settings.vx, 'g', 17)
                << " vy=" << QString::number(mark.settings.vy, 'g', 17);
        }
        out << "\n";
    }
}

// The model settings, and the Sky settings (Feature-11) when they are on.
QString detectorModelFields(const DetectionSettings& settings)
{
    QString text = QString(" max_patch_box_ratio=%1 reference_time_constant_seconds=%2 min_noise_adu=%3")
        .arg(settings.maxPatchBoxRatio, 0, 'g', 17).arg(settings.referenceTimeConstantSeconds, 0, 'g', 17)
        .arg(settings.minNoiseAdu, 0, 'g', 17);
    if (settings.brighteningShare > 0) text += QString(" brightening_share=%1").arg(settings.brighteningShare, 0, 'g', 17);
    if (settings.releaseSeconds > 0) text += QString(" release_seconds=%1").arg(settings.releaseSeconds, 0, 'g', 17);
    // The mask's shapes, separated by ';', their corners x,y by '_'.
    if (!settings.mask.empty()) text += " mask=" + QString::fromStdString(settings.mask.text()).replace(' ', '_');
    // Feature-13, when on.
    if (settings.darkening)
        text += QString(" darkening=yes max_dark_pixels=%1 max_dark_box_ratio=%2")
            .arg(settings.maxDarkPixels).arg(settings.maxDarkBoxRatio, 0, 'g', 17);
    if (settings.darkeningShare > 0) text += QString(" darkening_share=%1").arg(settings.darkeningShare, 0, 'g', 17);
    // Feature-31: flashes on the earthlit side leave out the sunlit side.
    if (settings.ignoreAboveShare > 0) text += QString(" ignore_above_share=%1").arg(settings.ignoreAboveShare, 0, 'g', 17);
    return text;
}

QString triggerFields(const ClipTrigger& trigger)
{
    QString text = QString("onset_frame_index=%1 trigger_frame_index=%2")
        .arg(trigger.onsetFrameIndex).arg(trigger.triggerFrameIndex);
    if (trigger.kind==TriggerKind::Manual)
    {
        text += " kind=manual requested_host_utc=" + utcText(trigger.requestedAt);
        text += " requested_source_utc=" + utcText(trigger.requestSourceTime);
        text += QString(" detection_sigma=%1 detection_min_pixels=%2 detection_max_pixels=%3 detection_one_frame=%4")
            .arg(trigger.settings.sigma,0,'g',17).arg(trigger.settings.minPixels).arg(trigger.settings.maxPixels).arg(trigger.settings.oneFrame?"yes":"no");
    }
    else
    {
        text += QString(" bright_pixel_count=%1").arg(trigger.brightPixelCount);
        text += QString(" sigma=%1 min_pixels=%2 max_pixels=%3 one_frame=%4 peak_sigma=%5 noise_floor_adu=%6 peak=(%7,%8) box=(%9,%10,%11,%12)")
        .arg(trigger.settings.sigma, 0, 'g', 17).arg(trigger.settings.minPixels).arg(trigger.settings.maxPixels)
        .arg(trigger.settings.oneFrame ? "yes" : "no").arg(trigger.peakSigma, 0, 'g', 17)
        .arg(trigger.noiseFloor, 0, 'g', 17).arg(trigger.peakX).arg(trigger.peakY)
        .arg(trigger.boxX).arg(trigger.boxY).arg(trigger.boxWidth).arg(trigger.boxHeight);
        // For a fall, the pixel count, peak_sigma and peak describe the dark
        // patch: its size, its depth in sigma, and its deepest pixel.
        switch (trigger.change)
        {
        case ChangeKind::Rise: text += " kind=automatic"; break;
        case ChangeKind::Fall: text += QString(" kind=darkening fall_adu=%1").arg(-trigger.peakAdu, 0, 'g', 17); break;
        case ChangeKind::LargeRise:
        case ChangeKind::LargeFall:
            text += QString(" kind=%1 share=%2 median_difference_adu=%3")
                .arg(trigger.change == ChangeKind::LargeRise ? "brightening" : "large_darkening")
                .arg(trigger.share, 0, 'g', 17).arg(trigger.medianDifferenceAdu, 0, 'g', 17);
            break;
        }
        text += QString(" active_patches=%1 protected_pixels=%2 learning_frames=%3 longest_active_seconds=%4")
            .arg(trigger.activePatches).arg(trigger.protectedPixels).arg(trigger.learningFrames).arg(trigger.longestActiveSeconds,0,'g',17);
    }
    text += detectorModelFields(trigger.settings);
    if (trigger.continuesPreviousClip)
    {
        text += " continues_previous_clip";
    }
    return text;
}

double seconds(std::chrono::system_clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

QString captionTime(std::chrono::system_clock::time_point time)
{
    return utcDateTime(time).toString("yyyy-MM-dd HH:mm:ss.zzz") + " UTC";
}

// The overview's lines about the clip: when and how it was triggered, the
// first trigger's measurement, the clip's extent, and its flags.
QStringList overviewCaption(const EventClip& clip, bool duringCorrection)
{
    const ClipTrigger& first = clip.triggers.front();
    QStringList lines;
    const bool manual = first.kind == TriggerKind::Manual;
    const bool large = !manual && (first.change == ChangeKind::LargeRise || first.change == ChangeKind::LargeFall);
    const QString kind = manual ? "Save now"
        : first.change == ChangeKind::LargeRise ? "large brightening"
        : first.change == ChangeKind::LargeFall ? "large darkening"
        : first.change == ChangeKind::Fall ? "darkening" : "automatic trigger";
    QString heading = QString("%1 · frame %2 · %3 · %4 mode").arg(captionTime(first.triggerTime)).arg(first.triggerFrameIndex)
        .arg(kind).arg(clip.captureSettings.target == Target::Sky ? "Sky" : "Moon");
    if (clip.triggers.size() > 1) heading += QString(" · %1 triggers").arg(clip.triggers.size());
    if (first.continuesPreviousClip) heading += " · continues the previous clip";
    lines << heading;
    if (manual)
        lines << QString("Requested at %1 · K %2").arg(captionTime(first.requestedAt)).arg(first.settings.sigma, 0, 'g', 4);
    else if (large)
        lines << QString("%1% of the frame at K at once, median difference %2 ADU · K %3")
            .arg(100 * first.share, 0, 'f', 1).arg(first.medianDifferenceAdu, 0, 'f', 1).arg(first.settings.sigma, 0, 'g', 4);
    else if (first.change == ChangeKind::Fall)
        lines << QString("Deepest fall %1 σ (%2 ADU) · %3 px in a %4 × %5 box at (%6, %7) · K %8, dark patch %9 to %10 px")
            .arg(first.peakSigma, 0, 'f', 1).arg(-first.peakAdu, 0, 'f', 1).arg(first.brightPixelCount)
            .arg(first.boxWidth).arg(first.boxHeight).arg(first.peakX).arg(first.peakY)
            .arg(first.settings.sigma, 0, 'g', 4).arg(first.settings.minPixels).arg(first.settings.maxDarkPixels);
    else
        lines << QString("Peak %1 σ · %2 px in a %3 × %4 box at (%5, %6) · K %7, patch %8 to %9 px")
            .arg(first.peakSigma, 0, 'f', 1).arg(first.brightPixelCount).arg(first.boxWidth).arg(first.boxHeight)
            .arg(first.peakX).arg(first.peakY).arg(first.settings.sigma, 0, 'g', 4)
            .arg(first.settings.minPixels).arg(first.settings.maxPixels);

    const auto start = clip.frames.front()->timestamp;
    uint64_t dropped = 0;
    for (size_t i = 1; i < clip.frames.size(); ++i)
        if (clip.frames[i]->frameIndex > clip.frames[i - 1]->frameIndex + 1)
            dropped += clip.frames[i]->frameIndex - clip.frames[i - 1]->frameIndex - 1;
    lines << QString("Clip: %1 frames, %2 s, from %3 s before the trigger · ended: %4")
        .arg(clip.frames.size()).arg(seconds(clip.frames.back()->timestamp - start), 0, 'f', 2)
        .arg(seconds(first.triggerTime - start), 0, 'f', 2).arg(QString::fromStdString(clip.endReason));
    QStringList flags;
    if (duringCorrection) flags << "during mount correction";
    if (dropped) flags << QString("%1 dropped frames").arg(dropped);
    if (containsTestSignal(clip)) flags << "contains a test signal";
    if (clip.endReason == "clip memory allowance") flags << "ended by the RAM allowance";
    if (!flags.isEmpty()) lines << "Flags: " + flags.join(" · ");
    lines << "Source: " + orUnknown(clip.stream.source);
    return lines;
}

// The guiding state nearest the first trigger, and every mount pulse whose
// motion or settling overlaps the clip's frames.
void writeGuidingLines(QTextStream& out, const EventClip& clip, const std::deque<GuidePulseRecord>& pulses,
    const std::deque<GuideStateRecord>& states)
{
    const auto trigger = clip.triggers.front().triggerTime;
    const GuideStateRecord* nearest = nullptr;
    for (const GuideStateRecord& state : states)
        if (!nearest || std::chrono::abs(state.time - trigger) < std::chrono::abs(nearest->time - trigger)) nearest = &state;
    if (nearest && std::chrono::abs(nearest->time - trigger) <= std::chrono::seconds{2})
    {
        out << "guiding_at_first_trigger: state=\"" << QString::fromStdString(nearest->state) << "\" offset_dx="
            << QString::number(nearest->dx, 'f', 2) << " offset_dy=" << QString::number(nearest->dy, 'f', 2)
            << " offset_time=" << utcText(nearest->time) << "\n";
    }
    else
    {
        out << "guiding_at_first_trigger: no lock record within 2 s\n";
    }

    std::vector<const GuidePulseRecord*> inside;
    if (!clip.frames.empty())
    {
        const auto first = clip.frames.front()->timestamp;
        const auto last = clip.frames.back()->timestamp;
        for (const GuidePulseRecord& pulse : pulses)
        {
            const auto end = pulse.start + std::chrono::milliseconds{pulse.milliseconds}
                + std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(pulse.settleSeconds));
            if (pulse.start <= last && end >= first) inside.push_back(&pulse);
        }
    }
    out << "mount_pulses_in_clip: " << inside.size() << "\n";
    for (const GuidePulseRecord* pulse : inside)
    {
        out << "mount_pulse: start=" << utcText(pulse->start) << " direction=" << QString::fromStdString(pulse->direction)
            << " ms=" << pulse->milliseconds << " settle_seconds=" << pulse->settleSeconds
            << " purpose=" << (pulse->calibration ? "calibration" : "guiding") << "\n";
    }
}

bool samePreviewLayout(const Frame& a, const Frame& b)
{
    return a.width == b.width && a.height == b.height && a.format == b.format
        && a.significantBits == b.significantBits && a.alignment == b.alignment && a.bayer == b.bayer;
}

std::vector<QImage> preparePreviewGifFrames(const EventClip& clip, const ClipOverview* overview,
    int maxLongEdge, QString& failureNote)
{
    const auto& frames = clip.frames;
    if (frames.empty())
    {
        failureNote = "not written: no frames";
        return {};
    }

    size_t firstPos = 0, peakPos = frames.size() / 2, lastPos = 0;
    if (overview && overview->hasActiveRun && overview->mainPosition < frames.size())
    {
        firstPos = overview->firstActivePosition;
        peakPos = overview->mainPosition;
        lastPos = overview->lastActivePosition;
    }
    else
    {
        const uint64_t targetIdx = clip.peak.ready
            ? clip.peak.frameIndex
            : (!clip.triggers.empty() ? clip.triggers.front().triggerFrameIndex : frames[peakPos]->frameIndex);
        for (size_t i = 0; i < frames.size(); ++i)
        {
            if (frames[i]->frameIndex == targetIdx)
            {
                peakPos = i;
                break;
            }
        }
        firstPos = lastPos = peakPos;
    }

    const std::vector<size_t> positions = selectEventGifFramePositions(
        frames.size(), firstPos, peakPos, lastPos, 2, 2, 10);
    if (positions.empty())
    {
        failureNote = "not written: no frames selected";
        return {};
    }

    std::vector<QImage> out;
    try
    {
        // Anchor auto-stretch levels on a quiet pre/post frame if available so a bright
        // flash or meteor does not dim the surrounding context frames.
        const size_t refPos = (positions.front() < firstPos) ? positions.front()
            : (positions.back() > lastPos ? positions.back() : positions.front());
        const Frame& refFrame = *frames[refPos];
        const AutoStretchCurve curve = computeAutoStretchCurve(refFrame);

        const int longSide = std::max(refFrame.width, refFrame.height);
        int outW = refFrame.width;
        int outH = refFrame.height;
        if (maxLongEdge > 0 && longSide > maxLongEdge)
        {
            const double scale = static_cast<double>(maxLongEdge) / static_cast<double>(longSide);
            outW = static_cast<int>(std::lround(refFrame.width * scale));
            outH = static_cast<int>(std::lround(refFrame.height * scale));
        }
        outW = std::max(2, outW & ~1);
        outH = std::max(2, outH & ~1);

        out.reserve(positions.size());
        for (size_t pos : positions)
        {
            const Frame& f = *frames[pos];
            if (!samePreviewLayout(f, refFrame)) continue;
            QImage rendered = renderAutoStretchedFrame(f, curve, false, true);
            if (rendered.isNull())
            {
                failureNote = "not written: RAM limit";
                return {};
            }
            if (rendered.width() != outW || rendered.height() != outH)
                rendered = rendered.scaled(outW, outH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (rendered.format() != QImage::Format_RGB888)
                rendered = std::move(rendered).convertToFormat(QImage::Format_RGB888);
            if (rendered.isNull())
            {
                failureNote = "not written: image conversion failed";
                return {};
            }
            out.push_back(std::move(rendered));
        }
    }
    catch (const MemoryLimitError&)
    {
        failureNote = "not written: RAM limit";
        return {};
    }
    catch (const std::exception& ex)
    {
        failureNote = QString("not written: %1").arg(QString::fromUtf8(ex.what()));
        return {};
    }
    if (out.empty()) failureNote = "not written: no compatible frames";
    return out;
}

} // namespace

// A trigger as the clip's group sees it (Feature-46): what triggerFields
// writes into event.txt, which the nightly report reads back.
TriggerPoint triggerPointOf(const ClipTrigger& trigger)
{
    TriggerPoint point;
    point.time = trigger.triggerTime;
    point.frameIndex = trigger.triggerFrameIndex;
    if (trigger.kind == TriggerKind::Manual)
    {
        point.change = TriggerPoint::Change::Manual;
        return point;
    }
    switch (trigger.change)
    {
    case ChangeKind::Rise: point.change = TriggerPoint::Change::Rise; break;
    case ChangeKind::Fall: point.change = TriggerPoint::Change::Fall; break;
    case ChangeKind::LargeRise: point.change = TriggerPoint::Change::WideRise; break;
    case ChangeKind::LargeFall: point.change = TriggerPoint::Change::WideFall; break;
    }
    point.peak = QPoint(trigger.peakX, trigger.peakY);
    if (trigger.boxWidth > 0 && trigger.boxHeight > 0)
        point.box = QRect(trigger.boxX, trigger.boxY, trigger.boxWidth, trigger.boxHeight);
    point.pixels = trigger.brightPixelCount;
    point.sigma = trigger.peakSigma;
    point.share = trigger.share;
    return point;
}

EventWriter::EventWriter(QObject* context, QString outputRoot, size_t maxQueuedBytes, Finished finished)
    : context_(context)
    , outputRoot_(std::move(outputRoot))
    , maxQueuedBytes_(maxQueuedBytes)
    , finished_(std::move(finished))
    , freeSpace_(freeBytesAt)
{
    thread_ = std::jthread([this](std::stop_token stopToken) {
        run(stopToken);
    });
}

EventWriter::~EventWriter()
{
    shutdown();
}

void EventWriter::setByteLimit(size_t bytes)
{
    std::lock_guard lock(mutex_);
    maxQueuedBytes_ = bytes;
}

void EventWriter::setOutputRoot(QString outputRoot, bool sessionLayout)
{
    {
        std::lock_guard lock(mutex_);
        if (ended_) return;
        Job job;
        job.sessionLayout = sessionLayout;
        job.setOutputRoot = std::move(outputRoot);
        queue_.push_back(std::move(job));
    }
    ready_.notify_one();
}

void EventWriter::setFreeSpace(FreeSpace freeSpace)
{
    std::lock_guard lock(mutex_);
    freeSpace_ = std::move(freeSpace);
}

bool EventWriter::belowDiskReserve(const Job& job, ClipWriteResult& result) const
{
    const uint64_t reserve = job.clip.captureSettings.diskReserveBytes;
    if (reserve == 0) return false;
    FreeSpace freeSpace;
    {
        std::lock_guard lock(mutex_);
        freeSpace = freeSpace_;
    }
    const auto free = freeSpace ? freeSpace(outputRoot_) : std::nullopt;
    if (!free || *free >= reserve + job.payloadBytes) return false;
    result.notSaved = "disk reserve";
    result.diskReserve = true;
    result.freeBytes = *free;
    return true;
}

WriterUsage EventWriter::usage() const
{
    std::lock_guard lock(mutex_);
    return usage_;
}

void EventWriter::submit(EventClip clip)
{
    const bool testSignal = containsTestSignal(clip);
    // Frames of a refused clip are released after the lock is dropped.
    std::vector<FramePtr> dropped;
    BudgetVector<float> droppedDifference;
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
            clip.writerByteLimit = maxQueuedBytes_;
            job.recordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            for (const auto& frame : clip.frames) job.payloadBytes += frame->data.capacity();
            job.payloadBytes += clip.peakDifference.capacity() * sizeof(float);
            if (job.payloadBytes > maxQueuedBytes_ - std::min(maxQueuedBytes_, usage_.retainedBytes))
            {
                job.notSavedReason = "writer busy";
                dropped.swap(clip.frames);
                droppedDifference.swap(clip.peakDifference);
                job.payloadBytes = 0;
            }
            usage_.retainedBytes += job.payloadBytes;
            if (job.payloadBytes) ++usage_.outstandingClips;
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

void EventWriter::logSessionStart(const StreamInfo& stream, const ObservingSite& site, const std::string& clockRecord,
    bool testingTools)
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
            job.sessionStart = SessionStart{std::chrono::system_clock::now(), stream, site, clockRecord, testingTools};
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

void EventWriter::finishReport(QString folder, std::function<void(QString, QString)> finished)
{
    std::lock_guard lock(mutex_);
    Job job;
    job.folder = std::move(folder);
    job.reportFinished = std::move(finished);
    queue_.push_back(std::move(job));
    ready_.notify_one();
}

void EventWriter::afterQueued(std::function<void()> finished)
{
    {
        std::lock_guard lock(mutex_);
        if (!ended_)
        {
            Job job;
            job.drained = std::move(finished);
            queue_.push_back(std::move(job));
            ready_.notify_one();
            return;
        }
    }
    finished();
}

void EventWriter::logRecord(QString lines, QString folder, QString fileName)
{
    bool ended = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_) ended = true;
        else
        {
            Job job;
            job.recordLines = std::move(lines);
            job.folder = std::move(folder);
            job.fileName = std::move(fileName);
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

void EventWriter::logGuiding(QString lines, std::optional<GuidePulseRecord> pulse, std::optional<GuideStateRecord> state)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ended_)
        {
            if (!lines.isEmpty()) ++usage_.guidingWriteFailures;
            return;
        }
        Job job;
        job.guiding = true;
        job.guidingLines = std::move(lines);
        job.pulse = std::move(pulse);
        job.guideState = std::move(state);
        queue_.push_back(std::move(job));
    }
    ready_.notify_one();
}

bool EventWriter::duringMountCorrection(std::chrono::system_clock::time_point time) const
{
    for (const GuidePulseRecord& pulse : pulses_)
    {
        const auto end = pulse.start + std::chrono::milliseconds{pulse.milliseconds}
            + std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(pulse.settleSeconds));
        if (time >= pulse.start && time <= end) return true;
    }
    return false;
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
        usage_.retainedBytes = 0;
        usage_.outstandingClips = 0;
    }
    for (auto& job : abandoned)
        if (job.drained) job.drained();
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
        }

        if (job.drained)
        {
            job.drained();
            continue;
        }

        if (job.reportFinished)
        {
            QString error;
            const QString path = writeNightlyReport(job.folder, error);
            QMetaObject::invokeMethod(context_, [done = std::move(job.reportFinished), path, error] {
                done(path, error);
            }, Qt::QueuedConnection);
            continue;
        }

        if (job.setOutputRoot)
        {
            sessionLayout_ = job.sessionLayout;
            if (outputRoot_ != *job.setOutputRoot)
            {
                outputRoot_ = std::move(*job.setOutputRoot);
                pulses_.clear();
                guideStates_.clear();
            }
            continue;
        }

        // Guiding lines are not part of the candidate record; a failure is counted.
        // The records are kept for the clips that follow, newest last.
        if (job.guiding)
        {
            // Each list keeps 15 minutes before its newest record; a recording's
            // frame times can be years old.
            constexpr auto kKeep = std::chrono::minutes{15};
            if (job.pulse) pulses_.push_back(*job.pulse);
            if (job.guideState) guideStates_.push_back(*job.guideState);
            while (!pulses_.empty() && pulses_.front().start < pulses_.back().start - kKeep) pulses_.pop_front();
            while (!guideStates_.empty() && guideStates_.front().time < guideStates_.back().time - kKeep) guideStates_.pop_front();
            if (!job.guidingLines.isEmpty() && !appendLog(job.guidingLines, "guiding.txt"))
            {
                std::lock_guard lock(mutex_);
                ++usage_.guidingWriteFailures;
            }
            continue;
        }

        // Record lines that are not about a clip; a failure breaks the record.
        if (!job.recordLines.isEmpty())
        {
            if (!appendLog(job.recordLines, job.fileName, job.folder)) deliver(ClipWriteResult{{}, {}, true});
            continue;
        }

        // A session_start line is reported only when it could not be written.
        const ClipWriteResult result = process(job);
        {
            std::lock_guard lock(mutex_);
            usage_.retainedBytes -= job.payloadBytes;
            if (job.payloadBytes) --usage_.outstandingClips;
        }
        if (!job.sessionStart || result.recordFailed)
        {
            deliver(result);
        }
    }
}

ClipWriteResult EventWriter::process(Job& job)
{
    ClipWriteResult result;
    result.memoryLimited = job.clip.endReason == "clip memory allowance";

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
    else if (belowDiskReserve(job, result))
    {
        // The candidates are logged below, as not saved.
    }
    else
    {
        // One clip that cannot be written is one failed clip. The writer goes on.
        try
        {
            QString error;
            const QString folder = writeClip(job.clip, error, result.group, result.groupFacts);
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
    job.clip.peakDifference.clear();

    if (!appendCandidates(job, result, false))
    {
        result.recordFailed = true;
    }
    return result;
}

QString EventWriter::writeClip(EventClip& clip, QString& error, QString& group, QString& groupFacts)
{
    if (clip.triggers.empty())
    {
        error = "the clip has no trigger";
        return {};
    }

    const QString folder = createClipFolder(sessionLayout_ ? QDir(outputRoot_).filePath("clips") : outputRoot_, utcStamp(clip.triggers.front().triggerTime), error);
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
        out << "mode: " << targetName(clip.captureSettings.target) << "\n";
        writeStreamLines(out, clip.stream, clip.frames.empty() ? nullptr : clip.frames.front().get());
        // Feature-14: where and with what, how far to trust the times, and
        // the continuous recording this clip fell in.
        writeSiteLines(out, clip.site);
        out << "clock_check: " << (clip.clockRecord.empty() ? QString("not made") : QString::fromStdString(clip.clockRecord)) << "\n";
        out << "frame_intervals: " << timingText(measureFrameTiming(clip.frames, !clip.stream.sdkVersion.empty())) << "\n";
        if (!clip.duringRecording.empty())
            out << "during_recording: " << QString::fromStdString(clip.duringRecording) << "\n";
        writeTestSignalLines(out, clip);
        out << "triggers: " << clip.triggers.size() << "\n";
        for (const ClipTrigger& trigger : clip.triggers)
        {
            out << "trigger: " << triggerFields(trigger)
                << " trigger_time=" << utcText(trigger.triggerTime)
                << (duringMountCorrection(trigger.triggerTime) ? " during_mount_correction=yes" : "") << "\n";
        }
        writeGuidingLines(out, clip, pulses_, guideStates_);
        out << "detector: per-pixel sigma / compact patch\n";
        out << "reference_policy: update background outside passing patch boxes plus one pixel; clip collection does not hold learning\n";
        out << "persistence_policy: adjacent/overlapping patches inherit state; separate patches trigger independently\n";
        const auto& firstSettings = clip.triggers.front().settings;
        out << "detector_settings_scope: first trigger\n";
        out << "reference_time_constant_seconds: " << QString::number(firstSettings.referenceTimeConstantSeconds, 'g', 17) << "\n";
        out << "max_patch_box_ratio: " << QString::number(firstSettings.maxPatchBoxRatio, 'g', 17) << "\n";
        out << "min_noise_adu: " << QString::number(firstSettings.minNoiseAdu, 'g', 17) << "\n";
        out << "frames_before: " << clip.framesBefore << "\n";
        out << "frames_after: " << clip.framesAfter << "\n";
        out << "capture_history_seconds: " << clip.captureSettings.historySeconds << "\n";
        out << "capture_post_seconds: " << clip.captureSettings.postSeconds << "\n";
        out << "capture_max_post_seconds: " << clip.captureSettings.maxPostSeconds << "\n";
        out << "memory_limit_bytes: " << clip.captureSettings.memoryBytes << "\n";
        out << "history_available_seconds: " << clip.availableHistorySeconds << "\n";
        out << "history_byte_limit: " << clip.historyByteLimit << "\n";
        out << "collection_byte_limit: " << clip.collectionByteLimit << "\n";
        out << "writer_byte_limit: " << clip.writerByteLimit << "\n";
        out << "collection_end: " << QString::fromStdString(clip.endReason) << "\n";
        out << "disk_reserve_bytes: " << clip.captureSettings.diskReserveBytes << "\n";
        if (clip.continuesClipTriggeredAt)
            out << "continues_clip: " << utcStamp(*clip.continuesClipTriggeredAt) << "\n";
        for (const ReleaseRecord& release : clip.releases)
            out << "released_to_background: frame_index=" << release.frameIndex << " utc=" << utcText(release.time) << "\n";
        out << "difference_memory_limited: " << (clip.previewLimited ? "yes" : "no") << "\n";
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
            if (frame.sourceDroppedFrames) out << " sdk_dropped_frames=" << *frame.sourceDroppedFrames;
            else if (!clip.stream.sdkVersion.empty()) out << " sdk_dropped_frames=unknown";
            if (!clip.stream.sdkVersion.empty())
                out << " host_steady_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(frame.steadyTimestamp.time_since_epoch()).count();
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
    const auto writeStarted = std::chrono::steady_clock::now();
    if (!ser.start(*clip.frames.front(), clip.stream.camera, error, containsTestSignal(clip), clip.site.telescope)) return {};
    for (const FramePtr& frame : clip.frames)
    {
        if (!ser.append(*frame, error)) return {};
    }
    if (!ser.finish(error) || !evidence.flush())
    {
        if (error.isEmpty()) error = "could not flush event.ser";
        return {};
    }
    const auto evidenceBytes = evidence.size();
    evidence.close();
    {
        std::lock_guard lock(mutex_);
        usage_.writtenBytes += static_cast<uint64_t>(evidenceBytes);
        usage_.writeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - writeStarted).count();
    }

    // Feature-12: the overview needs every frame, so it is made before the
    // frames are released. A review aid must not cost the evidence: when it
    // cannot be made, the clip is still complete and says why.
    ClipDescription description;
    ClipOverview overviewData;
    bool hasOverviewData = false;
    const QString overviewNote = writeOverview(clip, folder, description, &overviewData);
    group = groupName(description.group);
    groupFacts = description.facts;
    hasOverviewData = overviewData.mainPosition < clip.frames.size();

    QString previewGifNote;
    std::vector<QImage> previewGifFrames = preparePreviewGifFrames(
        clip, hasOverviewData ? &overviewData : nullptr, 512, previewGifNote);

    const uint64_t onset = clip.triggers.front().onsetFrameIndex;
    FramePtr baseline;
    for (const FramePtr& frame : clip.frames)
    {
        if (frame->frameIndex < onset) baseline = frame;
    }
    const auto peakTrigger = std::max_element(clip.triggers.begin(), clip.triggers.end(),
        [](const ClipTrigger& a, const ClipTrigger& b) { return a.brightPixelCount < b.brightPixelCount; });
    const auto peakFrame = std::find_if(clip.frames.begin(), clip.frames.end(), [&](const FramePtr& frame) {
        return frame->frameIndex == (clip.peak.ready ? clip.peak.frameIndex : peakTrigger->triggerFrameIndex);
    });
    const FramePtr peak = peakFrame == clip.frames.end() ? clip.frames.front() : *peakFrame;
    // Metadata and native evidence are closed. Retain just the two frames
    // needed for display previews, freeing the remaining payload promptly.
    clip.frames.clear();
    const MemoryBudget budget = peak->data.get_allocator().budget;
    QImage baselineImage;
    if (baseline)
    {
        baselineImage = frameToPreviewImage(*baseline);
    }
    else
    {
        baselineImage = makeBudgetImage(peak->width, peak->height, QImage::Format_RGB32, budget);
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

    QImage difference = makeBudgetImage(peak->width, peak->height, QImage::Format_RGB32, budget);
    difference.fill(Qt::black);
    if (clip.peakDifference.size() == static_cast<size_t>(peak->width) * peak->height)
    {
        const float maximum = std::max(0.001f, *std::max_element(clip.peakDifference.begin(), clip.peakDifference.end()));
        for (int y = 0; y < peak->height; ++y)
        {
            auto* row = reinterpret_cast<QRgb*>(difference.scanLine(y));
            for (int x = 0; x < peak->width; ++x)
            {
                const int value = static_cast<int>(std::clamp(clip.peakDifference[static_cast<size_t>(y) * peak->width + x]
                    * 255.0f / maximum, 0.0f, 255.0f));
                row[x] = qRgb(value, value, value);
            }
        }
        QPainter painter(&difference);
        painter.setPen(QPen(Qt::red, 1));
        painter.drawRect(QRect(clip.peak.boxX, clip.peak.boxY, clip.peak.boxWidth, clip.peak.boxHeight));
        painter.drawText(8, 18, QString("Pre-update reference difference; peak %1 sigma; display scaled")
            .arg(clip.peak.peakSigma, 0, 'f', 1));
    }
    else
    {
        QPainter painter(&difference);
        painter.setPen(Qt::yellow);
        painter.drawText(difference.rect(), Qt::AlignCenter, clip.previewLimited
            ? "Reference difference not retained: RAM limit" : "No measured reference difference available");
    }
    if (!difference.save(dir.filePath("03_diff_annotated.png"), "PNG"))
    {
        error = "could not write annotated difference preview";
        return {};
    }

    if (previewGifNote.isEmpty())
    {
        EventPreviewGifOptions gifOptions;
        gifOptions.fps = 4;
        gifOptions.maxLongEdge = 512;
        gifOptions.maxBytes = 512 * 1024;
        int gifW = 0, gifH = 0;
        qint64 gifBytes = 0;
        QString gifErr;
        if (encodeEventPreviewGif(previewGifFrames, dir.filePath("04_event_preview.gif"),
                gifOptions, gifErr, &gifW, &gifH, &gifBytes))
        {
            previewGifNote = QString("04_event_preview.gif frames=%1 size=%2x%3 bytes=%4 fps=%5; display only, not evidence")
                .arg(previewGifFrames.size()).arg(gifW).arg(gifH).arg(gifBytes).arg(gifOptions.fps);
        }
        else
        {
            previewGifNote = QString("not written: %1").arg(gifErr);
        }
    }
    previewGifFrames.clear();

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
        out << "peak_preview_role: " << (clip.peak.ready ? "selected automatic passing patch" : "capture request frame; no measured passing peak") << "\n";
        if (clip.peak.ready)
        {
            out << "peak_sigma: " << QString::number(clip.peak.peakSigma, 'g', 17) << "\n";
            out << "peak_delta_adu: " << QString::number(clip.peak.peakAdu, 'g', 17) << "\n";
            out << "peak_patch: pixels=" << clip.peak.brightPixels << " peak=(" << clip.peak.peakX << "," << clip.peak.peakY
                << ") box=(" << clip.peak.boxX << "," << clip.peak.boxY << "," << clip.peak.boxWidth << "," << clip.peak.boxHeight << ")\n";
            out << "peak_settings: sigma=" << QString::number(clip.peak.settings.sigma, 'g', 17)
                << " min_pixels=" << clip.peak.settings.minPixels << " max_pixels=" << clip.peak.settings.maxPixels
                << " one_frame=" << (clip.peak.settings.oneFrame ? "yes" : "no")
                << detectorModelFields(clip.peak.settings) << "\n";
            out << "difference_preview: " << (clip.previewLimited ? "unavailable: RAM limit" : "positive native-ADU difference scaled to its maximum; annotations are display only") << "\n";
        }
        out << "overview: " << overviewNote << "\n";
        out << "preview_gif: " << previewGifNote << "\n";
        // Feature-46: measured, from the triggers and the run around the peak.
        out << "group: " << groupName(description.group) << " (" << description.facts << "); measured, not an identification\n";
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

QString EventWriter::writeOverview(const EventClip& clip, const QString& folder, ClipDescription& description,
    ClipOverview* outOverview) const
{
    const ClipTrigger& first = clip.triggers.front();
    OverviewRequest request;
    request.sigma = first.settings.sigma;
    request.referenceTimeConstantSeconds = first.settings.referenceTimeConstantSeconds;
    request.minNoiseAdu = first.settings.minNoiseAdu;
    request.minPixels = first.settings.minPixels;
    request.onsetFrameIndex = first.onsetFrameIndex;
    if (clip.peak.ready) request.peakFrameIndex = clip.peak.frameIndex;
    request.referenceTime = first.triggerTime;
    request.arcsecPerPixel = clip.site.arcsecPerPixel();
    bool duringCorrection = false;
    for (const ClipTrigger& trigger : clip.triggers)
    {
        request.triggers.push_back(triggerPointOf(trigger));
        duringCorrection = duringCorrection || duringMountCorrection(trigger.triggerTime);
    }
    request.mask = first.settings.mask;
    request.caption = overviewCaption(clip, duringCorrection);
    // Feature-46: without an overview the group comes from the triggers alone.
    description = describeClip(request.triggers, {}, request.arcsecPerPixel);
    try
    {
        const ClipOverview overview = makeClipOverview(clip.frames, request, clip.frames.front()->data.get_allocator().budget);
        if (outOverview)
        {
            *outOverview = overview;
            outOverview->image = {};
        }
        // The group is kept even if the picture cannot be saved.
        description = overview.description;
        if (!overview.image.save(QDir(folder).filePath("00_overview.png"), "PNG"))
            return "not written: could not save 00_overview.png";
        // The run around the peak is recorded so the nightly report can
        // group the clip again exactly as here.
        QString run;
        if (overview.run)
            run = QString(" run_frames=%1 run_seconds=%2 run_moved_px=%3")
                .arg(overview.run->frames).arg(overview.run->seconds, 0, 'g', 6).arg(overview.run->movedPixels, 0, 'f', 1);
        return QString("00_overview.png main_frame_index=%1 sigma=%2 active_frames=%3 changed_pixels=%4%5; display only, not evidence")
            .arg(overview.mainFrameIndex).arg(request.sigma, 0, 'g', 17).arg(overview.activeFrames).arg(overview.changedPixels).arg(run);
    }
    catch (const MemoryLimitError&)
    {
        return "not written: RAM limit";
    }
    catch (const std::exception& ex)
    {
        return QString("not written: %1").arg(QString::fromUtf8(ex.what()));
    }
}

// One line per candidate, saved or not. False when the log cannot be written.
bool EventWriter::appendCandidates(const Job& job, const ClipWriteResult& result, bool pending) const
{
    const EventClip& clip = job.clip;
    // The outcome comes last on the line, because a reason can contain spaces.
    const QString outcome = result.folder.isEmpty()
        ? QString("not_saved=%1").arg(result.notSaved)
        : QString("clip=%1").arg(QDir(outputRoot_).relativeFilePath(QFileInfo(result.folder).absoluteFilePath()));

    QString lines;
    QTextStream out(&lines);
    for (const ClipTrigger& trigger : clip.triggers)
    {
        out << utcText(trigger.triggerTime) << " ";
        if (pending) out << (trigger.kind==TriggerKind::Manual ? "manual_pending " : "candidate_pending ");
        out << triggerFields(trigger)
            << " mode=" << targetName(clip.captureSettings.target)
            << (duringMountCorrection(trigger.triggerTime) ? " during_mount_correction=yes" : "")
            << (job.containsTestSignal ? " contains_test_signal=yes" : "")
            << " record_id=" << job.recordId;
        if (!pending && !result.group.isEmpty())
            out << " group=" << result.group;
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
    if (!start.stream.sdkVersion.empty())
    {
        line+=QString(" device_id=\"%1\" transfer_speed=\"%2\" sensor_adc_bits=\"%3\"")
            .arg(orUnknown(start.stream.deviceId),orUnknown(start.stream.transferSpeed),orUnknown(start.stream.sensorBits));
        line += QString(" sdk=\"%1\" serial=\"%2\" exposure=\"%3\" gain=\"%4\" offset=\"%5\" binning=\"%6\" roi=\"%7\" sample_layout=\"%8\" usb_bandwidth=\"%9\" high_speed=\"%10\" frame_time=\"%11\" frame_index=\"%12\"")
            .arg(orUnknown(start.stream.sdkVersion),orUnknown(start.stream.serial),orUnknown(start.stream.exposure),orUnknown(start.stream.gain),orUnknown(start.stream.offset),
                orUnknown(start.stream.binning),orUnknown(start.stream.roi),orUnknown(start.stream.sampleLayout),orUnknown(start.stream.usbBandwidth),orUnknown(start.stream.highSpeed),
                orUnknown(start.stream.frameTime),orUnknown(start.stream.frameIndex));
    }
    if (!start.stream.playback.empty())
    {
        line += QString(" playback=\"%1\"").arg(QString::fromStdString(start.stream.playback));
    }
    // Feature-14: where and with what, and the last clock check. Whether the
    // window's testing tools were on: injected frames carry their own marks.
    line += siteFields(start.site);
    line += QString(" testing_tools=\"%1\"").arg(start.testingTools ? "on" : "off");
    line += QString(" clock_check=\"%1\"").arg(start.clockRecord.empty() ? QString("not made")
        : QString::fromStdString(start.clockRecord).replace('"', '\''));
    line += QString(" source=\"%1\"\n").arg(orUnknown(start.stream.source));
    return appendLog(line);
}

// Appends to candidates.txt, or to another log in the output folder. False
// when the log cannot be written.
bool EventWriter::appendLog(const QString& lines, const QString& fileName, const QString& folder) const
{
    const QDir location(folder.isEmpty() ? outputRoot_ : folder);
    if (!QDir().mkpath(location.path()))
    {
        return false;
    }

    QFile log(location.filePath(fileName));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return false;
    }

    QTextStream out(&log);
    out << lines;
    out.flush();
    return out.status() == QTextStream::Ok && log.flush();
}
