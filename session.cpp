#include "session.h"

#include "clock_check.h"
#include "recording/metadata_text.h"

#include <QtCore/QMetaObject>

#include <format>
#include <utility>
#include <stdexcept>

Session::Session(QObject* context, QString outputRoot, Notifications notifications)
    : context_(context)
    , notifications_(std::move(notifications))
    , writer_(context, outputRoot, captureSettings_.memoryBytes * 2 / 5,
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
              writer_.submit(std::move(clip));
          }, {}, memory_)
    , guider_(writer_, [this] { post(notifications_.lockChanged); })
    , outputRoot_(std::move(outputRoot))
    , recorder_([this](const RecordingWriter::Status& status) {
          // Writer thread: the recording ended, by a stop or by itself.
          writer_.logRecord(QString("%1 recording_stop name=%2 frames=%3 bytes=%4 seconds=%5 ended=\"%6\"\n")
              .arg(utcText(std::chrono::system_clock::now()), status.name).arg(status.frames).arg(status.bytes)
              .arg(status.streamSeconds, 0, 'f', 3).arg(QString::fromStdString(status.endReason)));
          post(notifications_.recordingChanged);
      })
{
}

Session::~Session()
{
    stop();
    guider_.shutdown();
    // A recording still being written is finished before the record closes.
    recorder_.stop("the program closed");
    recorder_.waitIdle();
    if (clockThread_.joinable()) clockThread_.join();
    writer_.shutdown();
}

void Session::setSite(const ObservingSite& site)
{
    std::lock_guard lock(requestMutex_);
    site_ = site;
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

bool Session::requestRecording(bool record)
{
    std::lock_guard lock(requestMutex_);
    if (!running_ || sourceFinished_.load() || mode_.load() != Mode::Monitoring) return false;
    recordRequest_ = record;
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
    std::optional<Schedule> schedule;
    {
        std::lock_guard lock(requestMutex_);
        request = recordRequest_;
        recordRequest_.reset();
        schedule = schedule_;
    }
    const bool monitoring = mode_.load() == Mode::Monitoring;
    if (recording_)
    {
        if (!monitoring) endRecording("monitoring stopped");
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
        if (recorder_.push(frame)) recordingLastIndex_ = frame->frameIndex;
        else
        {
            // It ended by itself; its writer has the reason and reports it.
            recording_ = false;
            scheduledRecording_ = false;
        }
        return;
    }
    if (!monitoring) return;
    if (request && *request)
    {
        beginRecording(frame, "the observer");
        return;
    }
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

void Session::beginRecording(const FramePtr& first, const std::string& startedBy)
{
    RecordingWriter::Start start;
    start.outputRoot = outputRoot_;
    start.stream = streamInfo_;
    {
        std::lock_guard lock(requestMutex_);
        start.site = site_;
        start.clockRecord = clockRecord_;
    }
    start.target = captureSettings_.target;
    start.diskReserveBytes = captureSettings_.diskReserveBytes;
    start.startedBy = startedBy;
    start.queueBytes = recordingQueueBytes_ ? recordingQueueBytes_ : captureSettings_.memoryBytes / 8;
    if (!recorder_.start(start, first)) return;
    recording_ = true;
    recordingKnown_ = true;
    recordingName_ = utcStamp(first->timestamp).toStdString();
    recordingFirstIndex_ = recordingLastIndex_ = first->frameIndex;
    writer_.logRecord(QString("%1 recording_start name=%2 first_frame_index=%3 started_by=\"%4\"\n")
        .arg(utcText(first->timestamp), QString::fromStdString(recordingName_)).arg(first->frameIndex)
        .arg(QString::fromStdString(startedBy)));
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
    capture_.setMonitoring(mode == Mode::Monitoring);
    mode_.store(mode);
    settlingUntil_ = {};
    blip_.reset();
    lastPreviewTime_ = {};
    (void)previewSlot_.take();

    startClockCheck();
    if (mode == Mode::Monitoring)
    {
        std::lock_guard lock(requestMutex_);
        writer_.logSessionStart(streamInfo_, site_, clockRecord_);
    }
    // A held lock carries over; its estimate starts again with this run.
    // Guiding is off at every start (safety rule 1).
    guider_.discontinuity();
    guider_.endGuiding("The stream started. Guiding is off at every start; turn it on again.");
    source_->start(*this);
    running_ = true;
    return true;
}

void Session::stop(std::string reason)
{
    if (!running_)
    {
        return;
    }
    source_->stop();
    // The source thread is joined, so its recording state is this thread's now.
    endRecording("the stream stopped");
    // Safety rule 8: no guiding without frames. Only pulses were sent, so nothing is undone.
    guider_.endGuiding("The stream stopped (" + reason + ").");
    applyManualSave();
    capture_.finishPartial(std::move(reason));
    blip_.reset();
    running_ = false;
    mode_.store(Mode::Stopped);
    { std::lock_guard lock(requestMutex_); monitoringRequest_.reset(); }
}

bool Session::requestMonitoring(bool monitoring)
{
    std::lock_guard lock(requestMutex_);
    if (!running_ || sourceFinished_.load() || monitoringRequest_ || (monitoring && cameraTuned_)) return false;
    monitoringRequest_ = monitoring;
    return true;
}

Session::Mode Session::mode() const { return mode_.load(); }
bool Session::modeChangePending() const
{
    std::lock_guard lock(requestMutex_);
    return monitoringRequest_.has_value();
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
    applyManualSave();
    capture_.setMonitoring(*request);
    if (*request && mode_.load() != Mode::Monitoring)
    {
        std::lock_guard lock(requestMutex_);
        writer_.logSessionStart(streamInfo_, site_, clockRecord_);
    }
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
    detectionSettings_ = settings;
}

void Session::setDetectionDisplay(DetectionOverlay overlay, int probeX, int probeY)
{
    std::lock_guard<std::mutex> lock(requestMutex_);
    displayRequest_.overlay = overlay;
    displayRequest_.probeX = probeX;
    displayRequest_.probeY = probeY;
}

// From the moment the frame is held as a FramePtr it is published: nobody
// writes it again. Test injection is the last write before publication.
void Session::onFrame(std::shared_ptr<Frame> frame)
{
    applyModeRequest();
    if (!budgetPrepared_)
    {
        // Reserve for mean/variance, two maps/differences, GUI/writer images,
        // incoming/spare frames, and bounded sparse work before sharing the
        // rest between the open clip and all writer-held clips.
        const size_t pixels = static_cast<size_t>(frame->width) * frame->height;
        const size_t working = std::max(captureSettings_.memoryBytes / 5,
            pixels * 32 + frame->data.capacity() * 5 + size_t{4} * 1024 * 1024);
        if (working >= captureSettings_.memoryBytes) throw std::runtime_error("RAM ceiling too small for this frame layout and processing reserve; increase it");
        const size_t half = (captureSettings_.memoryBytes - working) / 2;
        const size_t previewAndNextFrame = pixels * sizeof(float) + frame->data.capacity();
        if (previewAndNextFrame >= half) throw std::runtime_error("RAM ceiling leaves too little room for a frame and its difference; increase it");
        capture_.configure(captureSettings_, half - previewAndNextFrame, half / 2);
        writer_.setByteLimit(half);
        // A recording's backlog mostly shares its frames with the ring; past
        // the ring's allowance the disk is not keeping up (Feature-14).
        recordingQueueBytes_ = half / 2;
        budgetPrepared_ = true;
    }
    acquiredFrames_.fetch_add(1, std::memory_order_relaxed);
    if (frame->sourceDroppedFrames) droppedFrames_.store(*frame->sourceDroppedFrames);
    dropsKnown_.store(frame->sourceDroppedFrames.has_value());

    blip_.apply(*frame);
    const FramePtr published = std::move(frame);
    updateRecording(published);

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
    if (settling) capture_.reset(false);
    const bool previewDue = now - lastPreviewTime_ >= std::chrono::milliseconds{50};
    display.makeMap = previewDue;
    capture_.setDetectionSettings(settings);
    capture_.onFrame(published, display);
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
    guider_.discontinuity();
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
    post(notifications_.cameraSettingsChanged);
}

void Session::onIdle() { applyModeRequest(); }

void Session::onEnded()
{
    sourceFinished_.store(true);
    applyManualSave();
    endRecording("the source ended");
    capture_.finishPartial("source ended");
    post(notifications_.ended);
}

void Session::onFailed(const std::string& message)
{
    sourceFinished_.store(true);
    applyManualSave();
    endRecording("source failure: " + message);
    capture_.finishPartial("source failure: " + message);
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
