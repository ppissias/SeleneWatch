#include "mainwindow.h"

#include "preview_image.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QStatusBar>

#include <string>
#include <utility>

namespace {

// Frames of history kept in the ring.
constexpr size_t kRingFrames = 150;

// Frames that may wait for the writer: two clips of the maximum length.
constexpr size_t kMaxQueuedFrames = 2 * (kRingFrames + static_cast<size_t>(kMaxPostEventFrames));

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // Clips are written next to the executable.
    const QString outputRoot = QDir(QCoreApplication::applicationDirPath()).filePath("events");
    eventWriter_ = std::make_unique<EventWriter>(this, outputRoot, kMaxQueuedFrames,
        [this](const ClipWriteResult& result) { onClipWritten(result); });

    // A finished clip goes straight from the capture to the writer, on the
    // source thread. The window is not in between.
    capture_ = std::make_unique<EventCapture>(
        kRingFrames,
        [this] {
            QMetaObject::invokeMethod(this, [this] { onCandidateStarted(); }, Qt::QueuedConnection);
        },
        [writer = eventWriter_.get()](EventClip clip) { writer->submit(std::move(clip)); });

    setupStatusBar();

    frameSource_ = std::make_unique<SyntheticFrameSource>(640, 480, ui.targetFpsSpinBox->value());
    frameSource_->setFrameCallback([this](std::shared_ptr<Frame> frame) {
        onFrameArrived(std::move(frame));
    });
    frameSource_->setErrorCallback([this](const std::string& message) {
        const QString text = QString::fromStdString(message);
        QMetaObject::invokeMethod(this, [this, text] {
            onSourceFailed(text);
        }, Qt::QueuedConnection);
    });

    connect(ui.startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui.stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui.injectFlashButton, &QPushButton::clicked, this, &MainWindow::onInjectFlashClicked);
    connect(ui.targetFpsSpinBox, &QSpinBox::valueChanged, this, &MainWindow::onTargetFpsChanged);

    bufferReadoutTimer_ = new QTimer(this);
    bufferReadoutTimer_->setInterval(1000);
    connect(bufferReadoutTimer_, &QTimer::timeout, this, &MainWindow::updateStreamReadout);
}

MainWindow::~MainWindow()
{
    if (bufferReadoutTimer_)
    {
        bufferReadoutTimer_->stop();
    }
    if (frameSource_)
    {
        frameSource_->stop();
    }
    if (capture_)
    {
        capture_->finishPartial();
    }
    if (eventWriter_)
    {
        eventWriter_->shutdown();
    }
}

void MainWindow::setupStatusBar()
{
    statusLabel_ = new QLabel("Status: Idle", this);
    fpsLabel_ = new QLabel("FPS: --", this);
    framesLabel_ = new QLabel("Frames: 0", this);
    candidatesLabel_ = new QLabel(this);
    bufferLabel_ = new QLabel(this);
    resolutionLabel_ = new QLabel("640 x 480 (Mono8)", this);

    statusLabel_->setMinimumWidth(150);
    fpsLabel_->setMinimumWidth(80);
    framesLabel_->setMinimumWidth(100);
    candidatesLabel_->setMinimumWidth(110);
    bufferLabel_->setMinimumWidth(210);
    resolutionLabel_->setMinimumWidth(120);

    statusBar()->addPermanentWidget(statusLabel_);
    statusBar()->addPermanentWidget(fpsLabel_);
    statusBar()->addPermanentWidget(framesLabel_);
    statusBar()->addPermanentWidget(candidatesLabel_);
    statusBar()->addPermanentWidget(bufferLabel_);
    statusBar()->addPermanentWidget(resolutionLabel_);

    updateBufferReadout();
    updateCandidateReadout();
}

void MainWindow::updateBufferReadout()
{
    if (!capture_ || !bufferLabel_)
    {
        return;
    }

    // The memory is that of every live frame: the ring, clips being
    // collected or waiting for the writer, and buffers waiting for reuse.
    const size_t stored = capture_->bufferSize();
    const size_t cap = capture_->bufferCapacity();
    const size_t liveBytes = frameSource_ ? frameSource_->liveFrameBytes() : 0;
    const double mb = static_cast<double>(liveBytes) / 1'000'000.0;

    bufferLabel_->setText(QString("Buffer: %1/%2 frames (%3 MB live)")
        .arg(stored)
        .arg(cap)
        .arg(mb, 0, 'f', 1));
}

void MainWindow::updateStreamReadout()
{
    updateBufferReadout();

    const auto now = std::chrono::steady_clock::now();
    const uint64_t acquired = acquiredFrames_.load(std::memory_order_relaxed);
    const double elapsed = std::chrono::duration<double>(now - lastFpsSampleTime_).count();

    if (fpsLabel_ && elapsed > 0.0)
    {
        const double fps = static_cast<double>(acquired - lastAcquiredSample_) / elapsed;
        fpsLabel_->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
    }

    lastAcquiredSample_ = acquired;
    lastFpsSampleTime_ = now;

    if (framesLabel_)
    {
        framesLabel_->setText(QString("Frames: %1").arg(acquired));
    }
}

void MainWindow::updateCandidateReadout()
{
    QString text = QString("Candidates: %1").arg(candidates_);
    if (clipsNotSaved_ > 0)
    {
        text += QString(" (%1 not saved)").arg(clipsNotSaved_);
    }
    candidatesLabel_->setText(text);
}

// The label is always worked out from the current state, so a late
// notification cannot leave a stale text behind.
void MainWindow::updateStatusLabel()
{
    const bool streaming = frameSource_ && frameSource_->isRunning();

    if (sourceFailed_)
    {
        statusLabel_->setText("Status: Source failed");
    }
    else if (recordFailed_)
    {
        statusLabel_->setText("Status: Recording failed");
    }
    else if (streaming)
    {
        statusLabel_->setText("Status: Streaming");
    }
    else
    {
        statusLabel_->setText("Status: Stopped");
    }
}

void MainWindow::onStartClicked()
{
    sourceFailed_ = false;
    recordFailed_ = false;
    candidates_ = 0;
    clipsNotSaved_ = 0;
    acquiredFrames_.store(0, std::memory_order_relaxed);
    lastAcquiredSample_ = 0;
    lastFpsSampleTime_ = std::chrono::steady_clock::now();
    framesLabel_->setText("Frames: 0");
    fpsLabel_->setText("FPS: --");
    updateCandidateReadout();

    capture_->reset();
    updateBufferReadout();

    frameSource_->setTargetFps(ui.targetFpsSpinBox->value());
    frameSource_->start();
    bufferReadoutTimer_->start();

    ui.startButton->setEnabled(false);
    ui.stopButton->setEnabled(true);
    ui.injectFlashButton->setEnabled(true);
    updateStatusLabel();
}

void MainWindow::onStopClicked()
{
    bufferReadoutTimer_->stop();
    frameSource_->stop();
    capture_->finishPartial();

    ui.startButton->setEnabled(true);
    ui.stopButton->setEnabled(false);
    ui.injectFlashButton->setEnabled(false);
    fpsLabel_->setText("FPS: --");
    framesLabel_->setText(QString("Frames: %1").arg(acquiredFrames_.load(std::memory_order_relaxed)));

    updateStatusLabel();
    updateBufferReadout();
}

void MainWindow::onInjectFlashClicked()
{
    if (frameSource_)
    {
        frameSource_->requestFlash();
    }
}

void MainWindow::onTargetFpsChanged(int fps)
{
    if (frameSource_)
    {
        frameSource_->setTargetFps(fps);
    }
}

// Source thread. From the moment the frame is held as a FramePtr it is
// published: nobody writes it again.
void MainWindow::onFrameArrived(std::shared_ptr<Frame> frame)
{
    acquiredFrames_.fetch_add(1, std::memory_order_relaxed);

    const FramePtr published = std::move(frame);

    // The preview converts on the GUI thread. It is woken once per frame it
    // has not yet taken, and skips frames when it falls behind.
    if (previewSlot_.put(published))
    {
        QMetaObject::invokeMethod(this, [this] { showLatestFrame(); }, Qt::QueuedConnection);
    }

    capture_->onFrame(published);
}

void MainWindow::showLatestFrame()
{
    const FramePtr frame = previewSlot_.take();
    if (frame)
    {
        displayImage(frameToPreviewImage(*frame));
    }
}

void MainWindow::onCandidateStarted()
{
    ++candidates_;
    updateCandidateReadout();
    statusBar()->showMessage(QString("Candidate %1 detected").arg(candidates_));
}

void MainWindow::onClipWritten(const ClipWriteResult& result)
{
    if (result.recordFailed)
    {
        recordFailed_ = true;
    }

    if (result.folder.isEmpty())
    {
        ++clipsNotSaved_;
        updateCandidateReadout();
        statusBar()->showMessage(QString("Clip not saved: %1").arg(result.notSaved));
    }
    else
    {
        statusBar()->showMessage(result.folder);
    }

    updateStatusLabel();
}

void MainWindow::onSourceFailed(const QString& message)
{
    sourceFailed_ = true;
    if (bufferReadoutTimer_)
    {
        bufferReadoutTimer_->stop();
    }
    frameSource_->stop();
    capture_->finishPartial();

    ui.startButton->setEnabled(true);
    ui.stopButton->setEnabled(false);
    ui.injectFlashButton->setEnabled(false);
    statusBar()->showMessage(message);
    fpsLabel_->setText("FPS: --");
    updateStatusLabel();
}

void MainWindow::displayImage(const QImage& image)
{
    if (!image.isNull())
    {
        const QSize targetSize = ui.viewportLabel->size();
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(
            targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
        ui.viewportLabel->setPixmap(pixmap);
    }
}
