#include "session/session.h"

#include "session/clock_check.h"
#include "detection/target_defaults.h"
#include "recording/metadata_text.h"
#include "recording/output_paths.h"
#include "selenewatch_version.h"

#include <QtCore/QTimer>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>

#include <algorithm>
#include <format>
#include <utility>
#include <stdexcept>

Session::Session(QObject* context, QString outputRoot, Notifications notifications)
    : context_(context)
    , notifications_(std::move(notifications))
    , writer_(context, ::eventsFolder(outputRoot, captureSettings_.target), captureSettings_.memoryBytes * 2 / 5,
          [done = notifications_.clipWritten](const ClipWriteResult& result) {
              if (done)
              {
                  done(result);
              }
          })
    , capture_(
          captureSettings_,
          [this] { post(notifications_.candidateStarted); },
          // A finished clip goes straight from the capture to the writer, on
          // the source thread. The window is not in between.
          [this](EventClip clip) {
              clip.stream = streamInfo_;
              {
                  std::lock_guard lock(requestMutex_);
                  clip.site = site_;
                  clip.clockRecord = clockRecord_;
              }
              // Feature-14: the clip names the recording its frames fell in.
              if (recordingKnown_ && !clip.frames.empty() && clip.frames.front()->frameIndex <= recordingLastIndex_
                  && clip.frames.back()->frameIndex >= recordingFirstIndex_)
                  clip.duringRecording = recordingName_;
               if (!transitFolder_.isEmpty()) ++transitWrites_;
               writer_.submit(std::move(clip));
               if (!transitFolder_.isEmpty()) writer_.afterQueued([this] { --transitWrites_; });
          }, {}, memory_)
    , guider_(writer_, [this] { post(notifications_.lockChanged); })
    , outputRoot_(std::move(outputRoot))
    , recorder_([this](const RecordingWriter::Status& status) {
          // Writer thread: the recording ended, by a stop or by itself.
          // Evidence rule 5: Preview creates no candidate records.
          if (status.sessionMode == "monitoring")
          {
              writer_.logRecord(QString("%1 recording_stop name=%2 frames=%3 bytes=%4 seconds=%5 ended=\"%6\"\n")
                  .arg(utcText(std::chrono::system_clock::now()), status.name).arg(status.frames).arg(status.bytes)
                  .arg(status.streamSeconds, 0, 'f', 3).arg(QString::fromStdString(status.endReason)),
                QFileInfo(status.parentFolder).absolutePath());
          }
          post(notifications_.recordingChanged);
      })
    , skyTimelapse_([this](const SkyTimelapse::Status& status) {
          // Its writer thread: the time-lapse ended.
          const QString watchFolder = QFileInfo(status.folder).absolutePath();
          addToRecord(watchFolder, "session.txt", QString("%1 timelapse_stop frames=%2 skipped=%3 video=%4 reason=\"%5\"%6\n")
              .arg(utcText(std::chrono::system_clock::now())).arg(status.frames).arg(status.skipped)
              .arg(status.videoPath.isEmpty() ? "none" : "timelapse/timelapse.mp4", status.endReason,
                  status.problem.isEmpty() ? QString() : " problem=\"" + status.problem + "\""), false);
          if (auto ended = notifications_.watchTimelapseEnded) post([ended, status] { ended(status); });
      })
{
}

Session::~Session()
{
    stop();
    finishInterruptedWatch();
    waitTimeLapseFinished();
    guider_.shutdown();
    // A recording still being written is finished before the record closes.
    skyTimelapse_.finish("the program closed");
    skyTimelapse_.waitIdle();
    recorder_.stop("the program closed");
    recorder_.waitIdle();
    if (clockThread_.joinable()) clockThread_.join();
    writer_.shutdown();
}

bool Session::openWatch(QString& error)
{
    const bool newFolder = watchFolder_.isEmpty();
    if (watchFolder_.isEmpty())
    {
        const QDir parent(eventsRoot());
        const QString base = "session-" + utcStamp(std::chrono::system_clock::now());
        if (QDir().mkpath(parent.path()))
        {
            for (int n = 1; n <= 999; ++n)
            {
                const QString name = n == 1 ? base : base + "-" + QString::number(n);
                if (parent.mkdir(name)) { watchFolder_ = parent.filePath(name); break; }
                if (!parent.exists(name)) break;
            }
        }
        if (watchFolder_.isEmpty())
        {
            error = "Could not create a monitoring session under " + parent.absolutePath();
            return false;
        }
    }
    writer_.setOutputRoot(watchFolder_, true);
    watchIntervalOpen_ = true;
    watchStart_ = std::chrono::steady_clock::now();
    Watch watch;
    double timelapseSeconds = 0;
    {
        std::lock_guard lock(requestMutex_);
        watch = watch_;
        timelapseSeconds = watchTimelapseSeconds_;
    }
    writer_.logRecord(utcText(std::chrono::system_clock::now()) + " monitoring_start target="
        + targetName(captureSettings_.target) + " watch=" + watchName(watch) + "\n", watchFolder_, "session.txt");
    // Feature-36: one time-lapse per watch folder, across its restarts.
    if (newFolder && timelapseSeconds > 0)
    {
        skyTimelapse_.start(watchFolder_, timelapseSeconds, captureSettings_.diskReserveBytes);
        writer_.logRecord(utcText(std::chrono::system_clock::now()) + " timelapse_start interval_s="
            + QString::number(timelapseSeconds, 'g', 6) + " folder=timelapse\n", watchFolder_, "session.txt");
    }
    recordingKnown_ = false;
    return true;
}

void Session::closeWatch(const QString& reason, bool resume)
{
    if (watchFolder_.isEmpty()) return;
    if (watchIntervalOpen_)
    {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - watchStart_).count();
        writer_.logRecord(utcText(std::chrono::system_clock::now()) + " monitoring_stop seconds="
            + QString::number(seconds, 'f', 3) + " reason=" + reason + "\n", watchFolder_, "session.txt");
        watchIntervalOpen_ = false;
    }
    if (resume) return;
    skyTimelapse_.finish(reason);
    const QString folder = std::exchange(watchFolder_, QString{});
    {
        std::lock_guard lock(lastWatchMutex_);
        lastWatchFolder_ = folder;
    }
    // The recorder may still be flushing its SER. Its stop record and all
    // earlier clip outcomes must reach the event writer before finalization.
    recorder_.afterQueued([this, folder, done = notifications_.reportReady] {
        writer_.finishReport(folder, [done](QString path, QString error) { if (done) done(path, error); });
    });
    writer_.setOutputRoot(eventsRoot());
}

QString Session::lastWatchFolder() const
{
    std::lock_guard lock(lastWatchMutex_);
    return lastWatchFolder_;
}

void Session::addToRecord(const QString& folder, const QString& fileName, const QString& line, bool rewriteReport)
{
    if (folder.isEmpty()) return;
    // Behind the recorder, as closeWatch queues the report.
    recorder_.afterQueued([this, folder, fileName, line, rewriteReport] {
        writer_.logRecord(line, folder, fileName);
        if (rewriteReport) writer_.finishReport(folder, [this, folder](QString, QString error) { retryReport(folder, error, 5); });
    });
}

// Context thread. A report written moments ago can still be held open by
// another program (a virus scan), so replacing it is refused; it is tried
// again a second later, a few times.
void Session::retryReport(const QString& folder, const QString& error, int attemptsLeft)
{
    if (error.isEmpty() || attemptsLeft <= 0) return;
    QTimer::singleShot(1000, context_, [this, folder, attemptsLeft] {
        writer_.finishReport(folder, [this, folder, attemptsLeft](QString, QString error) {
            retryReport(folder, error, attemptsLeft - 1);
        });
    });
}

void Session::setSite(const ObservingSite& site)
{
    std::lock_guard lock(requestMutex_);
    site_ = site;
}

void Session::setTestingTools(bool on)
{
    std::lock_guard lock(requestMutex_);
    testingTools_ = on;
}

void Session::setWatch(Watch watch)
{
    std::lock_guard lock(requestMutex_);
    watch_ = watch;
}

void Session::setWatchTimelapse(double intervalSeconds)
{
    std::lock_guard lock(requestMutex_);
    watchTimelapseSeconds_ = std::max(0.0, intervalSeconds);
}

void Session::setOutputRoot(const QString& outputRoot)
{
    if (running_) throw std::logic_error("output folder must be changed while stopped");
    outputRoot_ = outputRoot;
    writer_.setOutputRoot(::eventsFolder(outputRoot_, captureSettings_.target));
}

QString Session::recordingFolder() const
{
    return ::recordingsFolder(outputRoot_);
}

QString Session::eventsRoot() const
{
    return ::eventsFolder(outputRoot_, captureSettings_.target);
}

QString Session::timelapsesFolder() const
{
    return ::timelapsesFolder(outputRoot_);
}

void Session::setClockCheck(bool enabled, std::string ntpServer)
{
    std::lock_guard lock(requestMutex_);
    clockCheck_ = enabled;
    ntpServer_ = std::move(ntpServer);
}

std::string Session::clockRecord() const
{
    std::lock_guard lock(requestMutex_);
    return clockRecord_;
}

// Context thread, at Start. One check at a time; a Start during one keeps it.
void Session::startClockCheck()
{
    std::string server;
    {
        std::lock_guard lock(requestMutex_);
        if (!clockCheck_) return;
        server = ntpServer_;
    }
    if (clockBusy_.exchange(true)) return;
    if (clockThread_.joinable()) clockThread_.join();
    clockThread_ = std::jthread([this, server] {
        const std::string windows = windowsTimeStatus();
        std::optional<NtpResult> ntp;
        if (!server.empty()) ntp = queryNtp(server, 123, 4, std::chrono::milliseconds{800});
        const std::string text = clockCheckText(windows, ntp ? &*ntp : nullptr, std::chrono::system_clock::now());
        {
            std::lock_guard lock(requestMutex_);
            clockRecord_ = text;
        }
        clockBusy_.store(false);
    });
}

bool Session::requestRecording(bool record, uint64_t maxFrames)
{
    std::lock_guard lock(requestMutex_);
    if (!running_ || sourceFinished_.load() || mode_.load() == Mode::TimeLapse || previewWatchRequest_) return false;
    recordRequest_ = record;
    recordMaxFrames_ = record ? maxFrames : 0;
    return true;
}

void Session::scheduleRecording(Schedule schedule)
{
    std::lock_guard lock(requestMutex_);
    schedule_ = schedule;
}

void Session::cancelSchedule()
{
    std::lock_guard lock(requestMutex_);
    schedule_.reset();
}

std::optional<Session::Schedule> Session::schedule() const
{
    std::lock_guard lock(requestMutex_);
    return schedule_;
}

RecordingWriter::Status Session::recordingStatus() const
{
    return recorder_.status();
}

// Source thread, for each published frame: a request, a schedule, or the
// end of Monitoring starts or ends the recording; a running one takes the frame.
void Session::updateRecording(const FramePtr& frame)
{
    std::optional<bool> request;
    uint64_t requestMaxFrames = 0;
    std::optional<Schedule> schedule;
    {
        std::lock_guard lock(requestMutex_);
        request = recordRequest_;
        recordStarting_ = request.value_or(false);
        requestMaxFrames = recordMaxFrames_;
        recordRequest_.reset();
        recordMaxFrames_ = 0;
        schedule = schedule_;
    }
    const bool monitoring = mode_.load() == Mode::Monitoring;
    if (recording_)
    {
        if (scheduledRecording_ && !monitoring) endRecording("monitoring stopped");
        else if (request && !*request) endRecording("stopped by the observer");
        else if (scheduledRecording_ && schedule && frame->timestamp >= schedule->end)
        {
            endRecording("the scheduled end");
            std::lock_guard lock(requestMutex_);
            if (schedule_ && schedule_->end == schedule->end) schedule_.reset();
        }
    }
    if (recording_)
    {
        if (recorder_.push(frame))
        {
            recordingLastIndex_ = frame->frameIndex;
            ++recordingFrames_;
            if (recordingMaxFrames_ > 0 && recordingFrames_ >= recordingMaxFrames_)
                endRecording("reached frame limit");
        }
        else
        {
            // It ended by itself; its writer has the reason and reports it.
            recording_ = false;
            scheduledRecording_ = false;
        }
        if (request.value_or(false)) { std::lock_guard lock(requestMutex_); recordStarting_ = false; }
        return;
    }
    if (request && *request)
    {
        beginRecording(frame, "the observer", requestMaxFrames);
        { std::lock_guard lock(requestMutex_); recordStarting_ = false; }
        return;
    }
    if (!monitoring) return;
    if (schedule && frame->timestamp >= schedule->start)
    {
        if (frame->timestamp < schedule->end)
        {
            beginRecording(frame, std::format("a schedule from {} for {:.1f} s", utcText(schedule->start).toStdString(),
                std::chrono::duration<double>(schedule->end - schedule->start).count()));
            scheduledRecording_ = recording_;
        }
        else
        {
            // Its end passed while not Monitoring.
            std::lock_guard lock(requestMutex_);
            if (schedule_ && schedule_->end == schedule->end) schedule_.reset();
            post(notifications_.recordingChanged);
        }
    }
}

void Session::beginRecording(const FramePtr& first, const std::string& startedBy, uint64_t maxFrames)
{
    RecordingWriter::Start start;
    start.folder = mode_.load() == Mode::Monitoring ? QDir(watchFolder_).filePath("recordings") : recordingFolder();
    start.stream = streamInfo_;
    {
        std::lock_guard lock(requestMutex_);
        start.site = site_;
        start.clockRecord = clockRecord_;
    }
    start.target = captureSettings_.target;
    start.sessionMode = mode_.load() == Mode::Monitoring ? "monitoring" : "preview";
    start.maxFrames = maxFrames;
    start.diskReserveBytes = captureSettings_.diskReserveBytes;
    start.startedBy = startedBy;
    start.queueBytes = recordingQueueBytes_ ? recordingQueueBytes_ : captureSettings_.memoryBytes / 8;
    if (!recorder_.start(start, first)) return;
    recording_ = true;
    recordingFrames_ = 1;
    recordingMaxFrames_ = maxFrames;
    recordingFirstIndex_ = recordingLastIndex_ = first->frameIndex;
    if (mode_.load() == Mode::Monitoring)
    {
        recordingKnown_ = true;
        recordingName_ = utcStamp(first->timestamp).toStdString();
        writer_.logRecord(QString("%1 recording_start name=%2 first_frame_index=%3 started_by=\"%4\" folder=\"%5\"\n")
            .arg(utcText(first->timestamp), QString::fromStdString(recordingName_)).arg(first->frameIndex)
            .arg(QString::fromStdString(startedBy), QDir::toNativeSeparators(start.folder)));
    }
    if (recordingMaxFrames_ > 0 && recordingFrames_ >= recordingMaxFrames_)
        endRecording("reached frame limit");
    post(notifications_.recordingChanged);
}

void Session::endRecording(const std::string& reason)
{
    if (!recording_) return;
    recorder_.stop(reason);
    recording_ = false;
    scheduledRecording_ = false;
}

void Session::setSource(std::unique_ptr<FrameSource> source)
{
    stop();
    source_ = std::move(source);
}

void Session::setCaptureSettings(CaptureSettings settings)
{
    if (running_ || !settings.valid()) throw std::invalid_argument("capture settings must be valid and changed while stopped");
    captureSettings_ = settings;
    writer_.setOutputRoot(::eventsFolder(outputRoot_, captureSettings_.target));
}

bool Session::start(QString& error, Mode mode)
{
    if (mode == Mode::Stopped) throw std::invalid_argument("start in Preview or Monitoring");
    if (running_)
    {
        return true;
    }
    if (!source_)
    {
        error = "no source is selected";
        return false;
    }

    capture_.reset();
    (void)previewSlot_.take();
    source_->setMemoryBudget(memory_);
    if (!memory_.setLimit(captureSettings_.memoryBytes))
    {
        error = "RAM ceiling is below buffers still held by the writer or display; wait for them to drain";
        return false;
    }
    budgetPrepared_ = false;
    std::string reason;
    if (!source_->open(reason))
    {
        error = QString::fromStdString(reason);
        return false;
    }

    { std::lock_guard lock(requestMutex_); streamInfo_ = source_->streamInfo(); }
    acquiredFrames_.store(0, std::memory_order_relaxed);
    droppedFrames_.store(0); dropsKnown_.store(false);
    sourceFinished_.store(false);
    { std::lock_guard lock(requestMutex_); manualRequest_.reset(); monitoringRequest_.reset(); cameraTuned_ = false; }
    capture_.reset();
    resetStabilizer();
    captureResetRequested_.store(false);
    capture_.setMonitoring(mode == Mode::Monitoring);
    mode_.store(mode);
    settlingUntil_ = {};
    blip_.reset();
    lastPreviewTime_ = {};
    (void)previewSlot_.take();

    if (mode == Mode::Monitoring && !openWatch(error))
    {
        source_->stop();
        mode_.store(Mode::Stopped);
        return false;
    }
    startClockCheck();
    if (mode == Mode::Monitoring)
    {
        std::lock_guard lock(requestMutex_);
        writer_.logSessionStart(streamInfo_, site_, clockRecord_, testingTools_);
    }
    // A held lock carries over; its estimate starts again with this run.
    // Guiding is off at every start (safety rule 1).
    guider_.discontinuity();
    guider_.endGuiding("The stream started. Guiding is off at every start; turn it on again.");
    source_->start(*this);
    running_ = true;
    return true;
}

void Session::stop(std::string reason, bool resumeWatch)
{
    if (!running_)
    {
        return;
    }
    std::shared_ptr<TimelapseSession> tl;
    {
        std::lock_guard lock(timelapseMutex_);
        if (timelapse_)
        {
            tl = std::move(timelapse_);
            finishingTimelapses_.emplace_back(activeTimelapseId_, tl);
            activeTimelapseId_ = 0;
        }
    }
    if (tl) tl->stop(reason);
    source_->stop();
    // Do not block the GUI thread on FFmpeg finalization here; tl stays alive
    // in finishingTimelapses_ until its queued finished callback cleans it up
    // (or ~Session / waitTimeLapseFinished joins it).
    // The source thread is joined, so its recording state is this thread's now.
    endRecording("the stream stopped");
    // Safety rule 8: no guiding without frames. Only pulses were sent, so nothing is undone.
    guider_.endGuiding("The stream stopped (" + reason + ").");
    applyManualSave();
    if (tuneWatch_.busy())
    {
        tuneWatch_.stopped("the stream stopped");
        post(notifications_.autoTuneChanged);
    }
    capture_.finishPartial(reason);
    closeTransitWatch(reason);
    closeWatch(QString::fromStdString(reason), resumeWatch);
    blip_.reset();
    running_ = false;
    mode_.store(Mode::Stopped);
    {
        std::lock_guard lock(requestMutex_);
        monitoringRequest_.reset();
        previewWatchRequest_.reset();
        recordRequest_.reset();
        recordStarting_ = false;
    }
}

bool Session::requestMonitoring(bool monitoring)
{
    std::lock_guard lock(requestMutex_);
    if (!running_ || sourceFinished_.load() || monitoringRequest_ || previewWatchRequest_
        || mode_.load() == Mode::TimeLapse || (monitoring && cameraTuned_)) return false;
    monitoringRequest_ = monitoring;
    return true;
}

Session::Mode Session::mode() const { return mode_.load(); }
bool Session::modeChangePending() const
{
    std::lock_guard lock(requestMutex_);
    return monitoringRequest_.has_value() || previewWatchRequest_.has_value();
}

bool Session::canChangePreviewWatchLocked() const
{
    return running_ && mode_.load() == Mode::Preview && !sourceFinished_.load()
        && !monitoringRequest_ && !previewWatchRequest_ && !recordRequest_ && !recordStarting_ && !recorder_.status().active;
}

bool Session::canChangePreviewWatch() const
{
    std::lock_guard lock(requestMutex_);
    return canChangePreviewWatchLocked();
}

bool Session::previewWatchChangePending() const
{
    std::lock_guard lock(requestMutex_);
    return previewWatchRequest_.has_value();
}

bool Session::requestPreviewWatch(Watch watch, const DetectionSettings& detection, CaptureSettings capture)
{
    if (!detection.valid() || !capture.valid() || capture.target != watchTarget(watch)) return false;
    std::lock_guard lock(requestMutex_);
    if (!canChangePreviewWatchLocked()) return false;
    previewWatchRequest_ = PreviewWatch{watch, detection, capture};
    return true;
}

bool Session::applyPreviewWatch()
{
    {
        std::lock_guard lock(requestMutex_);
        if (!previewWatchRequest_) return true;
        const auto& next = *previewWatchRequest_;
        capture_.reset();
        resetStabilizer();
        (void)previewSlot_.take();
        // An older evidence job may still own buffers. Leave the change queued
        // until those readers drain if the observer chose a lower ceiling.
        if (captureSettings_.memoryBytes != next.capture.memoryBytes && !memory_.setLimit(next.capture.memoryBytes))
            return false;
        captureSettings_ = next.capture;
        watch_ = next.watch;
        detectionSettings_ = next.detection;
        budgetPrepared_ = false;
        previewWatchRequest_.reset();
    }
    lastPreviewTime_ = {};
    blip_.reset();
    writer_.setOutputRoot(eventsRoot());
    cancelAutoTune("The observing watch changed.");
    post(notifications_.modeChanged);
    return true;
}

void Session::applyModeRequest()
{
    // Keep the request pending until capture has crossed the boundary, so
    // the context cannot enable camera edits while an old clip is open.
    std::optional<bool> request;
    {
        std::lock_guard lock(requestMutex_);
        request = monitoringRequest_;
        // A queued camera change can arrive after the context requests arming.
        // Recheck at the actual frame boundary as well as at request time.
        if (request && *request && cameraTuned_)
        {
            monitoringRequest_.reset();
            post(notifications_.modeChanged);
            return;
        }
    }
    if (!request) return;
    if (*request && mode_.load() != Mode::Monitoring)
    {
        QString error;
        if (!openWatch(error))
        {
            { std::lock_guard lock(requestMutex_); monitoringRequest_.reset(); }
            if (auto done = notifications_.reportReady) post([done, error] { done({}, error); });
            post(notifications_.modeChanged);
            return;
        }
    }
    if (recording_)
        endRecording(*request ? "monitoring started" : "monitoring stopped");
    applyManualSave();
    capture_.setMonitoring(*request);
    if (*request && mode_.load() != Mode::Monitoring)
    {
        std::lock_guard lock(requestMutex_);
        writer_.logSessionStart(streamInfo_, site_, clockRecord_, testingTools_);
    }
    if (!*request) closeWatch("monitoring stopped");
    mode_.store(*request ? Mode::Monitoring : Mode::Preview);
    { std::lock_guard lock(requestMutex_); monitoringRequest_.reset(); }
    post(notifications_.modeChanged);
}

bool Session::requestBlip(const BlipSettings& settings)
{
    return running_ && !sourceFinished_.load() && blip_.request(settings);
}

bool Session::blipBusy() const
{
    return blip_.busy();
}

uint64_t Session::startAutoTune(TuneWatchRequest request)
{
    if (!running_ || sourceFinished_.load() || mode_.load() != Mode::Preview || modeChangePending()) return 0;
    // A flash check's test flash must never be written into a recording.
    if ((request.kind == TuneWatchRequest::Kind::FlashCheck || request.kind == TuneWatchRequest::Kind::Footprint)
        && (recorder_.status().active || blip_.busy())) return 0;
    const uint64_t id = tuneWatch_.start(std::move(request));
    post(notifications_.autoTuneChanged);
    return id;
}

void Session::cancelAutoTune(const std::string& reason)
{
    if (!tuneWatch_.busy()) return;
    tuneWatch_.cancel(reason);
    post(notifications_.autoTuneChanged);
}

bool Session::requestManualSave()
{
    std::lock_guard<std::mutex> lock(requestMutex_);
    if (!running_ || sourceFinished_.load() || mode_.load() != Mode::Monitoring || monitoringRequest_ || manualRequest_) return false;
    manualRequest_=std::chrono::system_clock::now();
    return true;
}

bool Session::manualSavePending() const
{
    std::lock_guard<std::mutex> lock(requestMutex_);
    return manualRequest_.has_value();
}

void Session::applyManualSave()
{
    std::optional<std::chrono::system_clock::time_point> request;
    { std::lock_guard<std::mutex> lock(requestMutex_); request=manualRequest_; manualRequest_.reset(); }
    if (!request) return;
    const bool saved=capture_.saveNow(*request);
    if (notifications_.manualSaveHandled)
        post([done=notifications_.manualSaveHandled,saved]{ done(saved); });
}

bool Session::isRunning() const
{
    return running_;
}

bool Session::sourceFinished() const
{
    return sourceFinished_.load();
}

StreamInfo Session::streamInfo() const
{
    std::lock_guard lock(requestMutex_);
    return streamInfo_;
}

uint64_t Session::acquiredFrames() const
{
    return acquiredFrames_.load(std::memory_order_relaxed);
}

std::optional<uint64_t> Session::droppedFrames() const
{
    if (!dropsKnown_.load()) return {};
    return droppedFrames_.load();
}

size_t Session::bufferSize() const
{
    return capture_.bufferSize();
}

RingUsage Session::historyUsage() const { return capture_.historyUsage(); }
MemoryUsage Session::memoryUsage() const { return memory_.usage(); }
WriterUsage Session::writerUsage() const { return writer_.usage(); }

PreviewFrame Session::takePreview()
{
    return previewSlot_.take();
}

void Session::setDetectionSettings(const DetectionSettings& settings)
{
    if (!settings.valid()) throw std::invalid_argument("invalid detection settings");
    std::lock_guard<std::mutex> lock(requestMutex_);
    if (previewWatchRequest_) previewWatchRequest_->detection = settings;
    detectionSettings_ = settings;
}

void Session::setDetectionDisplay(DetectionOverlay overlay, int probeX, int probeY)
{
    std::lock_guard<std::mutex> lock(requestMutex_);
    displayRequest_.overlay = overlay;
    displayRequest_.probeX = probeX;
    displayRequest_.probeY = probeY;
}

bool Session::startTimeLapse(const TimelapseSettings& settings, QString& error,
    FreeSpaceQuery freeSpace, std::chrono::milliseconds stackDelay, std::optional<DetectionSettings> transitDetection)
{
    if (!settings.valid())
    {
        error = "invalid time-lapse settings";
        return false;
    }
    if (mode_.load() == Mode::Monitoring || modeChangePending() || recordingStatus().active
        || (running_ && sourceFinished()))
    {
        error = "stop Monitoring and recording before starting a time-lapse";
        return false;
    }
    if (settings.watchTransits)
    {
        if (captureSettings_.target != Target::Moon || (transitDetection && !transitDetection->valid()))
        {
            error = "choose a Moon watch with valid transit settings";
            return false;
        }
        transitDetection = settingsForWatch(transitDetection.value_or(defaultDetectionSettings(Watch::MoonShadows)),
            Watch::MoonShadows);
    }
    else transitDetection.reset();
    uint64_t tlId = 0;
    {
        std::lock_guard lock(timelapseMutex_);
        if (timelapse_ || !finishingTimelapses_.empty())
        {
            error = "a time-lapse is still active or finalizing";
            return false;
        }
        tlId = ++nextTimelapseId_;
    }
    if (!running_)
    {
        if (!start(error, Mode::Preview)) return false;
    }
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = timelapsesFolder();
    cfg.settings = settings;
    {
        std::lock_guard lock(requestMutex_);
        cfg.stream = streamInfo_;
        cfg.site = site_;
    }
    cfg.softwareVersion = std::string(SELENEWATCH_VERSION) + " (" + SELENEWATCH_REVISION + ")";
    cfg.diskReserveBytes = captureSettings_.diskReserveBytes;
    cfg.memory = memory_;
    const auto gStatus = guider_.status();
    if (gStatus.lock.state == LockState::Locked)
        cfg.lockRegion = gStatus.lock.region;
    cfg.freeSpace = std::move(freeSpace);
    if (settings.watchTransits) cfg.beforeBurst = [this] { capture_.reset(); resetStabilizer(); };
    cfg.statusChanged = [this](TimelapseStatus status) {
        status.transitWatch = transitState_.load();
        if (notifications_.timeLapseChanged) notifications_.timeLapseChanged(status);
    };
    cfg.finished = [this, tlId](const TimelapseFinishResult& res) { finishTimeLapse(tlId, res); };

    auto tl = std::make_shared<TimelapseSession>(&timeLapseCallbacks_, std::move(cfg));
    if (!tl->valid())
    {
        error = tl->initError();
        return false;
    }
    if (stackDelay.count() > 0) tl->setStackDelay(stackDelay);
    {
        std::lock_guard lock(timelapseMutex_);
        timelapse_ = tl;
        activeTimelapseId_ = tlId;
        transitDetection_ = std::move(transitDetection);
        transitDraining_ = transitDetection_.has_value();
    }
    transitState_.store(TransitWatchState::Off);
    captureResetRequested_.store(true);
    mode_.store(Mode::TimeLapse);
    post(notifications_.modeChanged);
    return true;
}

void Session::finishTimeLapse(uint64_t id, TimelapseFinishResult result)
{
    bool clearedActive = false;
    {
        std::lock_guard lock(timelapseMutex_);
        if (transitDraining_)
        {
            transitFinish_.emplace(id, std::move(result));
            return;
        }
        if (activeTimelapseId_ == id)
        {
            timelapse_.reset();
            activeTimelapseId_ = 0;
            clearedActive = true;
        }
        std::erase_if(finishingTimelapses_, [id](const auto& entry) { return entry.first == id; });
    }
    if (clearedActive && running_ && mode_.load() == Mode::TimeLapse) mode_.store(Mode::Preview);
    const QString transitFolder = QDir(result.sessionDir).filePath("transits");
    if (QFileInfo::exists(transitFolder)) result.transitFolder = transitFolder;
    post(notifications_.modeChanged);
    if (notifications_.timeLapseFinished) notifications_.timeLapseFinished(result);
}

void Session::stopTimeLapse(const std::string& reason)
{
    std::shared_ptr<TimelapseSession> tl;
    {
        std::lock_guard lock(timelapseMutex_);
        tl = timelapse_;
    }
    if (tl) tl->stop(reason);
    // Keep Mode::TimeLapse until the worker finishes finalizing and cfg.finished runs,
    // preventing overlapping sessions or premature mode changes while FFmpeg is encoding.
    post(notifications_.modeChanged);
}

void Session::waitTimeLapseFinished()
{
    std::vector<std::shared_ptr<TimelapseSession>> pending;
    {
        std::lock_guard lock(timelapseMutex_);
        if (timelapse_) pending.push_back(timelapse_);
        for (const auto& entry : finishingTimelapses_)
            if (entry.second) pending.push_back(entry.second);
    }
    for (const auto& tl : pending) tl->waitFinished();
}

void Session::cancelTimeLapseExport()
{
    std::vector<std::shared_ptr<TimelapseSession>> all;
    {
        std::lock_guard lock(timelapseMutex_);
        if (timelapse_) all.push_back(timelapse_);
        for (const auto& entry : finishingTimelapses_)
            if (entry.second) all.push_back(entry.second);
    }
    for (const auto& tl : all) tl->cancelExport();
}

bool Session::timeLapseFinalizing() const
{
    std::lock_guard lock(timelapseMutex_);
    if (!finishingTimelapses_.empty()) return true;
    return timelapse_ && !timelapse_->active();
}

TimelapseStatus Session::timeLapseStatus() const
{
    std::lock_guard lock(timelapseMutex_);
    TimelapseStatus status;
    if (timelapse_) status = timelapse_->status();
    else if (!finishingTimelapses_.empty()) status = finishingTimelapses_.back().second->status();
    status.transitWatch = transitState_.load();
    return status;
}

void Session::openTransitWatch(const QString& parent)
{
    std::lock_guard lock(timelapseMutex_);
    if (!transitDetection_ || !transitFolder_.isEmpty()) return;
    transitFolder_ = QDir(parent).filePath("transits");
    capture_.reset();
    resetStabilizer();
    capture_.setDetectionSettings(*transitDetection_);
    capture_.setMonitoring(true);
    writer_.setOutputRoot(transitFolder_, true);
    std::lock_guard requestLock(requestMutex_);
    writer_.logSessionStart(streamInfo_, site_, clockRecord_, testingTools_);
}

void Session::setTransitState(TransitWatchState state, std::chrono::system_clock::time_point utc)
{
    const auto previous = transitState_.exchange(state);
    if (previous == state) return;
    const auto watching = [](TransitWatchState value) {
        return value == TransitWatchState::Watching || value == TransitWatchState::Saving;
    };
    QString line;
    if (state == TransitWatchState::Finished) line = "transit_end";
    else if (watching(state) && !watching(previous)) line = "transit_resume";
    else if (!watching(state) && (watching(previous) || previous == TransitWatchState::Off))
        line = "transit_pause reason=time-lapse";
    if (!line.isEmpty())
        writer_.logRecord(utcText(utc) + " " + line + "\n", transitFolder_, "session.txt");
}

void Session::updateTransitWatch(const FramePtr& frame, TimelapseWork work)
{
    if (transitFolder_.isEmpty()) return;
    transitFrameUtc_ = frame->timestamp;
    if (work != TimelapseWork::Idle)
    {
        setTransitState(work == TimelapseWork::Capturing ? TransitWatchState::Capturing : TransitWatchState::Stacking,
            frame->timestamp);
        return;
    }
    bool stabilizeTransit = false;
    {
        std::lock_guard lock(timelapseMutex_);
        stabilizeTransit = transitDetection_ && transitDetection_->stabilizeDetection;
    }
    const FrameShift shift = stepStabilizer(*frame, stabilizeTransit, transitFolder_);
    capture_.onFrame(frame, {}, shift);
    setTransitState(capture_.collectingClip() || transitWrites_.load() ? TransitWatchState::Saving : TransitWatchState::Watching,
        frame->timestamp);
}

void Session::closeTransitWatch(const std::string& reason)
{
    {
        std::lock_guard lock(timelapseMutex_);
        if (!transitDetection_) return;
        transitDetection_.reset();
    }
    if (!transitFolder_.isEmpty())
    {
        capture_.finishPartial(reason);
        capture_.setMonitoring(false);
        capture_.reset();
        resetStabilizer();
        setTransitState(TransitWatchState::Finished, transitFrameUtc_);
        transitFolder_.clear();
        writer_.setOutputRoot(eventsRoot());
    }
    writer_.afterQueued([this] {
        QMetaObject::invokeMethod(&timeLapseCallbacks_, [this] {
            std::optional<std::pair<uint64_t, TimelapseFinishResult>> pending;
            {
                std::lock_guard lock(timelapseMutex_);
                transitDraining_ = false;
                pending = std::exchange(transitFinish_, std::nullopt);
            }
            if (pending) finishTimeLapse(pending->first, std::move(pending->second));
        }, Qt::QueuedConnection);
    });
}

void Session::resetStabilizer()
{
    stabilizer_.reset();
    prevStabilizeEnabled_ = false;
    lastStabilizerTime_ = {};
}

FrameShift Session::stepStabilizer(const Frame& frame, bool enabled, const QString& logFolder)
{
    if (enabled != prevStabilizeEnabled_)
    {
        stabilizer_.reset();
        lastStabilizerTime_ = {};
        capture_.resetDetector(false);
        prevStabilizeEnabled_ = enabled;
    }
    if (!enabled) return {};

    double dt = lastStabilizerTime_.time_since_epoch().count() != 0
        ? std::chrono::duration<double>(frame.timestamp - lastStabilizerTime_).count()
        : (1.0 / 30.0);
    lastStabilizerTime_ = frame.timestamp;

    std::optional<LockRegion> preferred;
    const GuiderStatus gStatus = guider_.status();
    if (gStatus.lock.state == LockState::Locked) preferred = gStatus.lock.region;

    const FrameStabilizer::StepResult res = stabilizer_.step(frame, dt, preferred);
    if (res.resetDetector)
        capture_.resetDetector(false);
    else if (res.recenterStepX != 0 || res.recenterStepY != 0)
        capture_.recenterDetector(res.recenterStepX, res.recenterStepY, frame, res.shift);

    if (!res.logEvent.empty() && !logFolder.isEmpty())
        writer_.logRecord(utcText(frame.timestamp) + " " + QString::fromStdString(res.logEvent) + "\n", logFolder, "session.txt");

    return res.shift;
}

void Session::prepareCaptureBudget(const Frame& frame)
{
    if (!budgetPrepared_)
    {
        // Reserve for mean/variance, two maps/differences, GUI/writer images,
        // incoming/spare frames, and bounded sparse work before sharing the
        // rest between the open clip and all writer-held clips.
        const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
        const size_t working = std::max(captureSettings_.memoryBytes / 5,
            pixels * 32 + frame.data.capacity() * 5 + size_t{4} * 1024 * 1024);
        if (working >= captureSettings_.memoryBytes) throw std::runtime_error("RAM ceiling too small for this frame layout and processing reserve; increase it");
        const size_t half = (captureSettings_.memoryBytes - working) / 2;
        const size_t previewAndNextFrame = pixels * sizeof(float) + frame.data.capacity();
        if (previewAndNextFrame >= half) throw std::runtime_error("RAM ceiling leaves too little room for a frame and its difference; increase it");
        capture_.configure(captureSettings_, half - previewAndNextFrame, half / 2);
        writer_.setByteLimit(half);
        // A recording's backlog mostly shares its frames with the ring; past
        // the ring's allowance the disk is not keeping up (Feature-14).
        recordingQueueBytes_ = half / 2;
        const size_t sized = pixels * 32 + frame.data.capacity() * 5 + size_t{4} * 1024 * 1024;
        tuneSpareBytes_.store(working > sized ? working - sized : 0);
        budgetPrepared_ = true;
    }
}

// From the moment the frame is held as a FramePtr it is published: nobody
// writes it again. Test injection is the last write before publication.
void Session::onFrame(std::shared_ptr<Frame> frame)
{
    const bool watchReady = applyPreviewWatch();
    applyModeRequest();
    acquiredFrames_.fetch_add(1, std::memory_order_relaxed);
    if (frame->sourceDroppedFrames) droppedFrames_.store(*frame->sourceDroppedFrames);
    dropsKnown_.store(frame->sourceDroppedFrames.has_value());
    if (!watchReady) return;
    prepareCaptureBudget(*frame);

    if (mode_.load() == Mode::TimeLapse)
    {
        std::shared_ptr<TimelapseSession> activeTl;
        {
            std::lock_guard lock(timelapseMutex_);
            activeTl = timelapse_;
        }
        if (tuneWatch_.busy())
        {
            tuneWatch_.stopped("a time-lapse started");
            post(notifications_.autoTuneChanged);
        }
        if (captureResetRequested_.exchange(false)) capture_.reset();
        const FramePtr published = std::move(frame);
        if (activeTl && activeTl->active())
        {
            if (transitFolder_.isEmpty()) openTransitWatch(activeTl->sessionDir());
            const TimelapseWork work = activeTl->onFrame(published, capture_.collectingClip() || transitWrites_.load());
            if (activeTl->active()) updateTransitWatch(published, work);
            else closeTransitWatch("time-lapse capture ended");
            const GuiderStatus gs = guider_.status();
            activeTl->setGuidingStatus(gs, gs.guide.mode == GuideMode::Guiding);
        }
        else closeTransitWatch("time-lapse capture ended");
        guider_.offer(published, !transitFolder_.isEmpty());
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPreviewTime_ >= std::chrono::milliseconds{50})
        {
            lastPreviewTime_ = now;
            if (previewSlot_.put(published, nullptr)) post(notifications_.previewReady);
        }
        return;
    }

    if (captureResetRequested_.exchange(false))
    {
        capture_.reset();
        resetStabilizer();
    }

    // Feature-26: a flash check's test flash never goes into a recording,
    // even one that starts with this frame.
    if (tuneWatch_.injecting())
    {
        bool recordingAsked = false;
        { std::lock_guard lock(requestMutex_); recordingAsked = recordRequest_.value_or(false); }
        if (recording_ || recordingAsked)
        {
            blip_.reset();
            tuneWatch_.cancel("A recording started: the test flash would have been written into it.");
        }
    }
    blip_.apply(*frame);
    const FramePtr published = std::move(frame);
    updateRecording(published);
    if (mode_.load() == Mode::Monitoring) skyTimelapse_.offer(*published);

    DetectionSettings settings;
    DetectionDisplayRequest display;
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        settings = detectionSettings_;
        display = displayRequest_;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool settling = now < settlingUntil_;
    // Analyse every delivered tuning frame, but do not let a delayed SDK
    // brightness step hold a patch reference through the settling interval.
    if (settling) { capture_.reset(false); resetStabilizer(); }
    const bool previewDue = now - lastPreviewTime_ >= std::chrono::milliseconds{50};
    display.makeMap = previewDue;
    capture_.setDetectionSettings(settings);
    const bool stabilizeNow = !settling && settings.stabilizeDetection && captureSettings_.target != Target::Sky;
    const FrameShift shift = stepStabilizer(*published, stabilizeNow,
        mode_.load() == Mode::Monitoring ? watchFolder_ : QString());
    capture_.onFrame(published, display, shift);
    // Feature-26: auto-tune measures after the live detector learned from the frame.
    if (tuneWatch_.busy())
    {
        const AutoTuneWatch::Context context{settings, mode_.load() == Mode::Preview, settling, tuneSpareBytes_.load()};
        if (const auto flash = tuneWatch_.onFrame(*published, capture_.detector(), capture_.detectionCandidates(), context))
        {
            if (recording_) tuneWatch_.cancel("A recording is running: the test flash would be written into it.");
            else if (!blip_.request(*flash)) tuneWatch_.cancel("Another test flash is in progress.");
        }
        if (tuneWatch_.takeChanged(now)) post(notifications_.autoTuneChanged);
    }
    applyManualSave();
    // The guider keeps only the newest frame and does nothing when no lock is wanted.
    guider_.offer(published, mode_.load() == Mode::Monitoring);

    // The preview converts on the context thread. It is woken once per frame
    // it has not yet taken, and skips frames when it falls behind.
    if (previewDue || capture_.detectionSummary().triggered)
    {
        lastPreviewTime_ = now;
        auto snapshot = capture_.detectionView();
        snapshot.monitoring = mode_.load() == Mode::Monitoring;
        snapshot.settling = settling;
        auto view = std::make_shared<const DetectionView>(std::move(snapshot));
        if (previewSlot_.put(published, std::move(view))) post(notifications_.previewReady);
    }
}

// The next frame is not the sky a moment later (a loop starting again). The
// jump would look like a sudden change, so the capture starts afresh.
void Session::onDiscontinuity()
{
    applyManualSave();
    capture_.finishPartial("playback loop boundary");
    capture_.reset();
    resetStabilizer();
    guider_.discontinuity();
    // Feature-26: a repeated clip is not a longer observation.
    if (tuneWatch_.busy())
    {
        tuneWatch_.sourceInterrupted("the recording jumped or started again from its beginning");
        post(notifications_.autoTuneChanged);
    }
}

void Session::onSettingsChanged(const StreamInfo& info, std::chrono::duration<double> settling)
{
    if (mode_.load() != Mode::Preview)
        throw std::logic_error("camera settings changed outside Preview; stopping to preserve clip metadata");
    capture_.reset();
    blip_.reset();
    (void)previewSlot_.take();
    lastPreviewTime_ = {};
    settlingUntil_ = std::chrono::steady_clock::now()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(settling);
    { std::lock_guard lock(requestMutex_); streamInfo_ = info; cameraTuned_ = true; }
    // Feature-26: a measurement belongs to the camera settings it started with.
    tuneWatch_.cancel("The camera settings changed during the measurement.");
    post(notifications_.cameraSettingsChanged);
}

void Session::onIdle()
{
    applyPreviewWatch();
    applyModeRequest();
    if (timeLapseFinalizing()) closeTransitWatch("time-lapse capture ended");
}

void Session::onEnded()
{
    sourceFinished_.store(true);
    std::shared_ptr<TimelapseSession> tl;
    {
        std::lock_guard lock(timelapseMutex_);
        tl = timelapse_;
    }
    if (tl) tl->stop("source ended");
    applyManualSave();
    endRecording("the source ended");
    capture_.finishPartial("source ended");
    closeTransitWatch("source ended");
    closeWatch("source ended");
    if (tuneWatch_.busy())
    {
        tuneWatch_.sourceInterrupted("the recording ended");
        post(notifications_.autoTuneChanged);
    }
    post(notifications_.ended);
}

void Session::onFailed(const std::string& message)
{
    sourceFinished_.store(true);
    std::shared_ptr<TimelapseSession> tl;
    {
        std::lock_guard lock(timelapseMutex_);
        tl = timelapse_;
    }
    if (tl) tl->stop("source failure: " + message);
    applyManualSave();
    endRecording("source failure: " + message);
    capture_.finishPartial("source failure: " + message);
    closeTransitWatch("source failure: " + message);
    closeWatch("source failure: " + QString::fromStdString(message));
    const QString text = QString::fromStdString(message);
    if (notifications_.failed)
    {
        QMetaObject::invokeMethod(context_, [failed = notifications_.failed, text] {
            failed(text);
        }, Qt::QueuedConnection);
    }
}

void Session::post(std::function<void()> call)
{
    if (call)
    {
        QMetaObject::invokeMethod(context_, std::move(call), Qt::QueuedConnection);
    }
}
