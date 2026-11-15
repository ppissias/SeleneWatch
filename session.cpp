#include "session.h"

#include <QtCore/QMetaObject>

#include <utility>
#include <stdexcept>

Session::Session(QObject* context, QString outputRoot, Notifications notifications)
    : context_(context)
    , notifications_(std::move(notifications))
    , writer_(context, std::move(outputRoot), captureSettings_.memoryBytes * 2 / 5,
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
              writer_.submit(std::move(clip));
          }, {}, memory_)
    , guider_(writer_, [this] { post(notifications_.lockChanged); })
{
}

Session::~Session()
{
    stop();
    guider_.shutdown();
    writer_.shutdown();
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

    if (mode == Mode::Monitoring) writer_.logSessionStart(streamInfo_);
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
    if (*request && mode_.load() != Mode::Monitoring) writer_.logSessionStart(streamInfo_);
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
        budgetPrepared_ = true;
    }
    acquiredFrames_.fetch_add(1, std::memory_order_relaxed);
    if (frame->sourceDroppedFrames) droppedFrames_.store(*frame->sourceDroppedFrames);
    dropsKnown_.store(frame->sourceDroppedFrames.has_value());

    blip_.apply(*frame);
    const FramePtr published = std::move(frame);

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
    capture_.finishPartial("source ended");
    post(notifications_.ended);
}

void Session::onFailed(const std::string& message)
{
    sourceFinished_.store(true);
    applyManualSave();
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
