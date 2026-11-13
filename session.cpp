#include "session.h"

#include <QtCore/QMetaObject>

#include <utility>

Session::Session(QObject* context, QString outputRoot, Notifications notifications)
    : context_(context)
    , notifications_(std::move(notifications))
    , writer_(context, std::move(outputRoot), kMaxQueuedFrames,
          [done = notifications_.clipWritten](const ClipWriteResult& result) {
              if (done)
              {
                  done(result);
              }
          })
    , capture_(
          kRingFrames,
          [this] { post(notifications_.candidateStarted); },
          // A finished clip goes straight from the capture to the writer, on
          // the source thread. The window is not in between.
          [this](EventClip clip) {
              clip.stream = streamInfo_;
              writer_.submit(std::move(clip));
          })
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
    capture_.finishPartial();
    running_ = false;
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

size_t Session::bufferCapacity() const
{
    return capture_.bufferCapacity();
}

size_t Session::liveFrameBytes() const
{
    return source_ ? source_->liveFrameBytes() : 0;
}

FramePtr Session::takePreviewFrame()
{
    return previewSlot_.take();
}

// From the moment the frame is held as a FramePtr it is published: nobody
// writes it again. Part C adds the blip just before this point.
void Session::onFrame(std::shared_ptr<Frame> frame)
{
    acquiredFrames_.fetch_add(1, std::memory_order_relaxed);

    const FramePtr published = std::move(frame);

    // The preview converts on the context thread. It is woken once per frame
    // it has not yet taken, and skips frames when it falls behind.
    if (previewSlot_.put(published))
    {
        post(notifications_.previewReady);
    }

    capture_.onFrame(published);
}

// The next frame is not the sky a moment later (a loop starting again). The
// jump would look like a sudden change, so the capture starts afresh.
void Session::onDiscontinuity()
{
    capture_.finishPartial();
    capture_.reset();
}

void Session::onEnded()
{
    capture_.finishPartial();
    sourceFinished_.store(true);
    post(notifications_.ended);
}

void Session::onFailed(const std::string& message)
{
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
