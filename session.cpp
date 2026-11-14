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
{
}

Session::~Session()
{
    stop();
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

bool Session::start(QString& error)
{
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

    streamInfo_ = source_->streamInfo();
    acquiredFrames_.store(0, std::memory_order_relaxed);
    sourceFinished_.store(false);
    capture_.reset();
    blip_.reset();
    lastPreviewTime_ = {};
    (void)previewSlot_.take();

    writer_.logSessionStart(streamInfo_);
    source_->start(*this);
    running_ = true;
    return true;
}

void Session::stop()
{
    if (!running_)
    {
        return;
    }
    source_->stop();
    capture_.finishPartial("stopped");
    blip_.reset();
    running_ = false;
}

bool Session::requestBlip(const BlipSettings& settings)
{
    return running_ && !sourceFinished_.load() && blip_.request(settings);
}

bool Session::blipBusy() const
{
    return blip_.busy();
}

bool Session::isRunning() const
{
    return running_;
}

bool Session::sourceFinished() const
{
    return sourceFinished_.load();
}

const StreamInfo& Session::streamInfo() const
{
    return streamInfo_;
}

uint64_t Session::acquiredFrames() const
{
    return acquiredFrames_.load(std::memory_order_relaxed);
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
    std::lock_guard<std::mutex> lock(detectionMutex_);
    detectionSettings_ = settings;
}

void Session::setDetectionDisplay(DetectionOverlay overlay, int probeX, int probeY)
{
    std::lock_guard<std::mutex> lock(detectionMutex_);
    displayRequest_.overlay = overlay;
    displayRequest_.probeX = probeX;
    displayRequest_.probeY = probeY;
}

// From the moment the frame is held as a FramePtr it is published: nobody
// writes it again. Test injection is the last write before publication.
void Session::onFrame(std::shared_ptr<Frame> frame)
{
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

    blip_.apply(*frame);
    const FramePtr published = std::move(frame);

    DetectionSettings settings;
    DetectionDisplayRequest display;
    {
        std::lock_guard<std::mutex> lock(detectionMutex_);
        settings = detectionSettings_;
        display = displayRequest_;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool previewDue = now - lastPreviewTime_ >= std::chrono::milliseconds{50};
    display.makeMap = previewDue;
    capture_.setDetectionSettings(settings);
    capture_.onFrame(published, display);

    // The preview converts on the context thread. It is woken once per frame
    // it has not yet taken, and skips frames when it falls behind.
    if (previewDue || capture_.detectionSummary().triggered)
    {
        lastPreviewTime_ = now;
        auto view = std::make_shared<const DetectionView>(capture_.detectionView());
        if (previewSlot_.put(published, std::move(view))) post(notifications_.previewReady);
    }
}

// The next frame is not the sky a moment later (a loop starting again). The
// jump would look like a sudden change, so the capture starts afresh.
void Session::onDiscontinuity()
{
    capture_.finishPartial("playback loop boundary");
    capture_.reset();
}

void Session::onEnded()
{
    capture_.finishPartial("source ended");
    sourceFinished_.store(true);
    post(notifications_.ended);
}

void Session::onFailed(const std::string& message)
{
    capture_.finishPartial("source failure: " + message);
    sourceFinished_.store(true);
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
