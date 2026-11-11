#include "mainwindow.h"

#include <QtCore/QMetaObject>
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
}

MainWindow::~MainWindow()
{
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

void MainWindow::onStartClicked()
{
    totalFrames_ = 0;
    framesInCurrentSecond_ = 0;
    lastFpsCalcTime_ = std::chrono::steady_clock::now();

    if (ringBuffer_)
    {
        ringBuffer_->clear();
        updateBufferReadout();
    }

    frameSource_->setTargetFps(ui.targetFpsSpinBox->value());
    frameSource_->start();

    ui.startButton->setEnabled(false);
    ui.stopButton->setEnabled(true);
    statusLabel_->setText("Status: Streaming");
}

void MainWindow::onStopClicked()
{
    frameSource_->stop();

    ui.startButton->setEnabled(true);
    ui.stopButton->setEnabled(false);
    statusLabel_->setText("Status: Stopped");
    fpsLabel_->setText("FPS: --");

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
    // Update frame diagnostics
    ++totalFrames_;
    ++framesInCurrentSecond_;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsCalcTime_).count();

    if (elapsed >= 1000)
    {
        const double fps = (framesInCurrentSecond_ * 1000.0) / elapsed;
        fpsLabel_->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
        framesInCurrentSecond_ = 0;
        lastFpsCalcTime_ = now;

        updateBufferReadout();
    }

    framesLabel_->setText(QString("Frames: %1").arg(totalFrames_));

    // Render image to viewport label
    if (!image.isNull())
    {
        const QSize targetSize = ui.viewportLabel->size();
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(
            targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
        ui.viewportLabel->setPixmap(pixmap);
    }
}
