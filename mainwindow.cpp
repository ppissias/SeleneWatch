#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "detection_panel.h"
#include "display_panel.h"
#include "histogram_panel.h"
#include "camera_panel.h"
#include "qhy_camera_panel.h"

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
#include <algorithm>
#include <utility>

namespace {

// Fast playback entry in the form's speed box.
constexpr int kFastEntry = 1;

constexpr int kSyntheticWidth = 640;
constexpr int kSyntheticHeight = 480;

} // namespace

MainWindow::MainWindow(QWidget *parent, QString outputRoot,std::shared_ptr<ZwoApi> cameraApi,std::shared_ptr<QhyApi> qhyApi)
    : QMainWindow(parent)
    , ui_(std::make_unique<Ui::MainWindowClass>())
{
    ui_->setupUi(this);
    ui_->sourceComboBox->setItemData(0,"synthetic");
    ui_->sourceComboBox->setItemData(1,"recording");

    // Clips are written next to the executable.
    if (outputRoot.isEmpty()) outputRoot = QDir(QCoreApplication::applicationDirPath()).filePath("events");

    Session::Notifications notifications;
    notifications.previewReady = [this] { showLatestFrame(); };
    notifications.candidateStarted = [this] { onCandidateStarted(); };
    notifications.modeChanged = [this] { updateControls(); updateStatusLabel(); };
    notifications.cameraSettingsChanged = [this] {
        if (session_->mode()!=Session::Mode::Preview) return;
        if (camera_) cameraPanel_->showLiveActual(camera_->actualProfile());
        else if (qhyCamera_) qhyPanel_->showLiveActual(qhyCamera_->actualProfile());
        else return;
        statusBar()->showMessage("Camera controls read back. Preview is settling and relearning.",4000);
    };
    notifications.manualSaveHandled = [this](bool saved) {
        if (saved) { ++manualCaptures_; updateCandidateReadout(); }
        updateControls();
        statusBar()->showMessage(saved ? "Manual capture marked; collecting following frames." : "Manual capture unavailable: no frames acquired.",5000);
    };
    notifications.clipWritten = [this](const ClipWriteResult& result) { onClipWritten(result); };
    notifications.ended = [this] { onPlaybackEnded(); };
    notifications.failed = [this](const QString& message) { onSourceFailed(message); };
    session_ = std::make_unique<Session>(this, outputRoot, std::move(notifications));

    setupStatusBar();
    setupDetectionPanel();
    setupDisplayPanel();
    setupHistogramPanel();
    // Only builds with the adapter and tests supplying a fake SDK get the camera entry and dock.
    cameraApi_=cameraApi?std::move(cameraApi):makeZwoApi();
    if (cameraApi_)
    {
        ui_->sourceComboBox->addItem("ZWO ASI camera","zwo");
        cameraPanel_=new CameraPanel(cameraApi_,this);
        cameraDock_=new QDockWidget("Camera",this); cameraDock_->setObjectName("cameraDock");
        cameraDock_->setWidget(cameraPanel_); addDockWidget(Qt::LeftDockWidgetArea,cameraDock_);
        cameraDock_->hide();
        connect(cameraPanel_,&CameraPanel::applyRequested,this,&MainWindow::onApplyCamera);
        connect(cameraPanel_,&CameraPanel::liveControlsRequested,this,&MainWindow::onApplyLiveCamera);
    }
    qhyApi_=qhyApi?std::move(qhyApi):makeQhyApi();
    if (qhyApi_)
    {
        ui_->sourceComboBox->addItem("QHY camera","qhy");
        qhyPanel_=new QhyCameraPanel(qhyApi_,this);
        qhyDock_=new QDockWidget("QHY camera",this);qhyDock_->setObjectName("qhyCameraDock");
        qhyDock_->setWidget(qhyPanel_);addDockWidget(Qt::LeftDockWidgetArea,qhyDock_);
        qhyDock_->hide();
        connect(qhyPanel_,&QhyCameraPanel::applyRequested,this,&MainWindow::onApplyCamera);
        connect(qhyPanel_,&QhyCameraPanel::liveControlsRequested,this,&MainWindow::onApplyLiveCamera);
        connect(qhyPanel_,&QhyCameraPanel::availabilityChanged,this,&MainWindow::updateControls);
    }

    connect(ui_->startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui_->stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui_->monitoringButton, &QPushButton::clicked, this, &MainWindow::onMonitoringClicked);
    connect(ui_->saveNowButton, &QPushButton::clicked, this, &MainWindow::onSaveNowClicked);
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
    updateStatusLabel();
    updateBlipReadout();
}

MainWindow::~MainWindow()
{
    // QMainWindow may hide its docks while its base destructor runs, after
    // this derived window and its members have ceased to exist.
    if (detectionDock_) disconnect(detectionDock_, nullptr, this, nullptr);
    if (detectionPanel_) disconnect(detectionPanel_, nullptr, this, nullptr);
    if (displayDock_) disconnect(displayDock_, nullptr, this, nullptr);
    if (displayPanel_) disconnect(displayPanel_, nullptr, this, nullptr);
    if (histogramDock_) disconnect(histogramDock_, nullptr, this, nullptr);
    if (histogramPanel_) disconnect(histogramPanel_, nullptr, this, nullptr);
    saveDetectionSettings();
    saveDisplaySettings();
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
    settings.maxPatchBoxRatio = saved.value("detection/maxPatchBoxRatio", settings.maxPatchBoxRatio).toDouble();
    settings.referenceTimeConstantSeconds = saved.value("detection/referenceTimeConstantSeconds", settings.referenceTimeConstantSeconds).toDouble();
    settings.minNoiseAdu = saved.value("detection/minNoiseAdu", settings.minNoiseAdu).toDouble();
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
    saved.setValue("detection/maxPatchBoxRatio", values.maxPatchBoxRatio);
    saved.setValue("detection/referenceTimeConstantSeconds", values.referenceTimeConstantSeconds);
    saved.setValue("detection/minNoiseAdu", values.minNoiseAdu);
    saved.sync();
}

void MainWindow::setupDisplayPanel()
{
    displayPanel_ = new DisplayPanel(this);
    displayDock_ = new QDockWidget("Display and capture", this);
    displayDock_->setObjectName("displayDock"); displayDock_->setWidget(displayPanel_);
    addDockWidget(Qt::RightDockWidgetArea, displayDock_);
    tabifyDockWidget(detectionDock_, displayDock_);
    menuBar()->actions().front()->menu()->addAction(displayDock_->toggleViewAction());
    QSettings saved;
    DisplaySettings display;
    const int mode = saved.value("display/mode", 0).toInt();
    display.mode = mode >= 0 && mode <= 2 ? static_cast<DisplayMode>(mode) : DisplayMode::Linear;
    display.black = saved.value("display/black", 0).toDouble(); display.white = saved.value("display/white", 1).toDouble();
    display.invert = saved.value("display/invert", false).toBool(); display.debayer = saved.value("display/debayer", true).toBool();
    displayPanel_->setDisplaySettings(display.valid() ? display : DisplaySettings{});
    CaptureSettings capture;
    capture.historySeconds = saved.value("capture/history", 5).toDouble();
    capture.postSeconds = saved.value("capture/post", 3).toDouble();
    capture.maxPostSeconds = saved.value("capture/maxPost", 10).toDouble();
    capture.memoryBytes = static_cast<size_t>(std::clamp(saved.value("capture/memoryMiB", 2048).toInt(), 16, 65536)) * 1024 * 1024;
    displayPanel_->setCaptureSettings(capture.valid() ? capture : CaptureSettings{});
    connect(displayPanel_, &DisplayPanel::displayChanged, this, &MainWindow::renderPreview);
    displayDock_->raise();
    updateBufferReadout();
}

void MainWindow::saveDisplaySettings()
{
    if (!displayPanel_) return;
    const auto display = displayPanel_->displaySettings();
    const auto capture = displayPanel_->captureSettings();
    QSettings saved;
    saved.setValue("display/mode", static_cast<int>(display.mode));
    saved.setValue("display/black", display.black); saved.setValue("display/white", display.white);
    saved.setValue("display/invert", display.invert); saved.setValue("display/debayer", display.debayer);
    saved.setValue("histogram/visible", !histogramDock_->isHidden());
    saved.setValue("histogram/fitRange", histogramPanel_->fitRange());
    saved.setValue("histogram/logCounts", histogramPanel_->logCounts());
    saved.setValue("capture/history", capture.historySeconds); saved.setValue("capture/post", capture.postSeconds);
    saved.setValue("capture/maxPost", capture.maxPostSeconds); saved.setValue("capture/memoryMiB", static_cast<int>(capture.memoryBytes / (1024 * 1024)));
    saved.sync();
}

void MainWindow::setupHistogramPanel()
{
    histogramPanel_ = new HistogramPanel(this);
    histogramDock_ = new QDockWidget("Histogram", this);
    histogramDock_->setObjectName("histogramDock");
    histogramDock_->setWidget(histogramPanel_);
    addDockWidget(Qt::BottomDockWidgetArea, histogramDock_);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    menuBar()->actions().front()->menu()->addAction(histogramDock_->toggleViewAction());

    const QSettings saved;
    histogramPanel_->setOptions(saved.value("histogram/fitRange", false).toBool(),
        saved.value("histogram/logCounts", true).toBool());
    histogramDock_->setVisible(saved.value("histogram/visible", false).toBool());
    connect(histogramDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) updateHistogram(true);
    });
    connect(histogramPanel_, &HistogramPanel::rangeChanged, this, [this] { updateHistogram(true); });
}

// Sample the latest preview frame at GUI cadence, with an immediate refresh on visibility or range changes.
void MainWindow::updateHistogram(bool force)
{
    if (!latestFrame_ || !histogramDock_->isVisible()) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - lastHistogramTime_ < std::chrono::milliseconds{200}) return;
    histogramPanel_->setFrame(*latestFrame_);
    lastHistogramTime_ = now;
}

void MainWindow::renderPreview()
{
    if (!latestFrame_) return;
    try
    {
        DisplayLevels levels;
        QImage image = frameToPreviewImage(*latestFrame_, displayPanel_->displaySettings(), &levels);
        ui_->previewWidget->setImage(std::move(image));
        displayPanel_->showLevels(levels);
    }
    catch (const MemoryLimitError&) { statusBar()->showMessage("Preview skipped: RAM ceiling reached"); }
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
    modeLabel_=new QLabel(this); modeLabel_->setObjectName("captureModeLabel");
    modeLabel_->setWordWrap(true); modeLabel_->setMargin(8);
    ui_->verticalLayout->insertWidget(2,modeLabel_);
    statusLabel_ = new QLabel("Status: Idle", this);
    fpsLabel_ = new QLabel("FPS: --", this);
    framesLabel_ = new QLabel("Frames: 0", this);
    framesLabel_->setObjectName("framesReadout");
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
    return ui_->sourceComboBox->currentData().toString()=="recording";
}
bool MainWindow::cameraSelected() const { return ui_->sourceComboBox->currentData().toString()=="zwo" && cameraPanel_; }
bool MainWindow::qhySelected() const { return ui_->sourceComboBox->currentData().toString()=="qhy" && qhyPanel_; }
bool MainWindow::anyCameraSelected() const { return cameraSelected() || qhySelected(); }

// Which controls can be used depends on the source and on whether it runs.
void MainWindow::updateControls()
{
    const bool running = session_->isRunning();
    const bool recording = recordingSelected();
    const bool monitoring = session_->mode()==Session::Mode::Monitoring;
    const bool pending = session_->modeChangePending();

    ui_->startButton->setEnabled(!running && (!qhySelected() || qhyPanel_->hasCamera()));
    ui_->stopButton->setEnabled(running);
    ui_->monitoringButton->setEnabled(running && !session_->sourceFinished() && !pending);
    ui_->monitoringButton->setText(monitoring?"Stop monitoring":"Start monitoring");
    ui_->saveNowButton->setEnabled(running && monitoring && !pending && !session_->sourceFinished() && !session_->manualSavePending());
    ui_->injectFlashButton->setEnabled(running && synthetic_ != nullptr);

    ui_->sourceComboBox->setEnabled(!running);
    ui_->openFileButton->setEnabled(!running && recording);
    ui_->loopCheckBox->setEnabled(!running && recording);
    ui_->loopSecondsSpinBox->setEnabled(!running && recording && ui_->loopCheckBox->isChecked());

    // The speed may change while a recording plays.
    ui_->speedComboBox->setEnabled(recording);
    ui_->targetFpsSpinBox->setEnabled(!anyCameraSelected());
    if (displayPanel_) displayPanel_->setRunning(running, recording || anyCameraSelected());
    if (cameraPanel_)
    {
        cameraDock_->setEnabled(cameraSelected());
        cameraPanel_->setRunning(running && cameraSelected(),running && (monitoring || pending));
    }
    if (qhyPanel_)
    {
        qhyDock_->setEnabled(qhySelected());
        qhyPanel_->setRunning(running && qhySelected(),running && (monitoring || pending));
    }
}

void MainWindow::updateBufferReadout()
{
    if (!bufferLabel_)
    {
        return;
    }

    // The memory is that of every live frame: the ring, clips being
    // collected or waiting for the writer, and buffers waiting for reuse.
    const auto memory = session_->memoryUsage();
    const auto history = session_->historyUsage();
    const auto writer = session_->writerUsage();
    bufferLabel_->setText(QString("History %1 s / %2 frames; RAM %3 / %4 MiB")
        .arg(history.seconds, 0, 'f', 1).arg(history.frames)
        .arg(memory.used / 1048576.0, 0, 'f', 1).arg(memory.limit / 1048576.0, 0, 'f', 0));
    if (displayPanel_) displayPanel_->showUsage(memory, history, writer);
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
    if (camera_ || qhyCamera_)
    {
        const auto drops=session_->droppedFrames();
        framesLabel_->setText(framesLabel_->text()+QString(" | SDK drops: %1").arg(drops?QString::number(*drops):"unknown"));
    }
}

void MainWindow::updateCandidateReadout()
{
    QString text = QString("Candidates: %1").arg(candidates_);
    if (manualCaptures_>0) text += QString(" | Manual: %1").arg(manualCaptures_);
    if (clipsNotSaved_ > 0)
    {
        text += QString(" (%1 not saved)").arg(clipsNotSaved_);
    }
    if (clipsLimited_ > 0) text += QString(" (%1 RAM limits)").arg(clipsLimited_);
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
        statusLabel_->setText(session_->mode()==Session::Mode::Monitoring?"Status: Monitoring":"Status: Preview");
    }
    else if (playbackEnded_)
    {
        statusLabel_->setText("Status: Playback finished");
    }
    else
    {
        statusLabel_->setText("Status: Stopped");
    }
    const bool monitoring=session_->mode()==Session::Mode::Monitoring;
    detectionPanel_->setMonitoring(monitoring);
    modeLabel_->setStyleSheet(monitoring?"background:#164b3c; color:white; font-weight:bold;":"background:#26364e; color:white; font-weight:bold;");
    if (session_->modeChangePending())
        modeLabel_->setText(monitoring?"Stopping monitoring… finishing the current capture before returning to Preview.":"Starting monitoring… applying the capture boundary at the next frame.");
    else if (!session_->isRunning())
        modeLabel_->setText("STOPPED — select an input and start Preview to tune it.");
    else if (monitoring)
        modeLabel_->setText(recordFailed_?"MONITORING — recording failed. Check the reported storage error.":"MONITORING — event capture ON. Stop monitoring to return to Preview and tune camera settings.");
    else
        modeLabel_->setText("PREVIEW — capture OFF. Tune the camera and detector, then start monitoring. Previously queued clips can finish writing.");
}

void MainWindow::updateFrameLabel(const Frame& frame)
{
    QString text = QString("%1 x %2 %3").arg(frame.width).arg(frame.height).arg(pixelFormatName(frame.format));
    if (frame.format == PixelFormat::Mono16)
    {
        if (camera_ || qhyCamera_) text += ", delivered 16-bit words";
        else text += QString(", %1 bits %2").arg(frame.significantBits)
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
    displayPanel_->setNativeMaximum(maximum);
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
    camera_ = nullptr;
    qhyCamera_ = nullptr;
    const int fps = ui_->targetFpsSpinBox->value();

    if (qhySelected())
    {
        if (!qhyPanel_->hasCamera()) { statusBar()->showMessage("Connect a QHY camera and refresh its list");return {}; }
        auto source=std::make_unique<QhySource>(qhyApi_,qhyPanel_->profile(),qhyPanel_->settings());
        qhyCamera_=source.get();return source;
    }

    if (cameraSelected())
    {
        if (!cameraPanel_->hasCamera()) { statusBar()->showMessage("Connect an ASI camera and refresh its list"); return {}; }
        auto source=std::make_unique<ZwoSource>(cameraApi_,cameraPanel_->profile(),cameraPanel_->settings());
        camera_=source.get(); return source;
    }
    if (!recordingSelected())
    {
        auto source = std::make_unique<SyntheticFrameSource>(kSyntheticWidth, kSyntheticHeight, fps);
        const int kind = displayPanel_->syntheticFormat();
        if (kind) source->setPixelLayout(PixelFormat::Mono16, kind == 3 ? 16 : 12,
            kind == 2 || kind == 4 ? BitAlignment::High : BitAlignment::Low,
            kind == 4 ? BayerPattern::RGGB : BayerPattern::None);
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
    startStream(Session::Mode::Preview);
}

void MainWindow::startStream(Session::Mode mode)
{
    std::unique_ptr<FrameSource> source = makeSource();
    if (!source)
    {
        return;
    }
    session_->setSource(std::move(source));
    ui_->previewWidget->clear();
    latestFrame_.reset();
    histogramPanel_->clear();
    lastHistogramTime_ = {};
    detectionPanel_->setView({});
    session_->setCaptureSettings(displayPanel_->captureSettings());
    latestDetection_ = {};
    updateDetectionDisplay();
    updateBlipReadout();

    QString error;
    if (!session_->start(error,mode))
    {
        synthetic_ = nullptr;
        recording_ = nullptr;
        camera_ = nullptr;
        qhyCamera_ = nullptr;
        statusBar()->showMessage(QString("Cannot start: %1").arg(error));
        updateControls();
        updateStatusLabel();
        return;
    }

    sourceFailed_ = false;
    if (camera_) cameraPanel_->showActual(camera_->actualProfile());
    if (qhyCamera_) qhyPanel_->showActual(qhyCamera_->actualProfile());
    recordFailed_ = false;
    playbackEnded_ = false;
    candidates_ = 0;
    manualCaptures_ = 0;
    clipsNotSaved_ = 0;
    clipsLimited_ = 0;
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

void MainWindow::onMonitoringClicked()
{
    if (!session_->isRunning() || session_->modeChangePending()) return;
    const bool monitoring=session_->mode()==Session::Mode::Monitoring;
    if (!monitoring && (camera_ || qhyCamera_))
    {
        // Reopen with the requested controls, then read them back before any
        // evidence frame. Never assign live Preview readbacks to buffered data.
        if (!validateCameraSettings()) return;
        bufferReadoutTimer_->stop();
        session_->stop("monitoring settings boundary");
        startStream(Session::Mode::Monitoring);
        return;
    }
    if (session_->requestMonitoring(!monitoring))
    {
        updateControls(); updateStatusLabel();
    }
}

void MainWindow::finishStream()
{
    bufferReadoutTimer_->stop();
    session_->stop();
    saveDetectionSettings();
    saveDisplaySettings();

    updateStreamReadout();
    fpsLabel_->setText("FPS: --");
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

    selectRecording(path);
}

void MainWindow::selectRecording(QString path)
{
    if (session_->isRunning()) return;
    recordingPath_ = std::move(path);
    ui_->sourceComboBox->setCurrentIndex(1);
    ui_->fileLabel->setText(QFileInfo(recordingPath_).fileName());
    ui_->fileLabel->setToolTip(QDir::toNativeSeparators(recordingPath_));
    updateControls();
}

void MainWindow::onSourceKindChanged()
{
    if (session_->isRunning()) return;
    if (cameraDock_) cameraDock_->setVisible(cameraSelected());
    if (qhyDock_) qhyDock_->setVisible(qhySelected());
    if (cameraSelected())
    {
        cameraDock_->raise();
        detectionDock_->raise();
        cameraPanel_->refresh();
    }
    if (qhySelected())
    {
        qhyDock_->raise();
        detectionDock_->raise();
        qhyPanel_->refresh();
    }
    if (recordingSelected())
    {
        ui_->fileLabel->setText(recordingPath_.isEmpty()?"No recording chosen":QFileInfo(recordingPath_).fileName());
        ui_->fileLabel->setToolTip(QDir::toNativeSeparators(recordingPath_));
    }
    else
    {
        ui_->fileLabel->clear();
        ui_->fileLabel->setToolTip({});
    }
    updateControls();
}

bool MainWindow::validateCameraSettings()
{
    try
    {
        if (qhySelected() && qhyPanel_->hasCamera())
            QhySource::validateSettings(qhyPanel_->profile().camera,qhyPanel_->settings());
        else if (cameraSelected() && cameraPanel_->hasCamera())
            ZwoSource::validateSettings(cameraPanel_->profile().camera,cameraPanel_->settings());
        else return false;
        return true;
    }
    catch (const std::exception& ex)
    {
        statusBar()->showMessage(QString::fromUtf8(ex.what()));
        return false;
    }
}

void MainWindow::onApplyCamera()
{
    if (session_->mode()==Session::Mode::Monitoring || session_->modeChangePending() || !validateCameraSettings()) return;
    if (session_->isRunning())
    {
        bufferReadoutTimer_->stop(); session_->stop("camera settings changed");
        startStream(Session::Mode::Preview);
    }
    else if (cameraSelected())
    {
        ZwoSource source(cameraApi_,cameraPanel_->profile(),cameraPanel_->settings());
        std::string error;
        if (source.open(error)) { cameraPanel_->showActual(source.actualProfile()); statusBar()->showMessage("Camera settings read back; ready to Start",5000); }
        else statusBar()->showMessage(QString::fromStdString(error));
    }
    else
    {
        QhySource source(qhyApi_,qhyPanel_->profile(),qhyPanel_->settings());std::string error;
        if (source.open(error)) { qhyPanel_->showActual(source.actualProfile());statusBar()->showMessage("QHY settings read back; ready to Start",5000); }
        else statusBar()->showMessage(QString::fromStdString(error));
    }
    updateControls();
}

void MainWindow::onApplyLiveCamera()
{
    if (session_->mode()!=Session::Mode::Preview || session_->modeChangePending() || session_->sourceFinished()) return;
    try
    {
        if (camera_) camera_->requestLiveControls(cameraPanel_->settings().controls);
        else if (qhyCamera_) qhyCamera_->requestLiveControls(qhyPanel_->settings().controls);
    }
    catch (const std::exception& ex) { statusBar()->showMessage(QString::fromUtf8(ex.what())); }
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
        latestFrame_ = frame;
        renderPreview();
        updateHistogram();
        if (packet.detection)
        {
            const auto& view = *packet.detection;
            latestDetection_ = view.summary;
            try { detectionPanel_->setView(view); }
            catch (const MemoryLimitError&) { statusBar()->showMessage("Overlay skipped: RAM ceiling reached"); }
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

void MainWindow::onSaveNowClicked()
{
    if (session_->requestManualSave())
        statusBar()->showMessage("Manual capture requested",3000);
    updateControls();
}

void MainWindow::onClipWritten(const ClipWriteResult& result)
{
    if (result.memoryLimited) { ++clipsLimited_; updateCandidateReadout(); }
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
        statusBar()->showMessage(result.memoryLimited ? "Clip saved at RAM limit: " + result.folder : result.folder);
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
