#include "mainwindow.h"

#include "frame_image.h"

#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QStatusBar>

#include <string>
#include <utility>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    eventWriter_ = std::make_unique<EventWriter>(this, [this](const QString& path, const QString& error) {
        onEventWritten(path, error);
    });
    capture_ = std::make_unique<EventCapture>(
        150,
        [this] { onCandidateStarted(); },
        [this](EventClip clip) { onClipReady(std::move(clip)); });

    setupStatusBar();

    frameSource_ = std::make_unique<SyntheticFrameSource>(640, 480, ui.targetFpsSpinBox->value());
    frameSource_->setFrameCallback([this](Frame& frame) {
        onFrameArrived(frame);
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
    bufferLabel_ = new QLabel(this);
    resolutionLabel_ = new QLabel("640 x 480 (Mono8)", this);

    statusLabel_->setMinimumWidth(150);
    fpsLabel_->setMinimumWidth(80);
    framesLabel_->setMinimumWidth(100);
    bufferLabel_->setMinimumWidth(210);
    resolutionLabel_->setMinimumWidth(120);

    statusBar()->addPermanentWidget(statusLabel_);
    statusBar()->addPermanentWidget(fpsLabel_);
    statusBar()->addPermanentWidget(framesLabel_);
    statusBar()->addPermanentWidget(bufferLabel_);
    statusBar()->addPermanentWidget(resolutionLabel_);

    updateBufferReadout();
}

void MainWindow::updateBufferReadout()
{
    if (!capture_ || !bufferLabel_)
    {
        return;
    }

    const size_t stored = capture_->bufferSize();
    const size_t cap = capture_->bufferCapacity();
    const double mb = static_cast<double>(capture_->memoryUsageBytes()) / 1'000'000.0;

    bufferLabel_->setText(QString("Buffer: %1/%2 frames (%3 MB)")
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

void MainWindow::onStartClicked()
{
    sourceFailed_ = false;
    acquiredFrames_.store(0, std::memory_order_relaxed);
    lastAcquiredSample_ = 0;
    lastFpsSampleTime_ = std::chrono::steady_clock::now();
    framesLabel_->setText("Frames: 0");
    fpsLabel_->setText("FPS: --");

    if (capture_)
    {
        capture_->reset();
        updateBufferReadout();
    }

    frameSource_->setTargetFps(ui.targetFpsSpinBox->value());
    frameSource_->start();
    bufferReadoutTimer_->start();

    ui.startButton->setEnabled(false);
    ui.stopButton->setEnabled(true);
    ui.injectFlashButton->setEnabled(capture_ && capture_->writesInFlight() == 0);
    statusLabel_->setText("Status: Streaming");
}

void MainWindow::onStopClicked()
{
    bufferReadoutTimer_->stop();
    frameSource_->stop();
    if (capture_)
    {
        capture_->finishPartial();
    }

    ui.startButton->setEnabled(true);
    ui.stopButton->setEnabled(false);
    ui.injectFlashButton->setEnabled(false);
    statusLabel_->setText("Status: Stopped");
    fpsLabel_->setText("FPS: --");
    framesLabel_->setText(QString("Frames: %1").arg(acquiredFrames_.load(std::memory_order_relaxed)));

    updateBufferReadout();
}

void MainWindow::onInjectFlashClicked()
{
    if (frameSource_)
    {
        frameSource_->requestFlash();
    }
}

void MainWindow::onEventWritten(const QString& path, const QString& error)
{
    const int remaining = capture_->onWriteFinished();

    if (!error.isEmpty())
    {
        statusLabel_->setText("Status: Save failed");
        statusBar()->showMessage(error);
    }
    else if (sourceFailed_)
    {
        statusLabel_->setText("Status: Source failed");
        statusBar()->showMessage(path);
    }
    else if (frameSource_ && frameSource_->isRunning())
    {
        statusLabel_->setText("Status: Streaming");
        statusBar()->showMessage(path);
    }
    else
    {
        statusLabel_->setText("Status: Stopped");
        statusBar()->showMessage(path);
    }

    if (remaining <= 0 && frameSource_ && frameSource_->isRunning() && !sourceFailed_)
    {
        ui.injectFlashButton->setEnabled(true);
    }
}

void MainWindow::onTargetFpsChanged(int fps)
{
    if (frameSource_)
    {
        frameSource_->setTargetFps(fps);
    }
}

void MainWindow::onFrameArrived(Frame& frame)
{
    acquiredFrames_.fetch_add(1, std::memory_order_relaxed);
    publishPreview(frame);
    capture_->onFrame(frame);
}

void MainWindow::publishPreview(const Frame& frame)
{
    if (renderingFrame_.exchange(true))
    {
        return;
    }

    QImage previewImage = frameToQImage(frame);
    QMetaObject::invokeMethod(this, [this, img = std::move(previewImage)]() mutable {
        displayImage(std::move(img));
        renderingFrame_.store(false);
    }, Qt::QueuedConnection);
}

void MainWindow::onCandidateStarted()
{
    QMetaObject::invokeMethod(this, [this] {
        if (!frameSource_ || !frameSource_->isRunning())
        {
            return;
        }
        statusLabel_->setText("Status: Candidate");
        ui.injectFlashButton->setEnabled(false);
    }, Qt::QueuedConnection);
}

void MainWindow::onClipReady(EventClip clip)
{
    if (eventWriter_)
    {
        eventWriter_->submit(std::move(clip));
    }
}

void MainWindow::onSourceFailed(const QString& message)
{
    sourceFailed_ = true;
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

    ui.startButton->setEnabled(true);
    ui.stopButton->setEnabled(false);
    ui.injectFlashButton->setEnabled(false);
    statusLabel_->setText("Status: Source failed");
    statusBar()->showMessage(message);
    fpsLabel_->setText("FPS: --");
}

void MainWindow::displayImage(QImage image)
{
    if (!image.isNull())
    {
        const QSize targetSize = ui.viewportLabel->size();
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(
            targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
        ui.viewportLabel->setPixmap(pixmap);
    }
}
