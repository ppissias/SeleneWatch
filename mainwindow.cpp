#include "mainwindow.h"

#include <QtCore/QMetaObject>
#include <QtGui/QPixmap>
#include <QtWidgets/QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    setupStatusBar();

    // Create synthetic source (640x480, default 30 FPS)
    frameSource_ = std::make_unique<SyntheticFrameSource>(640, 480, ui.targetFpsSpinBox->value());
    frameSource_->setFrameCallback([this](const Frame& frame) {
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
    resolutionLabel_ = new QLabel("640 x 480 (Mono8)", this);

    statusLabel_->setMinimumWidth(120);
    fpsLabel_->setMinimumWidth(80);
    framesLabel_->setMinimumWidth(100);
    resolutionLabel_->setMinimumWidth(120);

    statusBar()->addPermanentWidget(statusLabel_);
    statusBar()->addPermanentWidget(fpsLabel_);
    statusBar()->addPermanentWidget(framesLabel_);
    statusBar()->addPermanentWidget(resolutionLabel_);
}

void MainWindow::onStartClicked()
{
    totalFrames_ = 0;
    framesInCurrentSecond_ = 0;
    lastFpsCalcTime_ = std::chrono::steady_clock::now();

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
}

void MainWindow::onTargetFpsChanged(int fps)
{
    if (frameSource_)
    {
        frameSource_->setTargetFps(fps);
    }
}

void MainWindow::onFrameArrived(const Frame& frame)
{
    // Deliver to Qt GUI thread without blocking camera acquisition
    if (renderingFrame_.exchange(true))
    {
        // UI thread is still rendering the previous frame; drop UI display of this frame
        return;
    }

    QMetaObject::invokeMethod(this, [this, frame]() {
        displayFrame(frame);
        renderingFrame_.store(false);
    }, Qt::QueuedConnection);
}

void MainWindow::displayFrame(const Frame& frame)
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
    }

    framesLabel_->setText(QString("Frames: %1").arg(totalFrames_));

    // Render image to viewport label
    const QImage image = frame.toQImage();
    if (!image.isNull())
    {
        const QSize targetSize = ui.viewportLabel->size();
        const QPixmap pixmap = QPixmap::fromImage(image).scaled(
            targetSize, Qt::KeepAspectRatio, Qt::FastTransformation);
        ui.viewportLabel->setPixmap(pixmap);
    }
}
