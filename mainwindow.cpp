#include "mainwindow.h"

#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include <QtGui/QPixmap>
#include <QtWidgets/QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // Initialize the bounded RAM ring buffer (150 frames capacity)
    ringBuffer_ = std::make_unique<FrameRingBuffer>(150);

    // Set up status bar indicators
    setupStatusBar();

    // Create synthetic source (640x480, default 30 FPS)
    frameSource_ = std::make_unique<SyntheticFrameSource>(640, 480, ui.targetFpsSpinBox->value());
    frameSource_->setFrameCallback([this](Frame& frame) {
        onFrameArrived(frame);
    });

    // Wire up UI signals
    connect(ui.startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui.stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
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
}

void MainWindow::setupStatusBar()
{
    statusLabel_ = new QLabel("Status: Idle", this);
    fpsLabel_ = new QLabel("FPS: --", this);
    framesLabel_ = new QLabel("Frames: 0", this);
    bufferLabel_ = new QLabel(this);
    resolutionLabel_ = new QLabel("640 x 480 (Mono8)", this);

    statusLabel_->setMinimumWidth(110);
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
    if (!ringBuffer_ || !bufferLabel_)
    {
        return;
    }

    const size_t stored = ringBuffer_->size();
    const size_t cap = ringBuffer_->capacity();
    const double mb = static_cast<double>(ringBuffer_->memoryUsageBytes()) / 1'000'000.0;

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
    acquiredFrames_.store(0, std::memory_order_relaxed);
    lastAcquiredSample_ = 0;
    lastFpsSampleTime_ = std::chrono::steady_clock::now();
    framesLabel_->setText("Frames: 0");
    fpsLabel_->setText("FPS: --");

    if (ringBuffer_)
    {
        ringBuffer_->clear();
        updateBufferReadout();
    }

    frameSource_->setTargetFps(ui.targetFpsSpinBox->value());
    frameSource_->start();
    bufferReadoutTimer_->start();

    ui.startButton->setEnabled(false);
    ui.stopButton->setEnabled(true);
    statusLabel_->setText("Status: Streaming");
}

void MainWindow::onStopClicked()
{
    bufferReadoutTimer_->stop();
    frameSource_->stop();

    ui.startButton->setEnabled(true);
    ui.stopButton->setEnabled(false);
    statusLabel_->setText("Status: Stopped");
    fpsLabel_->setText("FPS: --");
    framesLabel_->setText(QString("Frames: %1").arg(acquiredFrames_.load(std::memory_order_relaxed)));

    updateBufferReadout();
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

    // Deliver preview frame to Qt GUI thread if not busy rendering
    if (!renderingFrame_.exchange(true))
    {
        QImage previewImage = frame.toQImage();
        QMetaObject::invokeMethod(this, [this, img = std::move(previewImage)]() mutable {
            displayImage(std::move(img));
            renderingFrame_.store(false);
        }, Qt::QueuedConnection);
    }

    // Push into the ring buffer and recycle the slot's buffer back into 'frame.data'
    ringBuffer_->push(frame);
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
