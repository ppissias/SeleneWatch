#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "detection_panel.h"
#include "display_panel.h"
#include "elided_label.h"
#include "histogram_panel.h"
#include "guiding_panel.h"
#include "camera_panel.h"
#include "camera_support_dialog.h"
#include "playerone_camera_panel.h"
#include "qhy_camera_panel.h"
#include "touptek_camera_panel.h"

#include "guiding/ascom_mount.h"
#include "guiding/simulated_mount.h"
#include "preview_image.h"
#include "recording/metadata_text.h"
#include "camera_settings_editor.h"
#include "selenewatch_version.h"
#include "site_dialog.h"
#include "sky_schedule.h"
#include "target_defaults.h"
#include "sources/recording_source.h"
#include "sources/synthetic_frame_source.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimeZone>
#include <QtCore/QUrl>
#include <QtGui/QAction>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>

#include <filesystem>
#include <algorithm>
#include <utility>

namespace {

// Fast playback entry in the form's speed box.
constexpr int kFastEntry = 1;

constexpr int kSyntheticWidth = 640;
constexpr int kSyntheticHeight = 480;

constexpr uint64_t kGiB = uint64_t{1024} * 1024 * 1024;

// Where a target's settings are saved. The Moon keeps the keys used before
// Sky mode existed, so an observer who never picks Sky sees no change.
QString settingsPrefix(Target target)
{
    return target == Target::Sky ? "sky/" : "";
}

// The mask's shapes for the preview.
std::vector<QPolygon> maskRects(const DetectionMask& mask)
{
    std::vector<QPolygon> shapes;
    for (const MaskShape& shape : mask.shapes())
    {
        QPolygon polygon;
        for (const MaskPoint& point : shape) polygon << QPoint(point.x, point.y);
        shapes.push_back(polygon);
    }
    return shapes;
}

} // namespace

MainWindow::MainWindow(QWidget *parent, QString outputRoot,std::shared_ptr<ZwoApi> cameraApi,std::shared_ptr<QhyApi> qhyApi,
    std::shared_ptr<PlayerOneApi> playerOneApi, std::shared_ptr<ToupTekApi> toupTekApi)
    : QMainWindow(parent)
    , ui_(std::make_unique<Ui::MainWindowClass>())
{
    ui_->setupUi(this);
    setWindowTitle(QString("SeleneWatch %1 — Lunar Observation Monitor").arg(SELENEWATCH_RELEASE));

    // Clips are written next to the executable.
    if (outputRoot.isEmpty()) outputRoot = QDir(QCoreApplication::applicationDirPath()).filePath("events");

    Session::Notifications notifications;
    notifications.previewReady = [this] { showLatestFrame(); };
    notifications.candidateStarted = [this] { onCandidateStarted(); };
    notifications.modeChanged = [this] { updateControls(); updateStatusLabel(); };
    notifications.cameraSettingsChanged = [this] {
        if (session_->mode()!=Session::Mode::Preview) return;
        CameraFamilyPanel* camera=activeCamera();
        if (!camera) return;
        camera->showSourceReadback(true);
        showMessage("Camera controls read back. Preview is settling and relearning.",4000);
    };
    notifications.manualSaveHandled = [this](bool saved) {
        if (saved) { ++manualCaptures_; updateCandidateReadout(); }
        updateControls();
        showMessage(saved ? "Manual capture marked; collecting following frames." : "Manual capture unavailable: no frames acquired.",5000);
    };
    notifications.clipWritten = [this](const ClipWriteResult& result) { onClipWritten(result); };
    notifications.ended = [this] { onPlaybackEnded(); };
    notifications.failed = [this](const QString& message) { onSourceFailed(message); };
    notifications.lockChanged = [this] { onLockChanged(); };
    notifications.recordingChanged = [this] { onRecordingChanged(); };
    session_ = std::make_unique<Session>(this, outputRoot, std::move(notifications));

    setupStatusBar();
    setupDetectionPanel();
    setupDisplayPanel();
    setupHistogramPanel();
    setupGuidingPanel();
    setupSessionMenu();
    setupHelpMenu();
    applyTarget();
    // Only builds with an adapter, and tests supplying a test camera, get a
    // camera family's Source entry and dock. No family touches its SDK yet.
    if (auto api=cameraApi?std::move(cameraApi):makeZwoApi())
        addCameraFamily(new CameraPanel(std::move(api),this),"Camera","cameraDock");
    if (auto api=qhyApi?std::move(qhyApi):makeQhyApi())
        addCameraFamily(new QhyCameraPanel(std::move(api),this),"QHY camera","qhyCameraDock");
    if (auto api=playerOneApi?std::move(playerOneApi):makePlayerOneApi())
        addCameraFamily(new PlayerOneCameraPanel(std::move(api),this),"Player One camera","playerOneCameraDock");
    if (auto api=toupTekApi?std::move(toupTekApi):makeToupTekApi())
        addCameraFamily(new ToupTekCameraPanel(std::move(api),this),"ToupTek camera","toupTekCameraDock");
    setupSourceChoice();

    connect(ui_->startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui_->stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(ui_->monitoringButton, &QPushButton::clicked, this, &MainWindow::onMonitoringClicked);
    connect(ui_->saveNowButton, &QPushButton::clicked, this, &MainWindow::onSaveNowClicked);
    connect(ui_->openFileButton, &QPushButton::clicked, this, &MainWindow::onOpenFileClicked);
    connect(ui_->injectFlashButton, &QPushButton::clicked, this, &MainWindow::onInjectFlashClicked);
    connect(ui_->injectBlipButton, &QPushButton::clicked, this, &MainWindow::onInjectBlipClicked);
    connect(ui_->previewWidget, &PreviewWidget::positionSelected, this, [this](QPoint position) {
        // Feature-15: with Draw mask on, a click places a corner of a shape.
        if (detectionPanel_->drawingMask() && static_cast<int>(pendingShape_.size()) < DetectionMask::kMaxCorners
            && (pendingShape_.empty() || pendingShape_.back() != MaskPoint{position.x(), position.y()}))
        {
            pendingShape_.push_back({position.x(), position.y()});
            std::vector<QPoint> corners;
            for (const MaskPoint& point : pendingShape_) corners.emplace_back(point.x, point.y);
            ui_->previewWidget->setPendingShape(std::move(corners));
        }
        updateDetectionDisplay();
        updateBlipReadout();
        updateControls();
    });
    connect(ui_->previewWidget, &PreviewWidget::shapeClosed, this, [this] { closeMaskShape(); });
    connect(ui_->previewWidget, &PreviewWidget::regionSelected, this, [this](QRect region) {
        // Feature-11: in Sky mode a drag draws the mask, and guiding is off.
        if (detectionPanel_->drawingMask())
        {
            pendingShape_.clear();
            ui_->previewWidget->setPendingShape({});
            if (!detectionPanel_->addMaskRect(region))
                showMessage(QString("The mask is full: %1 shapes. Clear it to start again.").arg(DetectionMask::kMaxShapes), 5000);
            return;
        }
        if (target_ == Target::Sky || !session_->isRunning()) return;
        session_->guider().requestLock({region.x(), region.y(), region.width(), region.height()});
        showMessage("Locking on the selected region…", 3000);
    });
    connect(ui_->blipAmplitudeSpinBox, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateBlipReadout);
    connect(ui_->sourceComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onSourceKindChanged);
    connect(ui_->speedComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onSpeedChanged);
    connect(ui_->loopCheckBox, &QCheckBox::toggled, this, &MainWindow::updateControls);
    connect(ui_->targetFpsSpinBox, &QSpinBox::valueChanged, this, &MainWindow::onTargetFpsChanged);

    bufferReadoutTimer_ = new QTimer(this);
    bufferReadoutTimer_->setInterval(1000);
    connect(bufferReadoutTimer_, &QTimer::timeout, this, &MainWindow::updateStreamReadout);
    prepareTimer_ = new QTimer(this);
    prepareTimer_->setInterval(50);
    connect(prepareTimer_, &QTimer::timeout, this, [this] { takePreparedRecording(std::chrono::milliseconds{0}); });

    // No source is chosen at start, unless a camera family is to connect at
    // startup: choosing it finds its camera, ready for Start.
    if (const int index = ui_->sourceComboBox->findData(startupCamera_); !startupCamera_.isEmpty() && index > 0)
        ui_->sourceComboBox->setCurrentIndex(index);

    updateControls();
    updateStatusLabel();
    updateBlipReadout();
}

void MainWindow::setupSourceChoice()
{
    startupCamera_ = QSettings().value("startup/camera").toString();
    connectAtStartup_ = new QCheckBox("Connect at startup", this);
    connectAtStartup_->setObjectName("connectAtStartup");
    connectAtStartup_->setToolTip("Choose this camera family when SeleneWatch starts and find its camera, ready for Start. "
        "Without it, SeleneWatch starts with no source chosen.");
    ui_->sourceLayout->insertWidget(ui_->sourceLayout->indexOf(ui_->sourceComboBox) + 1, connectAtStartup_);
    connect(connectAtStartup_, &QCheckBox::toggled, this, [this](bool on) {
        const CameraFamilyPanel* camera = selectedCamera();
        if (!camera) return;
        if (on) startupCamera_ = camera->sourceKey();
        else if (startupCamera_ == camera->sourceKey()) startupCamera_.clear();
        QSettings saved;
        if (startupCamera_.isEmpty()) saved.remove("startup/camera");
        else saved.setValue("startup/camera", startupCamera_);
    });

    testingBadge_ = new QLabel("TESTING TOOLS ON", this);
    testingBadge_->setObjectName("testingToolsBadge");
    testingBadge_->setStyleSheet("background:#8a5a00; color:white; font-weight:bold; padding:1px 6px;");
    testingBadge_->setToolTip("The synthetic source and the test flash are available (View → Testing tools). "
        "Injected frames are marked as test data, and session_start lines record testing_tools=\"on\".");
    ui_->sourceLayout->addWidget(testingBadge_);
    testingAction_ = viewMenu_->addAction("Testing tools");
    testingAction_->setObjectName("testingToolsAction");
    testingAction_->setCheckable(true);
    testingAction_->setToolTip("Show the synthetic source and the test flash");
    connect(testingAction_, &QAction::toggled, this, &MainWindow::setTestingTools);
    applyTestingTools(QSettings().value("testing/tools", false).toBool());
}

void MainWindow::setTestingTools(bool on)
{
    if (session_->isRunning())
    {
        const QSignalBlocker blocked(testingAction_);
        testingAction_->setChecked(testingTools_);
        showMessage("Stop the stream to change the testing tools.", 4000);
        return;
    }
    QSettings().setValue("testing/tools", on);
    applyTestingTools(on);
    showMessage(on ? "Testing tools on: the synthetic source and the test flash are available."
                   : "Testing tools off.", 4000);
}

void MainWindow::applyTestingTools(bool on)
{
    testingTools_ = on;
    {
        const QSignalBlocker blocked(testingAction_);
        testingAction_->setChecked(on);
    }
    ui_->blipGroupBox->setVisible(on);
    ui_->injectFlashButton->setVisible(on);
    testingBadge_->setVisible(on);
    session_->setTestingTools(on);
    rebuildSourceList();
    updateControls();
}

void MainWindow::rebuildSourceList()
{
    const QString current = ui_->sourceComboBox->currentData().toString();
    {
        const QSignalBlocker blocked(ui_->sourceComboBox);
        ui_->sourceComboBox->clear();
        ui_->sourceComboBox->addItem("Choose a source…", QString());
        if (testingTools_) ui_->sourceComboBox->addItem("Synthetic (testing)", "synthetic");
        ui_->sourceComboBox->addItem("Recording file", "recording");
        for (const auto& family : cameraFamilies_)
            ui_->sourceComboBox->addItem(family.panel->sourceName(), family.panel->sourceKey());
        ui_->sourceComboBox->setCurrentIndex(std::max(0, ui_->sourceComboBox->findData(current)));
    }
    // The synthetic source went away with the testing tools: nothing is chosen.
    if (ui_->sourceComboBox->currentData().toString() != current) onSourceKindChanged();
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
    if (guidingDock_) disconnect(guidingDock_, nullptr, this, nullptr);
    if (guidingPanel_) disconnect(guidingPanel_, nullptr, this, nullptr);
    if (guidingTimer_) guidingTimer_->stop();
    if (prepareTimer_) prepareTimer_->stop();
    // A recording being read is let finish; its result is dropped.
    if (preparing_.valid()) preparing_.wait();
    saveGuidingSettings();
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
    viewMenu_ = menuBar()->addMenu("View");
    viewMenu_->addAction(detectionDock_->toggleViewAction());
    target_ = QSettings().value("target", 0).toInt() == 1 ? Target::Sky : Target::Moon;
    detectionPanel_->setTarget(target_);
    detectionPanel_->setSettings(savedDetectionSettings(target_));
    session_->setDetectionSettings(detectionPanel_->settings());
    connect(detectionPanel_, &DetectionPanel::settingsChanged, this, [this] {
        session_->setDetectionSettings(detectionPanel_->settings());
        ui_->previewWidget->setMask(maskRects(detectionPanel_->settings().mask));
    });
    connect(detectionPanel_, &DetectionPanel::targetRequested, this, &MainWindow::onTargetRequested);

    // Feature-15: the night schedule and the exposure follower.
    {
        const QSettings saved;
        SkyExtras extras;
        extras.night = saved.value("sky/night/enabled", extras.night).toBool();
        extras.sunLimitDeg = saved.value("sky/night/sunLimit", extras.sunLimitDeg).toDouble();
        extras.follow = saved.value("sky/follow/enabled", extras.follow).toBool();
        extras.minExposureMs = saved.value("sky/follow/minExposureMs", extras.minExposureMs).toDouble();
        extras.maxExposureMs = saved.value("sky/follow/maxExposureMs", extras.maxExposureMs).toDouble();
        extras.maxGain = saved.value("sky/follow/maxGain", extras.maxGain).toDouble();
        extras.gainStep = saved.value("sky/follow/gainStep", extras.gainStep).toDouble();
        detectionPanel_->setSkyExtras(extras);
    }
    connect(detectionPanel_, &DetectionPanel::skyExtrasChanged, this, [this] {
        saveSkyExtras();
        skyExposureState_ = {};
        checkNightSchedule();
    });
    skyTimer_ = new QTimer(this);
    skyTimer_->setInterval(5000);
    connect(skyTimer_, &QTimer::timeout, this, [this] {
        checkSkyExposure(std::chrono::steady_clock::now());
        if (++skyTicks_ % 6 == 0) checkNightSchedule();
    });
    skyTimer_->start();
    connect(detectionPanel_, &DetectionPanel::maskDrawingChanged, this, [this](bool drawing) {
        pendingShape_.clear();
        ui_->previewWidget->setPendingShape({});
        if (drawing) showMessage("Drag a rectangle, or click the corners of a shape and double-click to close it.", 6000);
    });
    connect(detectionPanel_, &DetectionPanel::displayChanged, this, &MainWindow::updateDetectionDisplay);
    connect(detectionDock_, &QDockWidget::visibilityChanged, this, &MainWindow::updateDetectionDisplay);
    updateDetectionDisplay();
    resize(1340, 820);
}

DetectionSettings MainWindow::savedDetectionSettings(Target target)
{
    const QSettings saved;
    const QString key = settingsPrefix(target) + "detection/";
    DetectionSettings settings = defaultDetectionSettings(target);
    settings.sigma = saved.value(key + "sigma", settings.sigma).toDouble();
    settings.minPixels = saved.value(key + "minPixels", settings.minPixels).toInt();
    settings.maxPixels = saved.value(key + "maxPixels", settings.maxPixels).toInt();
    settings.oneFrame = saved.value(key + "oneFrame", settings.oneFrame).toBool();
    settings.maxPatchBoxRatio = saved.value(key + "maxPatchBoxRatio", settings.maxPatchBoxRatio).toDouble();
    settings.referenceTimeConstantSeconds = saved.value(key + "referenceTimeConstantSeconds", settings.referenceTimeConstantSeconds).toDouble();
    settings.minNoiseAdu = saved.value(key + "minNoiseAdu", settings.minNoiseAdu).toDouble();
    settings.darkening = saved.value(key + "darkening", settings.darkening).toBool();
    settings.maxDarkPixels = saved.value(key + "maxDarkPixels", settings.maxDarkPixels).toInt();
    settings.maxDarkBoxRatio = saved.value(key + "maxDarkBoxRatio", settings.maxDarkBoxRatio).toDouble();
    settings.darkeningShare = saved.value(key + "darkeningShare", settings.darkeningShare).toDouble();
    if (target == Target::Sky)
    {
        settings.brighteningShare = saved.value(key + "brighteningShare", settings.brighteningShare).toDouble();
        settings.releaseSeconds = saved.value(key + "releaseSeconds", settings.releaseSeconds).toDouble();
        settings.mask = DetectionMask::fromText(saved.value(key + "mask").toString().toStdString());
    }
    return settings.valid() ? settings : defaultDetectionSettings(target);
}

void MainWindow::saveDetectionSettings()
{
    if (!detectionPanel_) return;
    const auto values = detectionPanel_->settings();
    const QString key = settingsPrefix(detectionPanel_->target()) + "detection/";
    QSettings saved;
    saved.setValue(key + "sigma", values.sigma);
    saved.setValue(key + "minPixels", values.minPixels);
    saved.setValue(key + "maxPixels", values.maxPixels);
    saved.setValue(key + "oneFrame", values.oneFrame);
    saved.setValue(key + "maxPatchBoxRatio", values.maxPatchBoxRatio);
    saved.setValue(key + "referenceTimeConstantSeconds", values.referenceTimeConstantSeconds);
    saved.setValue(key + "minNoiseAdu", values.minNoiseAdu);
    saved.setValue(key + "darkening", values.darkening);
    saved.setValue(key + "maxDarkPixels", values.maxDarkPixels);
    saved.setValue(key + "maxDarkBoxRatio", values.maxDarkBoxRatio);
    saved.setValue(key + "darkeningShare", values.darkeningShare);
    if (detectionPanel_->target() == Target::Sky)
    {
        saved.setValue(key + "brighteningShare", values.brighteningShare);
        saved.setValue(key + "releaseSeconds", values.releaseSeconds);
        saved.setValue(key + "mask", QString::fromStdString(values.mask.text()));
    }
    saved.sync();
}

CaptureSettings MainWindow::savedCaptureSettings(Target target)
{
    const QSettings saved;
    const QString key = settingsPrefix(target) + "capture/";
    CaptureSettings capture = defaultCaptureSettings(target);
    capture.historySeconds = saved.value(key + "history", capture.historySeconds).toDouble();
    capture.postSeconds = saved.value(key + "post", capture.postSeconds).toDouble();
    capture.maxPostSeconds = saved.value(key + "maxPost", capture.maxPostSeconds).toDouble();
    capture.memoryBytes = static_cast<size_t>(std::clamp(saved.value(key + "memoryMiB", 2048).toInt(), 16, 65536)) * 1024 * 1024;
    capture.diskReserveBytes = static_cast<uint64_t>(std::clamp(
        saved.value(key + "diskReserveGiB", static_cast<qulonglong>(capture.diskReserveBytes / kGiB)).toLongLong(), 0LL, 100000LL)) * kGiB;
    return capture.valid() ? capture : defaultCaptureSettings(target);
}

void MainWindow::saveCaptureSettings()
{
    if (!displayPanel_) return;
    const auto capture = displayPanel_->captureSettings();
    const QString key = settingsPrefix(capture.target) + "capture/";
    QSettings saved;
    saved.setValue(key + "history", capture.historySeconds);
    saved.setValue(key + "post", capture.postSeconds);
    saved.setValue(key + "maxPost", capture.maxPostSeconds);
    saved.setValue(key + "memoryMiB", static_cast<int>(capture.memoryBytes / (1024 * 1024)));
    saved.setValue(key + "diskReserveGiB", static_cast<qulonglong>(capture.diskReserveBytes / kGiB));
    saved.sync();
}

void MainWindow::onTargetRequested(Target target)
{
    // The choice is disabled while running; a late request is undone.
    if (session_->isRunning() || target == target_)
    {
        detectionPanel_->setTarget(target_);
        return;
    }
    // Keep the old target's settings before the panels take the new ones.
    saveDetectionSettings();
    saveCaptureSettings();
    target_ = target;
    QSettings().setValue("target", target == Target::Sky ? 1 : 0);
    detectionPanel_->setTarget(target_);
    detectionPanel_->setSettings(savedDetectionSettings(target_));
    displayPanel_->setCaptureSettings(savedCaptureSettings(target_));
    session_->setDetectionSettings(detectionPanel_->settings());
    applyTarget();
    showMessage(target_ == Target::Sky ? "Watching the sky: the sky's settings are loaded. Guiding is for the Moon and is off."
        : "Watching the Moon: the Moon's settings are loaded.", 6000);
}

void MainWindow::saveSkyExtras()
{
    const SkyExtras extras = detectionPanel_->skyExtras();
    QSettings saved;
    saved.setValue("sky/night/enabled", extras.night);
    saved.setValue("sky/night/sunLimit", extras.sunLimitDeg);
    saved.setValue("sky/follow/enabled", extras.follow);
    saved.setValue("sky/follow/minExposureMs", extras.minExposureMs);
    saved.setValue("sky/follow/maxExposureMs", extras.maxExposureMs);
    saved.setValue("sky/follow/maxGain", extras.maxGain);
    saved.setValue("sky/follow/gainStep", extras.gainStep);
}

void MainWindow::checkNightSchedule()
{
    const SkyExtras extras = detectionPanel_->skyExtras();
    if (target_ != Target::Sky || !extras.night)
    {
        detectionPanel_->showNight({});
        nightOwns_ = NightOwns::Nothing;
        nightStartedTonight_ = false;
        return;
    }
    const ObservingSite site = savedSite();
    if (!site.hasPosition())
    {
        detectionPanel_->showNight("Needs the site's position: Session → Site and instrument.");
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const double altitude = sunAltitudeDegrees(*site.latitudeDeg, *site.longitudeDeg, now);
    const bool watching = session_->isRunning() && session_->mode() == Session::Mode::Monitoring;
    // What the schedule started may have ended without it: a source that
    // stopped, or Monitoring turned off by the observer.
    if (!session_->isRunning()
        || (nightOwns_ == NightOwns::Monitoring && !watching && !session_->modeChangePending()))
        nightOwns_ = NightOwns::Nothing;
    switch (nightAction(altitude, extras.sunLimitDeg, watching, nightStartedTonight_, nightOwns_ != NightOwns::Nothing))
    {
    case NightAction::Start:
        if (session_->modeChangePending()) break;
        if (!session_->isRunning() && ui_->sourceComboBox->currentData().toString().isEmpty())
        {
            // Nothing to watch with yet; tried again at the next check.
            showMessage("Dusk, but no source is chosen: choose the camera to watch with.", 6000);
            break;
        }
        nightStartedTonight_ = true;
        showMessage(QString("Dusk: the Sun is at %1°. Watching the sky.").arg(altitude, 0, 'f', 1), 6000);
        if (!session_->isRunning())
        {
            nightOwns_ = NightOwns::Stream;
            startStream(Session::Mode::Monitoring);
        }
        else
        {
            nightOwns_ = NightOwns::Monitoring;
            onMonitoringClicked();
        }
        break;
    case NightAction::Stop:
        showMessage(QString("Dawn: the Sun is at %1°. Watching stopped.").arg(altitude, 0, 'f', 1), 6000);
        if (nightOwns_ == NightOwns::Stream) finishStream();
        else if (!session_->modeChangePending() && session_->requestMonitoring(false))
        {
            updateControls();
            updateStatusLabel();
        }
        nightOwns_ = NightOwns::Nothing;
        break;
    case NightAction::None:
        break;
    }
    if (altitude >= extras.sunLimitDeg) nightStartedTonight_ = false;
    const auto next = nextSunCrossing(*site.latitudeDeg, *site.longitudeDeg, extras.sunLimitDeg, now);
    QString text = QString("Sun at %1°.").arg(altitude, 0, 'f', 1);
    if (next)
        text += QString(" %1 at %2 UTC.").arg(altitude < extras.sunLimitDeg ? "Dawn stop" : "Dusk start",
            utcDateTime(*next).toString("yyyy-MM-dd HH:mm"));
    else text += " It does not cross the limit in the next two days.";
    detectionPanel_->showNight(text);
}

void MainWindow::checkSkyExposure(std::chrono::steady_clock::time_point now)
{
    const SkyExtras extras = detectionPanel_->skyExtras();
    if (target_ != Target::Sky || !extras.follow)
    {
        detectionPanel_->showFollow({});
        return;
    }
    CameraSettingsEditor* controls = activeCamera() ? activeCamera()->controls() : nullptr;
    if (!controls || !session_->isRunning() || !latestFrame_ || session_->modeChangePending())
    {
        detectionPanel_->showFollow("Follows the sky while a camera runs.");
        return;
    }
    // Both camera families number Exposure 0 and Gain 1, in microseconds and their own units.
    constexpr int kExposure = 0, kGain = 1;
    const auto exposure = controls->controlValue(kExposure);
    if (!exposure) return;
    const auto gain = controls->controlValue(kGain);
    const auto gainRange = controls->controlRange(kGain);
    SkyExposureRule rule;
    rule.minExposureUs = extras.minExposureMs * 1000;
    rule.maxExposureUs = std::max(rule.minExposureUs, extras.maxExposureMs * 1000);
    rule.minGain = gainRange ? gainRange->first : 0;
    rule.maxGain = gainRange ? std::min(gainRange->second, std::max(rule.minGain, extras.maxGain)) : 0;
    rule.gainStep = gain ? extras.gainStep : 0;
    const double level = skyLevel(*latestFrame_, detectionPanel_->settings().mask);
    detectionPanel_->showFollow(QString("Sky at %1% of full scale. Exposure %2 ms, gain %3.")
        .arg(100 * level, 0, 'f', 1).arg(*exposure / 1000, 0, 'g', 4).arg(gain.value_or(0), 0, 'f', 0));
    const auto change = decideSkyExposure(level, *exposure, gain.value_or(0), now, skyExposureState_, rule);
    if (!change) return;
    controls->setControlValue(kExposure, change->exposureUs);
    if (gain) controls->setControlValue(kGain, change->gain);
    session_->logRecord(QString("%1 exposure_change exposure_ms=%2 to %3 gain=%4 to %5 reason=\"%6\"\n")
        .arg(utcText(std::chrono::system_clock::now())).arg(*exposure / 1000, 0, 'g', 6).arg(change->exposureUs / 1000, 0, 'g', 6)
        .arg(gain.value_or(0), 0, 'g', 6).arg(change->gain, 0, 'g', 6).arg(QString::fromStdString(change->reason)));
    showMessage(QString("The exposure follows the sky: %1 ms, gain %2 (%3).")
        .arg(change->exposureUs / 1000, 0, 'g', 4).arg(change->gain, 0, 'f', 0).arg(QString::fromStdString(change->reason)), 8000);
    if (session_->mode() == Session::Mode::Monitoring)
    {
        // A camera's settings are fixed while Monitoring: the clip in hand is
        // written and Monitoring starts again, with the new settings read back.
        bufferReadoutTimer_->stop();
        session_->stop("the exposure follows the sky");
        startStream(Session::Mode::Monitoring);
    }
    else onApplyLiveCamera();
}

void MainWindow::closeMaskShape()
{
    if (!detectionPanel_->drawingMask()) return;
    MaskShape shape = pendingShape_;
    pendingShape_.clear();
    ui_->previewWidget->setPendingShape({});
    if (shape.size() < 3)
    {
        showMessage("A mask shape needs at least three corners.", 4000);
        return;
    }
    if (!detectionPanel_->addMaskShape(shape))
        showMessage(QString("The mask is full: %1 shapes. Clear it to start again.").arg(DetectionMask::kMaxShapes), 5000);
}

void MainWindow::applyTarget()
{
    const bool sky = target_ == Target::Sky;
    ui_->previewWidget->setMask(maskRects(detectionPanel_->settings().mask));
    if (guidingPanel_)
    {
        if (sky) session_->guider().requestUnlock();
        guidingPanel_->setEnabled(!sky);
        guidingDock_->setWindowTitle(sky ? "Guiding (Moon only)" : "Guiding");
    }
}

void MainWindow::setupDisplayPanel()
{
    displayPanel_ = new DisplayPanel(this);
    displayDock_ = new QDockWidget("Display and capture", this);
    displayDock_->setObjectName("displayDock"); displayDock_->setWidget(displayPanel_);
    addDockWidget(Qt::RightDockWidgetArea, displayDock_);
    tabifyDockWidget(detectionDock_, displayDock_);
    viewMenu_->addAction(displayDock_->toggleViewAction());
    QSettings saved;
    DisplaySettings display;
    const int mode = saved.value("display/mode", 0).toInt();
    display.mode = mode >= 0 && mode <= 2 ? static_cast<DisplayMode>(mode) : DisplayMode::Linear;
    display.black = saved.value("display/black", 0).toDouble(); display.white = saved.value("display/white", 1).toDouble();
    display.invert = saved.value("display/invert", false).toBool(); display.debayer = saved.value("display/debayer", true).toBool();
    displayPanel_->setDisplaySettings(display.valid() ? display : DisplaySettings{});
    displayPanel_->setCaptureSettings(savedCaptureSettings(target_));
    connect(displayPanel_, &DisplayPanel::displayChanged, this, &MainWindow::renderPreview);
    displayDock_->raise();
    updateBufferReadout();
}

void MainWindow::saveDisplaySettings()
{
    if (!displayPanel_) return;
    const auto display = displayPanel_->displaySettings();
    QSettings saved;
    saved.setValue("display/mode", static_cast<int>(display.mode));
    saved.setValue("display/black", display.black); saved.setValue("display/white", display.white);
    saved.setValue("display/invert", display.invert); saved.setValue("display/debayer", display.debayer);
    saved.setValue("histogram/visible", !histogramDock_->isHidden());
    saved.setValue("histogram/fitRange", histogramPanel_->fitRange());
    saved.setValue("histogram/logCounts", histogramPanel_->logCounts());
    saved.sync();
    saveCaptureSettings();
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
    viewMenu_->addAction(histogramDock_->toggleViewAction());

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

void MainWindow::setupGuidingPanel()
{
    guidingPanel_ = new GuidingPanel(this);
    guidingDock_ = new QDockWidget("Guiding", this);
    guidingDock_->setObjectName("guidingDock");
    guidingDock_->setWidget(guidingPanel_);
    addDockWidget(Qt::RightDockWidgetArea, guidingDock_);
    tabifyDockWidget(displayDock_, guidingDock_);
    displayDock_->raise();
    viewMenu_->addAction(guidingDock_->toggleViewAction());

    simulatedPointing_ = std::make_shared<SimulatedPointing>(SimulatedPointing::Settings{});
    const QSettings saved;
    ascomDriver_ = saved.value("guiding/ascomDriver").toString();
    guidingPanel_->setAscomAvailable(ascomAvailable());
    guidingPanel_->setAscomDriver(ascomDriver_);
    guidingPanel_->setMountKind(saved.value("guiding/mountKind", 0).toInt() == 1 && ascomAvailable()
            ? GuidingPanel::MountKind::Ascom : GuidingPanel::MountKind::Simulated);
    guidingPanel_->setTolerancePercent(std::clamp(saved.value("guiding/tolerancePercent", 3.0).toDouble(), 0.5, 20.0));

    Guider& guider = session_->guider();
    connect(guidingPanel_, &GuidingPanel::lockAtSelectionRequested, this, &MainWindow::onLockAtSelection);
    connect(guidingPanel_, &GuidingPanel::unlockRequested, this, [&guider] { guider.requestUnlock(); });
    connect(guidingPanel_, &GuidingPanel::chooseDriverRequested, this, &MainWindow::onChooseMountDriver);
    connect(guidingPanel_, &GuidingPanel::connectRequested, this, &MainWindow::onConnectMount);
    connect(guidingPanel_, &GuidingPanel::disconnectRequested, this, [&guider] { guider.requestDisconnect(); });
    connect(guidingPanel_, &GuidingPanel::calibrateRequested, this, [&guider] { guider.requestCalibration(); });
    connect(guidingPanel_, &GuidingPanel::guidingRequested, this, [&guider](bool on) { guider.requestGuiding(on); });
    connect(guidingPanel_, &GuidingPanel::toleranceChanged, this, [this] { sendTolerance(); saveGuidingSettings(); });
    connect(guidingPanel_, &GuidingPanel::mountKindChanged, this, &MainWindow::saveGuidingSettings);

    // A hung driver call publishes nothing, so the panel also asks once a second.
    guidingTimer_ = new QTimer(this);
    guidingTimer_->setInterval(1000);
    connect(guidingTimer_, &QTimer::timeout, this, &MainWindow::onLockChanged);
    guidingTimer_->start();
}

void MainWindow::saveGuidingSettings()
{
    if (!guidingPanel_) return;
    QSettings saved;
    saved.setValue("guiding/ascomDriver", ascomDriver_);
    saved.setValue("guiding/mountKind", guidingPanel_->mountKind() == GuidingPanel::MountKind::Ascom ? 1 : 0);
    saved.setValue("guiding/tolerancePercent", guidingPanel_->tolerancePercent());
}

void MainWindow::setupSessionMenu()
{
    auto* menu = menuBar()->addMenu("Session");
    auto* site = menu->addAction("Site and instrument…");
    site->setObjectName("siteAction");
    connect(site, &QAction::triggered, this, &MainWindow::onSiteAndInstrument);
    auto* cameras = menu->addAction("Camera support…");
    cameras->setObjectName("cameraSupportAction");
    cameras->setToolTip("Which camera families this version supports, their SDK libraries and versions, and the connected cameras");
    connect(cameras, &QAction::triggered, this, &MainWindow::onCameraSupport);
    auto* schedule = menu->addAction("Schedule a recording…");
    schedule->setObjectName("scheduleAction");
    connect(schedule, &QAction::triggered, this, &MainWindow::onScheduleRecording);
    auto* folder = menu->addAction("Recordings folder…");
    folder->setObjectName("recordingFolderAction");
    connect(folder, &QAction::triggered, this, &MainWindow::onRecordingFolder);
    menu->addSeparator();
    auto* events = menu->addAction("Open the events folder");
    events->setObjectName("openEventsAction");
    events->setToolTip("Clips, candidates.txt and guiding.txt; beside SeleneWatch.exe");
    connect(events, &QAction::triggered, this, [this] {
        QDir().mkpath(session_->outputRoot());
        QDesktopServices::openUrl(QUrl::fromLocalFile(session_->outputRoot()));
    });

    // Record, beside Save now: every frame until pressed again (Feature-14).
    recordButton_ = new QPushButton("Record", this);
    recordButton_->setObjectName("recordButton");
    recordButton_->setToolTip("While Monitoring, writes every frame to one SER file in the recordings folder "
        "(Session → Recordings folder; events/recordings unless chosen) until pressed again.");
    ui_->controlsLayout->insertWidget(ui_->controlsLayout->indexOf(ui_->saveNowButton) + 1, recordButton_);
    connect(recordButton_, &QPushButton::clicked, this, &MainWindow::onRecordClicked);

    session_->setSite(savedSite());
    const QSettings saved;
    session_->setRecordingFolder(saved.value("recording/folder").toString());
    session_->setClockCheck(true, saved.value("time/ntp", false).toBool()
        ? saved.value("time/ntpServer", "pool.ntp.org").toString().toStdString() : std::string());
}

void MainWindow::setupHelpMenu()
{
    auto* menu = menuBar()->addMenu("Help");
    auto* about = menu->addAction("About SeleneWatch");
    about->setObjectName("aboutAction");
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(this, "About SeleneWatch", aboutText());
    });
    auto* acknowledgements = menu->addAction("Acknowledgements");
    acknowledgements->setObjectName("acknowledgementsAction");
    connect(acknowledgements, &QAction::triggered, this, &MainWindow::showAcknowledgements);
    auto* aboutQt = menu->addAction("About Qt");
    connect(aboutQt, &QAction::triggered, this, [this] { QMessageBox::aboutQt(this); });
}

QString MainWindow::acknowledgementsMarkdown()
{
    QFile file(":/ACKNOWLEDGEMENTS.md");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void MainWindow::showAcknowledgements()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Acknowledgements");
    dialog.resize(760, 640);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QTextBrowser(&dialog);
    text->setObjectName("acknowledgementsText");
    text->setOpenExternalLinks(true);
    text->setMarkdown(acknowledgementsMarkdown());
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QString MainWindow::aboutText()
{
    return QString("<p><b>SeleneWatch %1</b><br>Source revision %2</p>"
        "<p>Watches the Moon for impact flashes and the sky for meteors, aircraft, satellites and lightning, "
        "and keeps the evidence of each candidate.</p>"
        "<p>Copyright &copy; 2026, Petros Pissias. BSD 2-Clause License.<br>"
        "<a href=\"https://github.com/ppissias/SeleneWatch\">github.com/ppissias/SeleneWatch</a></p>"
        "<p>Built with Qt %3 under the GNU LGPL version 3 (Help &rarr; About Qt). The license texts of Qt and of "
        "the other components are in the <i>licenses</i> folder beside SeleneWatch.exe, and THIRD_PARTY_LICENSES.md "
        "says where their source code is. Help &rarr; Acknowledgements credits the methods and tools it builds on.</p>")
        .arg(SELENEWATCH_RELEASE, SELENEWATCH_REVISION, qVersion());
}

ObservingSite MainWindow::savedSite()
{
    const QSettings saved;
    ObservingSite site;
    site.observer = saved.value("site/observer").toString().toStdString();
    site.site = saved.value("site/name").toString().toStdString();
    if (saved.contains("site/latitude") && saved.contains("site/longitude"))
    {
        site.latitudeDeg = saved.value("site/latitude").toDouble();
        site.longitudeDeg = saved.value("site/longitude").toDouble();
        site.heightM = saved.value("site/height", 0).toDouble();
    }
    site.telescope = saved.value("site/telescope").toString().toStdString();
    site.apertureMm = saved.value("site/aperture", 0).toDouble();
    site.focalLengthMm = saved.value("site/focalLength", 0).toDouble();
    site.pixelSizeUm = saved.value("site/pixelSize", 0).toDouble();
    return site;
}

void MainWindow::saveSite(const ObservingSite& site, bool ntp, const QString& server)
{
    QSettings saved;
    saved.setValue("site/observer", QString::fromStdString(site.observer));
    saved.setValue("site/name", QString::fromStdString(site.site));
    if (site.hasPosition())
    {
        saved.setValue("site/latitude", *site.latitudeDeg);
        saved.setValue("site/longitude", *site.longitudeDeg);
        saved.setValue("site/height", site.heightM.value_or(0));
    }
    else
    {
        saved.remove("site/latitude");
        saved.remove("site/longitude");
        saved.remove("site/height");
    }
    saved.setValue("site/telescope", QString::fromStdString(site.telescope));
    saved.setValue("site/aperture", site.apertureMm);
    saved.setValue("site/focalLength", site.focalLengthMm);
    saved.setValue("site/pixelSize", site.pixelSizeUm);
    saved.setValue("time/ntp", ntp);
    saved.setValue("time/ntpServer", server);
    saved.sync();
}

void MainWindow::onSiteAndInstrument()
{
    SiteDialog dialog(this);
    dialog.setSite(savedSite());
    const QSettings saved;
    dialog.setClockCheck(saved.value("time/ntp", false).toBool(), saved.value("time/ntpServer", "pool.ntp.org").toString());
    if (dialog.exec() != QDialog::Accepted) return;
    applySite(dialog.site(), dialog.ntpCheck(), dialog.ntpServer());
}

void MainWindow::applySite(const ObservingSite& site, bool ntp, const QString& server)
{
    saveSite(site, ntp, server);
    session_->setSite(site);
    session_->setClockCheck(true, ntp && !server.isEmpty() ? server.toStdString() : std::string());
    showMessage("Site and instrument saved; they are recorded from the next clip on.", 4000);
}

void MainWindow::onCameraSupport()
{
    std::vector<CameraFamilyPanel*> panels;
    for (const auto& family : cameraFamilies_) panels.push_back(family.panel);
    CameraSupportDialog dialog(panels, session_->isRunning(), this);
    // Checked once the window shows, so it does not appear late.
    QTimer::singleShot(0, &dialog, &CameraSupportDialog::checkAll);
    dialog.exec();
    updateControls();
}

void MainWindow::onScheduleRecording()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Schedule a recording");
    auto* form = new QFormLayout(&dialog);
    auto* start = new QDateTimeEdit(&dialog);
    start->setObjectName("scheduleStart");
    start->setTimeZone(QTimeZone::UTC);
    start->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'");
    auto* seconds = new QDoubleSpinBox(&dialog);
    seconds->setObjectName("scheduleSeconds");
    seconds->setRange(1, 36000);
    seconds->setDecimals(1);
    seconds->setSuffix(" s");
    const auto current = session_->schedule();
    const auto toQt = [](std::chrono::system_clock::time_point time) {
        return QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count(),
            QTimeZone::UTC);
    };
    start->setDateTime(toQt(current ? current->start : std::chrono::system_clock::now() + std::chrono::minutes{1}));
    seconds->setValue(current ? std::chrono::duration<double>(current->end - current->start).count() : 30);
    form->addRow("From", start);
    form->addRow("For", seconds);
    auto* note = new QLabel("It runs while Monitoring, by the frames' times. For a predicted transit or "
        "occultation, start a little before the prediction.", &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* clear = buttons->addButton("Clear the schedule", QDialogButtonBox::ResetRole);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(clear, &QPushButton::clicked, &dialog, [this, &dialog] {
        session_->cancelSchedule();
        showMessage("The scheduled recording is cleared.", 4000);
        dialog.reject();
    });
    if (dialog.exec() == QDialog::Accepted)
    {
        const auto from = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{start->dateTime().toMSecsSinceEpoch()};
        session_->scheduleRecording({from, from + std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(seconds->value()))});
        showMessage("Recording scheduled. It runs while Monitoring.", 4000);
    }
    updateStatusLabel();
}

void MainWindow::onRecordingFolder()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Recordings folder");
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel("Each continuous recording gets a folder of its own here, named by its UTC start. "
        "Clips and candidates.txt stay in the events folder, and candidates.txt names where each recording went. "
        "The disk reserve is checked on this folder's drive.", &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* row = new QHBoxLayout;
    auto* path = new QLineEdit(QDir::toNativeSeparators(session_->recordingFolder()), &dialog);
    path->setObjectName("recordingFolderPath");
    path->setMinimumWidth(360);
    auto* choose = new QPushButton("Choose…", &dialog);
    auto* standard = new QPushButton("Default", &dialog);
    standard->setToolTip("events/recordings beside the clips");
    row->addWidget(path, 1);
    row->addWidget(choose);
    row->addWidget(standard);
    layout->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(choose, &QPushButton::clicked, &dialog, [&dialog, path] {
        const QString chosen = QFileDialog::getExistingDirectory(&dialog, "Recordings folder", path->text());
        if (!chosen.isEmpty()) path->setText(QDir::toNativeSeparators(chosen));
    });
    connect(standard, &QPushButton::clicked, &dialog, [path] { path->clear(); });
    if (dialog.exec() != QDialog::Accepted) return;
    QString error;
    if (!applyRecordingFolder(QDir::fromNativeSeparators(path->text().trimmed()), error))
        QMessageBox::warning(this, "Recordings folder", error);
}

bool MainWindow::applyRecordingFolder(const QString& folder, QString& error)
{
    // Empty, or the default itself, is saved as empty: the default.
    QString chosen = folder.isEmpty() ? QString() : QDir::cleanPath(QDir(folder).absolutePath());
    if (chosen.compare(QDir::cleanPath(session_->defaultRecordingFolder()), Qt::CaseInsensitive) == 0) chosen.clear();
    if (!chosen.isEmpty())
    {
        // A folder that cannot be written is refused now, not at the recording's start.
        QTemporaryFile probe(QDir(chosen).filePath("selenewatch-write-check-XXXXXX"));
        if (!QDir().mkpath(chosen) || !probe.open())
        {
            error = QString("SeleneWatch cannot write in %1. The recordings folder is unchanged.").arg(QDir::toNativeSeparators(chosen));
            return false;
        }
    }
    QSettings().setValue("recording/folder", chosen);
    session_->setRecordingFolder(chosen);
    showMessage(QString("Recordings go to %1 from the next recording on.").arg(QDir::toNativeSeparators(session_->recordingFolder())), 5000);
    return true;
}

void MainWindow::onRecordClicked()
{
    const bool recording = session_->recordingStatus().active;
    if (!session_->requestRecording(!recording)) showMessage("Recording needs Monitoring.", 4000);
    updateControls();
}

void MainWindow::onRecordingChanged()
{
    const auto status = session_->recordingStatus();
    if (!status.active && !status.endReason.empty())
        showMessage(QString("Recording %1 ended: %2. %3 frames.")
            .arg(status.name, QString::fromStdString(status.endReason)).arg(status.frames));
    else if (status.active)
        showMessage(QString("Recording every frame to %1.").arg(QDir::toNativeSeparators(session_->recordingFolder())));
    updateControls();
    updateStatusLabel();
}

QString MainWindow::recordingReadout() const
{
    const auto status = session_->recordingStatus();
    if (status.active)
        return QString("● RECORDING %1 s, %2 frames, %3 GB written at %4 MB/s.")
            .arg(status.streamSeconds, 0, 'f', 0).arg(status.frames)
            .arg(static_cast<double>(status.bytes) / 1e9, 0, 'f', 2).arg(status.megabytesPerSecond, 0, 'f', 0);
    if (const auto schedule = session_->schedule())
        return QString("Recording scheduled from %1 UTC for %2 s; it runs while Monitoring.")
            .arg(utcText(schedule->start).replace('T', ' ').chopped(1))
            .arg(std::chrono::duration<double>(schedule->end - schedule->start).count(), 0, 'f', 1);
    return {};
}

// The tolerance is chosen as a share of the field and sent in frame pixels.
void MainWindow::sendTolerance()
{
    if (frameShortSide_ <= 0) return;
    session_->guider().setTolerance(guidingPanel_->tolerancePercent() / 100.0 * frameShortSide_);
}

void MainWindow::onChooseMountDriver()
{
    std::string error;
    const auto chosen = chooseAscomTelescope(ascomDriver_.toStdString(), error);
    if (!chosen)
    {
        showMessage(QString("ASCOM chooser: %1").arg(QString::fromStdString(error)), 8000);
        return;
    }
    if (chosen->empty()) return;
    ascomDriver_ = QString::fromStdString(*chosen);
    guidingPanel_->setAscomDriver(ascomDriver_);
    saveGuidingSettings();
}

void MainWindow::onConnectMount()
{
    Guider& guider = session_->guider();
    if (guidingPanel_->mountKind() == GuidingPanel::MountKind::Simulated)
    {
        guider.requestConnect([pointing = simulatedPointing_] { return std::make_unique<SimulatedMount>(pointing); });
        showMessage("Simulated mount: it moves the synthetic Moon. Use the Synthetic source.", 6000);
        return;
    }
    if (ascomDriver_.isEmpty())
    {
        showMessage("Choose an ASCOM telescope driver first.", 5000);
        return;
    }
    guider.requestConnect([progId = ascomDriver_.toStdString()] { return std::make_unique<AscomMount>(progId); });
    showMessage(QString("Connecting to %1…").arg(ascomDriver_), 5000);
}

void MainWindow::onLockAtSelection()
{
    const auto position = ui_->previewWidget->selectedPosition();
    if (!position || !session_->isRunning()) return;
    constexpr int side = GuidingPanel::kDefaultLockSide;
    session_->guider().requestLock({position->x() - side / 2, position->y() - side / 2, side, side});
    showMessage("Locking on the selected pixel…", 3000);
}

// The guider's status reaches the panel, the preview boxes, and the status bar.
void MainWindow::onLockChanged()
{
    const GuiderStatus all = session_->guider().status();
    guidingPanel_->setStatus(all);
    const LockStatus& status = all.lock;
    const QString guiding = all.guide.mode == GuideMode::Idle ? QString() : QString(" | %1").arg(guideModeName(all.guide.mode));
    if (status.state == LockState::Off)
    {
        ui_->previewWidget->setLockBoxes({}, {}, false);
        lockLabel_->setText("Lock: off" + guiding);
        return;
    }
    // The found box appears with the first search; green while the lock holds.
    const QRect locked(status.region.x, status.region.y, status.region.width, status.region.height);
    std::optional<QRectF> found;
    if (status.lastMatch) found = QRectF(locked).translated(status.foundDx, status.foundDy);
    const bool valid = status.lastMatch && status.lastMatch->valid && status.state == LockState::Locked;
    ui_->previewWidget->setLockBoxes(locked, found, valid);
    if (status.state != LockState::Locked)
        lockLabel_->setText(QString("Lock: %1").arg(lockStateName(status.state)) + guiding);
    else if (status.estimateValid)
        lockLabel_->setText(QString("Lock: dx %1, dy %2 px").arg(status.estimateDx, 0, 'f', 1).arg(status.estimateDy, 0, 'f', 1) + guiding);
    else
        lockLabel_->setText("Lock: measuring" + guiding);
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
    catch (const MemoryLimitError&) { showMessage("Preview skipped: RAM ceiling reached"); }
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
    // Messages carry folder paths and error reasons, so they get the whole
    // width; a path that still does not fit keeps both of its ends.
    messageLabel_ = new ElidedLabel({}, Qt::ElideMiddle);
    messageLabel_->setObjectName("statusMessage");
    messageLabel_->setMinimumHeight(messageLabel_->fontMetrics().height());  // no jump when a message appears
    messageTimer_ = new QTimer(this);
    messageTimer_->setSingleShot(true);
    connect(messageTimer_, &QTimer::timeout, this, [this] { messageLabel_->clear(); });

    statusLabel_ = new ElidedLabel("Status: Idle");
    fpsLabel_ = new ElidedLabel("FPS: --");
    framesLabel_ = new ElidedLabel("Frames: 0");
    framesLabel_->setObjectName("framesReadout");
    candidatesLabel_ = new ElidedLabel;
    bufferLabel_ = new ElidedLabel;
    frameLabel_ = new ElidedLabel("No frame yet");
    lockLabel_ = new ElidedLabel("Lock: off");
    lockLabel_->setObjectName("lockReadout");

    // Room for the usual values, so the readouts do not shift as numbers
    // change. A longer text widens its readout while the window has room.
    statusLabel_->setMinimumWidth(150);
    fpsLabel_->setMinimumWidth(80);
    framesLabel_->setMinimumWidth(100);
    candidatesLabel_->setMinimumWidth(110);
    bufferLabel_->setMinimumWidth(210);
    frameLabel_->setMinimumWidth(160);

    auto* readouts = new QHBoxLayout;
    readouts->setContentsMargins(0, 0, 0, 0);
    readouts->setSpacing(8);
    const QList<QLabel*> labels{statusLabel_, fpsLabel_, framesLabel_, candidatesLabel_, bufferLabel_, frameLabel_, lockLabel_};
    for (QLabel* label : labels)
    {
        if (label != labels.front())
        {
            auto* separator = new QFrame;
            separator->setFrameShape(QFrame::VLine);
            separator->setFrameShadow(QFrame::Sunken);
            readouts->addWidget(separator);
        }
        readouts->addWidget(label);
    }
    readouts->addStretch(1);

    auto* area = new QWidget(this);
    auto* rows = new QVBoxLayout(area);
    rows->setContentsMargins(4, 0, 0, 0);
    rows->setSpacing(1);
    rows->addWidget(messageLabel_);
    rows->addLayout(readouts);
    statusBar()->setStyleSheet("QStatusBar::item { border: none; }");
    statusBar()->addPermanentWidget(area, 1);

    updateBufferReadout();
    updateCandidateReadout();
}

void MainWindow::showMessage(const QString& text, int timeoutMs)
{
    messageLabel_->setText(text);
    if (timeoutMs > 0)
        messageTimer_->start(timeoutMs);
    else
        messageTimer_->stop();
}

bool MainWindow::recordingSelected() const
{
    return ui_->sourceComboBox->currentData().toString()=="recording";
}
void MainWindow::addCameraFamily(CameraFamilyPanel* panel,const QString& dockTitle,const QString& dockName)
{
    auto* dock=new QDockWidget(dockTitle,this);
    dock->setObjectName(dockName);
    dock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea,dock);
    dock->hide();
    connect(panel,&CameraFamilyPanel::applyRequested,this,&MainWindow::onApplyCamera);
    connect(panel,&CameraFamilyPanel::liveControlsRequested,this,&MainWindow::onApplyLiveCamera);
    connect(panel,&CameraFamilyPanel::availabilityChanged,this,&MainWindow::updateControls);
    cameraFamilies_.push_back({panel,dock});
}
CameraFamilyPanel* MainWindow::selectedCamera() const
{
    const QString key=ui_->sourceComboBox->currentData().toString();
    for (const auto& family:cameraFamilies_)
        if (family.panel->sourceKey()==key) return family.panel;
    return nullptr;
}
CameraFamilyPanel* MainWindow::activeCamera() const
{
    for (const auto& family:cameraFamilies_)
        if (family.panel->madeSource()) return family.panel;
    return nullptr;
}

// Which controls can be used depends on the source and on whether it runs.
void MainWindow::updateControls()
{
    const bool running = session_->isRunning();
    const bool recording = recordingSelected();
    const bool monitoring = session_->mode()==Session::Mode::Monitoring;
    const bool pending = session_->modeChangePending();

    CameraFamilyPanel* selected = selectedCamera();
    const bool chosen = !ui_->sourceComboBox->currentData().toString().isEmpty();
    ui_->startButton->setEnabled(!running && chosen && (!selected || selected->hasCamera()));
    if (connectAtStartup_)
    {
        connectAtStartup_->setEnabled(!running && selected);
        const QSignalBlocker blocked(connectAtStartup_);
        connectAtStartup_->setChecked(selected && startupCamera_ == selected->sourceKey());
    }
    if (testingAction_) testingAction_->setEnabled(!running);
    ui_->stopButton->setEnabled(running);
    ui_->monitoringButton->setEnabled(running && !session_->sourceFinished() && !pending);
    ui_->monitoringButton->setText(monitoring?"Stop monitoring":"Start monitoring");
    ui_->saveNowButton->setEnabled(running && monitoring && !pending && !session_->sourceFinished() && !session_->manualSavePending());
    if (recordButton_)
    {
        const bool writing = session_->recordingStatus().active;
        recordButton_->setText(writing ? "Stop recording" : "Record");
        recordButton_->setEnabled(writing || (running && monitoring && !pending && !session_->sourceFinished()));
    }
    ui_->injectFlashButton->setEnabled(running && synthetic_ != nullptr);

    ui_->sourceComboBox->setEnabled(!running);
    if (detectionPanel_) detectionPanel_->setTargetEditable(!running);
    ui_->openFileButton->setEnabled(!running && recording);
    ui_->loopCheckBox->setEnabled(!running && recording);
    ui_->loopSecondsSpinBox->setEnabled(!running && recording && ui_->loopCheckBox->isChecked());

    // The speed may change while a recording plays.
    ui_->speedComboBox->setEnabled(recording);
    ui_->targetFpsSpinBox->setEnabled(!selected);
    if (displayPanel_) displayPanel_->setRunning(running, recording || selected);
    for (const auto& family : cameraFamilies_)
    {
        const bool thisFamily = family.panel == selected;
        family.dock->setEnabled(thisFamily);
        family.panel->setRunning(running && thisFamily, running && (monitoring || pending));
    }
    if (guidingPanel_) guidingPanel_->setAvailable(running, ui_->previewWidget->selectedPosition().has_value());
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
    if (guidingPanel_) guidingPanel_->setLogFailed(writer.guidingWriteFailures > 0);
}

void MainWindow::updateStreamReadout()
{
    updateBufferReadout();
    updateBlipReadout();
    if (session_->recordingStatus().active || session_->schedule()) updateStatusLabel();

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
    if (activeCamera())
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
    if (const QString recording = recordingReadout(); !recording.isEmpty())
        modeLabel_->setText(modeLabel_->text() + "\n" + recording);
}

void MainWindow::updateFrameLabel(const Frame& frame)
{
    QString text = QString("%1 x %2 %3").arg(frame.width).arg(frame.height).arg(pixelFormatName(frame.format));
    if (frame.format == PixelFormat::Mono16)
    {
        if (activeCamera()) text += ", delivered 16-bit words";
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
    for (const auto& family : cameraFamilies_) family.panel->forgetSource();
    const int fps = ui_->targetFpsSpinBox->value();

    if (CameraFamilyPanel* camera = selectedCamera())
    {
        if (!camera->hasCamera())
        {
            showMessage(QString("Connect a %1 and refresh its list").arg(camera->sourceName()));
            return {};
        }
        return camera->makeSource();
    }
    const QString kind = ui_->sourceComboBox->currentData().toString();
    if (kind.isEmpty())
    {
        showMessage("Choose a source first.");
        return {};
    }
    if (kind == "synthetic")
    {
        auto source = std::make_unique<SyntheticFrameSource>(kSyntheticWidth, kSyntheticHeight, fps);
        // The simulated mount moves the painted Moon; it stays put while that mount is not connected.
        source->setSceneOffset([pointing = simulatedPointing_] {
            const auto offset = pointing->imageOffset(SimulatedPointing::Clock::now());
            return std::pair{offset.x, offset.y};
        });
        const int layout = displayPanel_->syntheticFormat();
        if (layout) source->setPixelLayout(PixelFormat::Mono16, layout == 3 ? 16 : 12,
            layout == 2 || layout == 4 ? BitAlignment::High : BitAlignment::Low,
            layout == 4 ? BayerPattern::RGGB : BayerPattern::None);
        synthetic_ = source.get();
        return source;
    }

    if (recordingPath_.isEmpty())
    {
        showMessage("Choose a recording file first");
        return nullptr;
    }

    // The reader read off this thread when the file was chosen (startStream
    // makes sure it is this file's), shared from run to run.
    auto source = std::make_unique<RecordingSource>(std::filesystem::path(recordingPath_.toStdWString()), preparedReader_);
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
    // A recording starts from its reader. Its index is read off this thread;
    // a short read is waited for, a long one starts the stream when done.
    if (recordingSelected() && !recordingPath_.isEmpty())
    {
        if (!preparing_.valid() && !recordingReady()) prepareRecording();
        if (preparing_.valid()) takePreparedRecording(std::chrono::milliseconds{200});
        if (preparing_.valid())
        {
            startWhenRead_ = mode;
            showMessage("Reading the recording's index; it starts when that is done.");
            updateControls();
            return;
        }
        if (!preparedReader_)
        {
            showMessage(QString("Cannot start: %1").arg(preparedError_));
            return;
        }
    }
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
        for (const auto& family : cameraFamilies_) family.panel->forgetSource();
        showMessage(QString("Cannot start: %1").arg(error));
        updateControls();
        updateStatusLabel();
        return;
    }

    sourceFailed_ = false;
    if (CameraFamilyPanel* camera = activeCamera()) camera->showSourceReadback(false);
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
    showMessage({});

    bufferReadoutTimer_->start();
    updateControls();
    updateStatusLabel();
    updateBlipReadout();
}

void MainWindow::onStopClicked()
{
    // The observer's stop: the night schedule leaves it until the next dusk.
    nightOwns_ = NightOwns::Nothing;
    finishStream();
}

void MainWindow::onMonitoringClicked()
{
    if (!session_->isRunning() || session_->modeChangePending()) return;
    const bool monitoring=session_->mode()==Session::Mode::Monitoring;
    if (!monitoring && activeCamera())
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
    ui_->sourceComboBox->setCurrentIndex(ui_->sourceComboBox->findData("recording"));
    ui_->fileLabel->setToolTip(QDir::toNativeSeparators(recordingPath_));
    prepareRecording();
    updateControls();
}

bool MainWindow::recordingReady() const
{
    return !preparing_.valid() && preparedPath_ == recordingPath_ && preparedFps_ == ui_->targetFpsSpinBox->value();
}

void MainWindow::prepareRecording()
{
    if (recordingPath_.isEmpty()) return;
    // One read at a time; a newer choice is read when the current one ends.
    if (preparing_.valid())
    {
        prepareAgain_ = true;
        return;
    }
    preparedReader_.reset();
    preparedPath_.clear();
    preparedError_.clear();
    ui_->fileLabel->setText(QString("%1 — reading its index…").arg(QFileInfo(recordingPath_).fileName()));
    preparing_ = std::async(std::launch::async, [path = recordingPath_, fps = ui_->targetFpsSpinBox->value()] {
        PreparedRecording prepared{path, fps, {}, {}, {}};
        std::string error;
        prepared.reader = openRecordingReader(std::filesystem::path(path.toStdWString()), fps, error);
        prepared.error = QString::fromStdString(error);
        if (prepared.reader)
        {
            const int count = prepared.reader->frameCount();
            const double seconds = std::chrono::duration<double>(
                prepared.reader->frameTime(count - 1) - prepared.reader->frameTime(0)).count();
            prepared.summary = QString("%1 frames, %2 s").arg(count).arg(seconds, 0, 'f', 1);
        }
        return prepared;
    });
    prepareTimer_->start();
}

bool MainWindow::takePreparedRecording(std::chrono::milliseconds wait)
{
    if (!preparing_.valid() || preparing_.wait_for(wait) != std::future_status::ready) return false;
    PreparedRecording prepared = preparing_.get();
    prepareTimer_->stop();
    // The choice or the frame rate changed while this one was read.
    if (prepareAgain_ || prepared.path != recordingPath_ || prepared.fps != ui_->targetFpsSpinBox->value())
    {
        prepareAgain_ = false;
        prepareRecording();
        return false;
    }
    preparedReader_ = std::move(prepared.reader);
    preparedPath_ = prepared.path;
    preparedFps_ = prepared.fps;
    preparedError_ = prepared.error;
    const QString name = QFileInfo(prepared.path).fileName();
    ui_->fileLabel->setText(preparedReader_ ? QString("%1 — %2").arg(name, prepared.summary) : QString("%1 — cannot be read").arg(name));
    if (!preparedReader_) showMessage(QString("Cannot read %1: %2").arg(name, prepared.error));
    if (startWhenRead_)
    {
        const Session::Mode mode = *startWhenRead_;
        startWhenRead_.reset();
        if (preparedReader_ && !session_->isRunning()) startStream(mode);
    }
    updateControls();
    return true;
}

void MainWindow::onSourceKindChanged()
{
    if (session_->isRunning()) return;
    CameraFamilyPanel* selected = selectedCamera();
    for (const auto& family : cameraFamilies_)
    {
        family.dock->setVisible(family.panel == selected);
        if (family.panel != selected) continue;
        family.dock->raise();
        detectionDock_->raise();
        family.panel->refresh();
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
    CameraFamilyPanel* camera = selectedCamera();
    if (!camera || !camera->hasCamera()) return false;
    try
    {
        camera->validateSettings();
        return true;
    }
    catch (const std::exception& ex)
    {
        showMessage(QString::fromUtf8(ex.what()));
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
    else if (CameraFamilyPanel* camera = selectedCamera())
    {
        std::string error;
        if (camera->readBack(error)) showMessage("Camera settings read back; ready to Start",5000);
        else showMessage(QString::fromStdString(error));
    }
    updateControls();
}

void MainWindow::onApplyLiveCamera()
{
    if (session_->mode()!=Session::Mode::Preview || session_->modeChangePending() || session_->sourceFinished()) return;
    try
    {
        if (CameraFamilyPanel* camera = activeCamera()) camera->requestLiveControls();
    }
    catch (const std::exception& ex) { showMessage(QString::fromUtf8(ex.what())); }
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
    if (!testingTools_) return; // the test flash is a testing tool
    const auto centre = ui_->previewWidget->selectedPosition();
    if (!centre)
    {
        return;
    }
    const BlipSettings settings{centre->x(), centre->y(), ui_->blipFwhmSpinBox->value(),
        ui_->blipAmplitudeSpinBox->value(), ui_->blipFramesSpinBox->value()};
    if (session_->requestBlip(settings))
    {
        showMessage("Test flash requested");
    }
    updateBlipReadout();
}

void MainWindow::onTargetFpsChanged(int fps)
{
    if (synthetic_)
    {
        synthetic_->setTargetFps(fps);
    }
    // A file without frame times or a rate is timed at this rate, so its
    // reader is read again.
    if (recordingSelected() && !session_->isRunning() && !recordingPath_.isEmpty()) prepareRecording();
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
        if (const int side = std::min(frame->width, frame->height); side != frameShortSide_)
        {
            frameShortSide_ = side;
            guidingPanel_->setFrameShortSide(side);
            sendTolerance();
        }
        renderPreview();
        updateHistogram();
        if (packet.detection)
        {
            const auto& view = *packet.detection;
            latestDetection_ = view.summary;
            try { detectionPanel_->setView(view); }
            catch (const MemoryLimitError&) { showMessage("Overlay skipped: RAM ceiling reached"); }
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
    showMessage(QString("Candidate %1 detected").arg(candidates_));
}

void MainWindow::onSaveNowClicked()
{
    if (session_->requestManualSave())
        showMessage("Manual capture requested",3000);
    updateControls();
}

void MainWindow::onClipWritten(const ClipWriteResult& result)
{
    if (result.memoryLimited) { ++clipsLimited_; updateCandidateReadout(); }
    if (result.recordFailed)
    {
        recordFailed_ = true;
    }

    if (result.diskReserve)
    {
        // Feature-11: saving stops before the drive fills. The candidates are still logged.
        ++clipsNotSaved_;
        updateCandidateReadout();
        showMessage(QString("Saving stopped: %1 GiB free on the output drive, below the disk reserve of %2 GiB. "
            "Candidates are still logged.")
            .arg(static_cast<double>(result.freeBytes) / kGiB, 0, 'f', 1)
            .arg(static_cast<double>(displayPanel_->captureSettings().diskReserveBytes) / kGiB, 0, 'f', 0));
    }
    else if (!result.notSaved.isEmpty())
    {
        ++clipsNotSaved_;
        updateCandidateReadout();
        showMessage(QString("Clip not saved: %1").arg(result.notSaved));
    }
    else if (!result.folder.isEmpty())
    {
        showMessage(result.memoryLimited ? "Clip saved at RAM limit: " + result.folder : result.folder);
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
    showMessage("Playback finished");
}

void MainWindow::onSourceFailed(const QString& message)
{
    if (!session_->isRunning() || !session_->sourceFinished())
    {
        return;
    }
    sourceFailed_ = true;
    finishStream();
    showMessage(message);
}
