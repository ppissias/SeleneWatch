#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "detection_panel.h"

#include "preview_image.h"
#include "sources/recording_source.h"
#include "sources/synthetic_frame_source.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QSettings>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStatusBar>

#include <filesystem>
#include <utility>

namespace {

// Entries of the source and speed boxes, in the order of the form.
constexpr int kSyntheticEntry = 0;
constexpr int kFastEntry = 1;

constexpr int kSyntheticWidth = 640;
constexpr int kSyntheticHeight = 480;

} // namespace

MainWindow::MainWindow(QWidget *parent, QString outputRoot)
    : QMainWindow(parent)
    , ui_(std::make_unique<Ui::MainWindowClass>())
{
    ui_->setupUi(this);

    // Clips are written next to the executable.
    if (outputRoot.isEmpty()) outputRoot = QDir(QCoreApplication::applicationDirPath()).filePath("events");

    Session::Notifications notifications;
    notifications.previewReady = [this] { showLatestFrame(); };
    notifications.candidateStarted = [this] { onCandidateStarted(); };
    notifications.clipWritten = [this](const ClipWriteResult& result) { onClipWritten(result); };
    notifications.ended = [this] { onPlaybackEnded(); };
    notifications.failed = [this](const QString& message) { onSourceFailed(message); };
    session_ = std::make_unique<Session>(this, outputRoot, std::move(notifications));

    setupStatusBar();
    setupDetectionPanel();

    connect(ui_->startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui_->stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui_->openFileButton, &QPushButton::clicked, this, &MainWindow::onOpenFileClicked);
    connect(ui_->injectFlashButton, &QPushButton::clicked, this, &MainWindow::onInjectFlashClicked);
    connect(ui_->injectBlipButton, &QPushButton::clicked, this, &MainWindow::onInjectBlipClicked);
    connect(ui_->previewWidget, &PreviewWidget::positionSelected, this, [this] {
        updateDetectionDisplay();
        updateBlipReadout();
    });
    connect(ui_->blipAmplitudeSpinBox, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateBlipReadout);
    connect(ui_->sourceComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onSourceKindChanged);
    connect(ui_->speedComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onSpeedChanged);
    connect(ui_->loopCheckBox, &QCheckBox::toggled, this, &MainWindow::updateControls);
    connect(ui_->targetFpsSpinBox, &QSpinBox::valueChanged, this, &MainWindow::onTargetFpsChanged);

    bufferReadoutTimer_ = new QTimer(this);
    bufferReadoutTimer_->setInterval(1000);
    connect(bufferReadoutTimer_, &QTimer::timeout, this, &MainWindow::updateStreamReadout);

    updateControls();
    updateBlipReadout();
}

MainWindow::~MainWindow()
{
    // QMainWindow may hide its docks while its base destructor runs, after
    // this derived window and its members have ceased to exist.
    if (detectionDock_) disconnect(detectionDock_, nullptr, this, nullptr);
    if (detectionPanel_) disconnect(detectionPanel_, nullptr, this, nullptr);
    saveDetectionSettings();
    if (bufferReadoutTimer_)
    {
        bufferReadoutTimer_->stop();
    }
    // The session stops its source and writes what is queued.
    session_.reset();
}

void MainWindow::setupDetectionPanel()
{
    detectionPanel_ = new DetectionPanel(this);
    detectionDock_ = new QDockWidget("Detection", this);
    detectionDock_->setObjectName("detectionDock");
    detectionDock_->setWidget(detectionPanel_);
    addDockWidget(Qt::RightDockWidgetArea, detectionDock_);
    auto* view = menuBar()->addMenu("View");
    view->addAction(detectionDock_->toggleViewAction());
    QSettings saved;
    DetectionSettings settings;
    settings.sigma = saved.value("detection/sigma", settings.sigma).toDouble();
    settings.minPixels = saved.value("detection/minPixels", settings.minPixels).toInt();
    settings.maxPixels = saved.value("detection/maxPixels", settings.maxPixels).toInt();
    settings.oneFrame = saved.value("detection/oneFrame", settings.oneFrame).toBool();
    detectionPanel_->setSettings(settings.valid() ? settings : DetectionSettings{});
    session_->setDetectionSettings(detectionPanel_->settings());
    connect(detectionPanel_, &DetectionPanel::settingsChanged, this, [this] {
        session_->setDetectionSettings(detectionPanel_->settings());
    });
    connect(detectionPanel_, &DetectionPanel::displayChanged, this, &MainWindow::updateDetectionDisplay);
    connect(detectionDock_, &QDockWidget::visibilityChanged, this, &MainWindow::updateDetectionDisplay);
    updateDetectionDisplay();
    resize(1340, 820);
}

void MainWindow::saveDetectionSettings()
{
    if (!detectionPanel_) return;
    const auto values = detectionPanel_->settings();
    QSettings saved;
    saved.setValue("detection/sigma", values.sigma);
    saved.setValue("detection/minPixels", values.minPixels);
    saved.setValue("detection/maxPixels", values.maxPixels);
    saved.setValue("detection/oneFrame", values.oneFrame);
    saved.sync();
}

void MainWindow::updateDetectionDisplay()
{
    const auto position = ui_->previewWidget->selectedPosition();
    const DetectionOverlay overlay = detectionDock_->isVisible() ? detectionPanel_->overlay() : DetectionOverlay::None;
    session_->setDetectionDisplay(overlay, position ? position->x() : -1, position ? position->y() : -1);
    ui_->previewWidget->setDetectionOverlay({}, {});
}

void MainWindow::setupStatusBar()
{
    statusLabel_ = new QLabel("Status: Idle", this);
    fpsLabel_ = new QLabel("FPS: --", this);
    framesLabel_ = new QLabel("Frames: 0", this);
    candidatesLabel_ = new QLabel(this);
    bufferLabel_ = new QLabel(this);
    frameLabel_ = new QLabel("No frame yet", this);

    statusLabel_->setMinimumWidth(150);
    fpsLabel_->setMinimumWidth(80);
    framesLabel_->setMinimumWidth(100);
    candidatesLabel_->setMinimumWidth(110);
    bufferLabel_->setMinimumWidth(210);
    frameLabel_->setMinimumWidth(160);

    statusBar()->addPermanentWidget(statusLabel_);
    statusBar()->addPermanentWidget(fpsLabel_);
    statusBar()->addPermanentWidget(framesLabel_);
    statusBar()->addPermanentWidget(candidatesLabel_);
    statusBar()->addPermanentWidget(bufferLabel_);
    statusBar()->addPermanentWidget(frameLabel_);

    updateBufferReadout();
    updateCandidateReadout();
}

bool MainWindow::recordingSelected() const
{
    return ui_->sourceComboBox->currentIndex() != kSyntheticEntry;
}

// Which controls can be used depends on the source and on whether it runs.
void MainWindow::updateControls()
{
    const bool running = session_->isRunning();
    const bool recording = recordingSelected();

    ui_->startButton->setEnabled(!running);
    ui_->stopButton->setEnabled(running);
    ui_->injectFlashButton->setEnabled(running && synthetic_ != nullptr);

    ui_->sourceComboBox->setEnabled(!running);
    ui_->openFileButton->setEnabled(!running && recording);
    ui_->loopCheckBox->setEnabled(!running && recording);
    ui_->loopSecondsSpinBox->setEnabled(!running && recording && ui_->loopCheckBox->isChecked());

    // The speed may change while a recording plays.
    ui_->speedComboBox->setEnabled(recording);
}

void MainWindow::updateBufferReadout()
{
    if (!bufferLabel_)
    {
        return;
    }

    // The memory is that of every live frame: the ring, clips being
    // collected or waiting for the writer, and buffers waiting for reuse.
    const double mb = static_cast<double>(session_->liveFrameBytes()) / 1'000'000.0;
    bufferLabel_->setText(QString("Buffer: %1/%2 frames (%3 MB live)")
        .arg(session_->bufferSize())
        .arg(session_->bufferCapacity())
        .arg(mb, 0, 'f', 1));
}

void MainWindow::updateStreamReadout()
{
    updateBufferReadout();
    updateBlipReadout();

    const auto now = std::chrono::steady_clock::now();
    const uint64_t acquired = session_->acquiredFrames();
    const double elapsed = std::chrono::duration<double>(now - lastFpsSampleTime_).count();

    if (elapsed > 0.0)
    {
        const double fps = static_cast<double>(acquired - lastAcquiredSample_) / elapsed;
        fpsLabel_->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
    }

    lastAcquiredSample_ = acquired;
    lastFpsSampleTime_ = now;
    framesLabel_->setText(QString("Frames: %1").arg(acquired));
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
    if (sourceFailed_)
    {
        statusLabel_->setText("Status: Source failed");
    }
    else if (recordFailed_)
    {
        statusLabel_->setText("Status: Recording failed");
    }
    else if (session_->isRunning())
    {
        statusLabel_->setText(recording_ ? "Status: Playing" : "Status: Streaming");
    }
    else if (playbackEnded_)
    {
        statusLabel_->setText("Status: Playback finished");
    }
    else
    {
        statusLabel_->setText("Status: Stopped");
    }
}

void MainWindow::updateFrameLabel(const Frame& frame)
{
    QString text = QString("%1 x %2 %3").arg(frame.width).arg(frame.height).arg(pixelFormatName(frame.format));
    if (frame.format == PixelFormat::Mono16)
    {
        text += QString(", %1 bits %2").arg(frame.significantBits)
            .arg(frame.alignment == BitAlignment::High ? "high" : "low");
    }
    if (frame.bayer != BayerPattern::None)
    {
        text += QString(", Bayer %1").arg(bayerPatternName(frame.bayer));
    }
    if (frameLabel_->text() != text)
    {
        frameLabel_->setText(text);
    }
    const int maximum = frame.format == PixelFormat::Mono16 ? (1 << frame.significantBits) - 1 : 255;
    ui_->blipAmplitudeSpinBox->setMaximum(maximum);
}

void MainWindow::updateBlipReadout()
{
    const auto centre = ui_->previewWidget->selectedPosition();
    const bool busy = session_->blipBusy();
    ui_->injectBlipButton->setEnabled(session_->isRunning() && !session_->sourceFinished() && centre && !busy);
    ui_->blipPositionLabel->setText(centre
        ? QString("Centre: (%1, %2)").arg(centre->x()).arg(centre->y())
        : QString("Click the preview to place a flash"));
    ui_->blipStatusLabel->setText(QString("Next peak: %1 ADU. %2")
        .arg(ui_->blipAmplitudeSpinBox->value(), 0, 'f', 1)
        .arg(busy ? "Injecting test frames; edits apply to the next flash."
                  : "Injected frames are marked as test data."));
    if (centre && latestDetection_.probeX == centre->x() && latestDetection_.probeY == centre->y()
        && latestDetection_.probeNoise > 0)
    {
        ui_->blipStatusLabel->setText(ui_->blipStatusLabel->text()
            + QString("  %1 sigma at centre (noise %2 ADU).")
                .arg(ui_->blipAmplitudeSpinBox->value() / latestDetection_.probeNoise, 0, 'f', 1)
                .arg(latestDetection_.probeNoise, 0, 'f', 2));
    }
}

std::unique_ptr<FrameSource> MainWindow::makeSource()
{
    synthetic_ = nullptr;
    recording_ = nullptr;
    const int fps = ui_->targetFpsSpinBox->value();

    if (!recordingSelected())
    {
        auto source = std::make_unique<SyntheticFrameSource>(kSyntheticWidth, kSyntheticHeight, fps);
        synthetic_ = source.get();
        return source;
    }

    if (recordingPath_.isEmpty())
    {
        statusBar()->showMessage("Choose a recording file first");
        return nullptr;
    }

    auto source = std::make_unique<RecordingSource>(std::filesystem::path(recordingPath_.toStdWString()));
    source->setLoopSeconds(ui_->loopCheckBox->isChecked() ? ui_->loopSecondsSpinBox->value() : 0.0);
    source->setNominalFps(fps);
    source->setSpeed(ui_->speedComboBox->currentIndex() == kFastEntry
        ? RecordingSource::Speed::Fast : RecordingSource::Speed::RealTime);
    recording_ = source.get();
    return source;
}

void MainWindow::onStartClicked()
{
    std::unique_ptr<FrameSource> source = makeSource();
    if (!source)
    {
        return;
    }
    session_->setSource(std::move(source));
    ui_->previewWidget->clear();
    latestDetection_ = {};
    updateDetectionDisplay();
    updateBlipReadout();

    QString error;
    if (!session_->start(error))
    {
        synthetic_ = nullptr;
        recording_ = nullptr;
        statusBar()->showMessage(QString("Cannot start: %1").arg(error));
        updateControls();
        updateStatusLabel();
        return;
    }

    sourceFailed_ = false;
    recordFailed_ = false;
    playbackEnded_ = false;
    candidates_ = 0;
    clipsNotSaved_ = 0;
    lastAcquiredSample_ = 0;
    lastFpsSampleTime_ = std::chrono::steady_clock::now();
    framesLabel_->setText("Frames: 0");
    fpsLabel_->setText("FPS: --");
    updateCandidateReadout();
    updateBufferReadout();
    statusBar()->clearMessage();

    bufferReadoutTimer_->start();
    updateControls();
    updateStatusLabel();
    updateBlipReadout();
}

void MainWindow::onStopClicked()
{
    finishStream();
}

void MainWindow::finishStream()
{
    bufferReadoutTimer_->stop();
    session_->stop();
    saveDetectionSettings();

    fpsLabel_->setText("FPS: --");
    framesLabel_->setText(QString("Frames: %1").arg(session_->acquiredFrames()));
    updateControls();
    updateStatusLabel();
    updateBufferReadout();
    updateBlipReadout();
}

void MainWindow::onOpenFileClicked()
{
    const QString startDir = recordingPath_.isEmpty() ? QString() : QFileInfo(recordingPath_).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, "Open recording", startDir,
        "Recordings (*.ser *.avi);;SER (*.ser);;AVI (*.avi);;All files (*)");
    if (path.isEmpty())
    {
        return;
    }

    recordingPath_ = path;
    ui_->fileLabel->setText(QFileInfo(path).fileName());
    ui_->fileLabel->setToolTip(QDir::toNativeSeparators(path));
}

void MainWindow::onSourceKindChanged()
{
    updateControls();
}

void MainWindow::onInjectFlashClicked()
{
    if (synthetic_)
    {
        synthetic_->requestFlash();
    }
}

void MainWindow::onInjectBlipClicked()
{
    const auto centre = ui_->previewWidget->selectedPosition();
    if (!centre)
    {
        return;
    }
    const BlipSettings settings{centre->x(), centre->y(), ui_->blipFwhmSpinBox->value(),
        ui_->blipAmplitudeSpinBox->value(), ui_->blipFramesSpinBox->value()};
    if (session_->requestBlip(settings))
    {
        statusBar()->showMessage("Test flash requested");
    }
    updateBlipReadout();
}

void MainWindow::onTargetFpsChanged(int fps)
{
    if (synthetic_)
    {
        synthetic_->setTargetFps(fps);
    }
}

void MainWindow::onSpeedChanged()
{
    if (recording_)
    {
        recording_->setSpeed(ui_->speedComboBox->currentIndex() == kFastEntry
            ? RecordingSource::Speed::Fast : RecordingSource::Speed::RealTime);
    }
}

void MainWindow::showLatestFrame()
{
    const PreviewFrame packet = session_->takePreview();
    const FramePtr& frame = packet.frame;
    if (frame)
    {
        updateFrameLabel(*frame);
        ui_->previewWidget->setImage(frameToPreviewImage(*frame));
        if (packet.detection)
        {
            const auto& view = *packet.detection;
            latestDetection_ = view.summary;
            detectionPanel_->setView(view);
            if (detectionDock_->isVisible())
                ui_->previewWidget->setDetectionOverlay(detectionPanel_->overlayImage(),
                    {view.summary.boxX, view.summary.boxY, view.summary.boxWidth, view.summary.boxHeight});
        }
        updateBlipReadout();
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

    if (!result.notSaved.isEmpty())
    {
        ++clipsNotSaved_;
        updateCandidateReadout();
        statusBar()->showMessage(QString("Clip not saved: %1").arg(result.notSaved));
    }
    else if (!result.folder.isEmpty())
    {
        statusBar()->showMessage(result.folder);
    }

    updateStatusLabel();
}

void MainWindow::onPlaybackEnded()
{
    // A notification from an earlier run is ignored.
    if (!session_->isRunning() || !session_->sourceFinished())
    {
        return;
    }
    playbackEnded_ = true;
    finishStream();
    statusBar()->showMessage("Playback finished");
}

void MainWindow::onSourceFailed(const QString& message)
{
    if (!session_->isRunning() || !session_->sourceFinished())
    {
        return;
    }
    sourceFailed_ = true;
    finishStream();
    statusBar()->showMessage(message);
}
