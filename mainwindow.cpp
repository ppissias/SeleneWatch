#include "mainwindow.h"

#include "preview_image.h"
#include "sources/recording_source.h"
#include "sources/synthetic_frame_source.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);

    // Clips are written next to the executable.
    const QString outputRoot = QDir(QCoreApplication::applicationDirPath()).filePath("events");

    Session::Notifications notifications;
    notifications.previewReady = [this] { showLatestFrame(); };
    notifications.candidateStarted = [this] { onCandidateStarted(); };
    notifications.clipWritten = [this](const ClipWriteResult& result) { onClipWritten(result); };
    notifications.ended = [this] { onPlaybackEnded(); };
    notifications.failed = [this](const QString& message) { onSourceFailed(message); };
    session_ = std::make_unique<Session>(this, outputRoot, std::move(notifications));

    setupStatusBar();

    connect(ui.startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui.stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui.openFileButton, &QPushButton::clicked, this, &MainWindow::onOpenFileClicked);
    connect(ui.injectFlashButton, &QPushButton::clicked, this, &MainWindow::onInjectFlashClicked);
    connect(ui.injectBlipButton, &QPushButton::clicked, this, &MainWindow::onInjectBlipClicked);
    connect(ui.previewWidget, &PreviewWidget::positionSelected, this, [this] {
        updateBlipReadout();
    });
    connect(ui.blipAmplitudeSpinBox, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateBlipReadout);
    connect(ui.sourceComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onSourceKindChanged);
    connect(ui.speedComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onSpeedChanged);
    connect(ui.loopCheckBox, &QCheckBox::toggled, this, &MainWindow::updateControls);
    connect(ui.targetFpsSpinBox, &QSpinBox::valueChanged, this, &MainWindow::onTargetFpsChanged);

    bufferReadoutTimer_ = new QTimer(this);
    bufferReadoutTimer_->setInterval(1000);
    connect(bufferReadoutTimer_, &QTimer::timeout, this, &MainWindow::updateStreamReadout);

    updateControls();
    updateBlipReadout();
}

MainWindow::~MainWindow()
{
    if (bufferReadoutTimer_)
    {
        bufferReadoutTimer_->stop();
    }
    // The session stops its source and writes what is queued.
    session_.reset();
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
    return ui.sourceComboBox->currentIndex() != kSyntheticEntry;
}

// Which controls can be used depends on the source and on whether it runs.
void MainWindow::updateControls()
{
    const bool running = session_->isRunning();
    const bool recording = recordingSelected();

    ui.startButton->setEnabled(!running);
    ui.stopButton->setEnabled(running);
    ui.injectFlashButton->setEnabled(running && synthetic_ != nullptr);

    ui.sourceComboBox->setEnabled(!running);
    ui.openFileButton->setEnabled(!running && recording);
    ui.loopCheckBox->setEnabled(!running && recording);
    ui.loopSecondsSpinBox->setEnabled(!running && recording && ui.loopCheckBox->isChecked());

    // The speed may change while a recording plays.
    ui.speedComboBox->setEnabled(recording);
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
    ui.blipAmplitudeSpinBox->setMaximum(maximum);
}

void MainWindow::updateBlipReadout()
{
    const auto centre = ui.previewWidget->selectedPosition();
    const bool busy = session_->blipBusy();
    ui.injectBlipButton->setEnabled(session_->isRunning() && !session_->sourceFinished() && centre && !busy);
    ui.blipPositionLabel->setText(centre
        ? QString("Centre: (%1, %2)").arg(centre->x()).arg(centre->y())
        : QString("Click the preview to place a flash"));
    ui.blipStatusLabel->setText(QString("Next peak: %1 ADU. %2")
        .arg(ui.blipAmplitudeSpinBox->value(), 0, 'f', 1)
        .arg(busy ? "Injecting test frames; edits apply to the next flash."
                  : "Injected frames are marked as test data."));
}

std::unique_ptr<FrameSource> MainWindow::makeSource()
{
    synthetic_ = nullptr;
    recording_ = nullptr;
    const int fps = ui.targetFpsSpinBox->value();

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
    source->setLoopSeconds(ui.loopCheckBox->isChecked() ? ui.loopSecondsSpinBox->value() : 0.0);
    source->setNominalFps(fps);
    source->setSpeed(ui.speedComboBox->currentIndex() == kFastEntry
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
    ui.previewWidget->clear();
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
    ui.fileLabel->setText(QFileInfo(path).fileName());
    ui.fileLabel->setToolTip(QDir::toNativeSeparators(path));
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
    const auto centre = ui.previewWidget->selectedPosition();
    if (!centre)
    {
        return;
    }
    const BlipSettings settings{centre->x(), centre->y(), ui.blipFwhmSpinBox->value(),
        ui.blipAmplitudeSpinBox->value(), ui.blipFramesSpinBox->value()};
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
        recording_->setSpeed(ui.speedComboBox->currentIndex() == kFastEntry
            ? RecordingSource::Speed::Fast : RecordingSource::Speed::RealTime);
    }
}

void MainWindow::showLatestFrame()
{
    const FramePtr frame = session_->takePreviewFrame();
    if (frame)
    {
        updateFrameLabel(*frame);
        ui.previewWidget->setImage(frameToPreviewImage(*frame));
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
