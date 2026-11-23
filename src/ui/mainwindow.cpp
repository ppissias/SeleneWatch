#include "ui/mainwindow.h"
#include "ui_mainwindow.h"
#include "ui/auto_tune_panel.h"
#include "ui/detection_panel.h"
#include "ui/display_panel.h"
#include "ui/widgets/elided_label.h"
#include "ui/widgets/flow_layout.h"
#include "ui/widgets/control_help.h"
#include "ui/histogram_panel.h"
#include "ui/guiding_panel.h"
#include "ui/timelapse_panel.h"
#include "ui/resharpen_dialog.h"
#include "ui/manual_dialog.h"
#include "ui/tips_dialog.h"
#include "ui/acknowledgements_dialog.h"
#include "ui/release_checker.h"
#include "ui/widgets/wheel_guard.h"
#include "ui/cameras/camera_panel.h"
#include "ui/cameras/camera_support_dialog.h"
#include "ui/cameras/playerone_camera_panel.h"
#include "ui/cameras/qhy_camera_panel.h"
#include "ui/cameras/touptek_camera_panel.h"

#include "mounts/ascom_mount.h"
#include "mounts/simulated_mount.h"
#include "display/preview_image.h"
#include "recording/metadata_text.h"
#include "recording/output_paths.h"
#include "ui/cameras/camera_settings_editor.h"
#include "selenewatch_version.h"
#include "ui/site_dialog.h"
#include "detection/target_defaults.h"
#include "settings/watch_settings.h"
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
#include <QtGui/QActionGroup>
#include <QtGui/QCloseEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QScreen>
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
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWhatsThis>

#include <filesystem>
#include <algorithm>
#include <utility>

namespace {

// Fast playback entry in the form's speed box.
constexpr int kFastEntry = 1;

constexpr int kSyntheticWidth = 640;
constexpr int kSyntheticHeight = 480;

constexpr uint64_t kGiB = uint64_t{1024} * 1024 * 1024;

constexpr const char* kRecordTip = "In Preview or Monitoring, writes every frame to one SER file in recordings/ "
                                   "until stopped (or the optional frame limit is reached).";

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

// Feature-33: the Record button's mark, a red dot to start and a white square to stop.
QIcon recordMark(bool stop)
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(stop ? QColor(Qt::white) : QColor(0xd0, 0x30, 0x30));
    if (stop) painter.drawRect(12, 12, 40, 40);
    else painter.drawEllipse(8, 8, 48, 48);
    return QIcon(pixmap);
}

// A recording's length on the Stop button: m:ss, or h:mm:ss from an hour on.
QString recordingLength(double seconds)
{
    const auto total = static_cast<long long>(std::max(0.0, seconds));
    const long long hours = total / 3600;
    const long long minutes = (total / 60) % 60;
    const long long secs = total % 60;
    if (hours > 0)
        return QString("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
    return QString("%1:%2").arg(minutes).arg(secs, 2, 10, QChar('0'));
}

} // namespace

MainWindow::MainWindow(QWidget *parent, QString outputRoot,std::shared_ptr<ZwoApi> cameraApi,std::shared_ptr<QhyApi> qhyApi,
    std::shared_ptr<PlayerOneApi> playerOneApi, std::shared_ptr<ToupTekApi> toupTekApi)
    : QMainWindow(parent)
    , ui_(std::make_unique<Ui::MainWindowClass>())
{
    ui_->setupUi(this);
    ui_->verticalLayout->setContentsMargins(6, 4, 6, 4);
    ui_->verticalLayout->setSpacing(4);
    setWindowTitle(QString("SeleneWatch %1 — Lunar Observation Monitor").arg(SELENEWATCH_RELEASE));
    applyUiTheme(loadUiTheme());

    // Prevent accidental mouse-wheel changes on spin boxes, sliders, and closed combo boxes.
    wheelGuard_ = new WheelGuard(this);
    if (QCoreApplication* app = QCoreApplication::instance())
        app->installEventFilter(wheelGuard_);

    // Remembered output root across restarts and upgrades, or isolated root in tests.
    customOutputRoot_ = !outputRoot.isEmpty();
    if (!customOutputRoot_)
    {
        const QString savedFolder = QSettings().value("output/folder").toString().trimmed();
        if (savedFolder.isEmpty())
        {
            outputRoot = defaultOutputFolder();
            QDir().mkpath(outputRoot);
        }
        else
        {
            outputRoot = QDir::cleanPath(savedFolder);
        }
    }
    else
    {
        // Tests may pass an uncreated subdirectory under a temporary folder.
        QDir().mkpath(outputRoot);
    }

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
        onCameraReadbackForAutoTune();
    };
    notifications.autoTuneChanged = [this] { onAutoTuneChanged(); };
    notifications.manualSaveHandled = [this](bool saved) {
        if (saved) { ++manualCaptures_; updateCandidateReadout(); }
        updateControls();
        showMessage(saved ? "Manual capture marked; collecting following frames." : "Manual capture unavailable: no frames acquired.",5000);
    };
    notifications.reportReady = [this](QString path, QString error) {
        if (!error.isEmpty()) { showMessage(error); return; }
        reportOffer_->setProperty("reportPath", path);
        reportOffer_->show();
        // Feature-32: why a session ended by itself stays in view.
        showMessage((endNote_.isEmpty() ? QString("Monitoring finished.") : endNote_) + " Your nightly report is ready.");
    };
    notifications.clipWritten = [this](const ClipWriteResult& result) { onClipWritten(result); };
    notifications.ended = [this] { onPlaybackEnded(); };
    notifications.failed = [this](const QString& message) { onSourceFailed(message); };
    notifications.lockChanged = [this] { onLockChanged(); };
    notifications.recordingChanged = [this] { onRecordingChanged(); };
    notifications.watchTimelapseEnded = [this](const SkyTimelapse::Status& status) {
        // Feature-36: where the time-lapse is, and what it holds.
        if (status.frames == 0) showMessage("The time-lapse kept no frames. " + status.problem, 10000);
        else showMessage(QString("Time-lapse finished: %1 frames%2 in %3. %4").arg(status.frames)
            .arg(status.videoPath.isEmpty() ? QString() : " and timelapse.mp4")
            .arg(QDir::toNativeSeparators(status.folder), status.problem), 10000);
        updateStatusLabel();
    };
    notifications.timeLapseChanged = [this](const TimelapseStatus& status) {
        if (timelapsePanel_) timelapsePanel_->updateStatus(status);
        if (detectionPanel_ && session_->mode() == Session::Mode::TimeLapse)
        {
            const bool transits = timelapsePanel_ && timelapsePanel_->settings().watchTransits;
            QString detail;
            switch (status.transitWatch)
            {
            case TransitWatchState::Watching: detail = "watching between bursts"; break;
            case TransitWatchState::Saving: detail = "saving transit clip"; break;
            case TransitWatchState::Capturing: detail = "paused during burst capture"; break;
            case TransitWatchState::Stacking: detail = "paused during stacking"; break;
            case TransitWatchState::Off:
            case TransitWatchState::Finished: break;
            }
            detectionPanel_->setTimeLapseState(true, transits, detail);
        }
        updateClosingProgress(status);
    };
    notifications.timeLapseFinished = [this](const TimelapseFinishResult& result) {
        onTimeLapseFinished(result);
    };
    session_ = std::make_unique<Session>(this, outputRoot, std::move(notifications));

    setupStatusBar();
    setupPreviewZoomBar();
    setupBlipControls();
    setupDetectionPanel();
    setupDisplayPanel();
    setupHistogramPanel();
    setupGuidingPanel();
    setupTimelapsePanel();
    if (detectionDock_) detectionDock_->raise();
    setupSessionMenu();
    setupRemoteStatus();
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
    setupViewMenuExtras();
    setupSourceChoice();
    arrangeTopControls();
    applyDefaultDockLayout();
    restoreWindowLayout();
    loadSessionEnd();

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
        if (CameraFamilyPanel* camera = selectedCamera();
            camera && camera->controls() && camera->controls()->drawingRoi())
        {
            chooseDrawing(Drawing::None);
            if (!session_->isRunning() || session_->mode() != Session::Mode::Preview || activeCamera() != camera)
                return;
            const AlignedRoi aligned = camera->controls()->setDrawnRoi(region);
            const QString note = aligned.adjusted
                ? QString("Camera ROI set to %1 × %2 at (%3, %4) (rounded for %5).")
                      .arg(aligned.width).arg(aligned.height).arg(aligned.x).arg(aligned.y)
                      .arg(camera->controls()->alignmentHint())
                : QString("Camera ROI set to %1 × %2 at (%3, %4).")
                      .arg(aligned.width).arg(aligned.height).arg(aligned.x).arg(aligned.y);
            applyCameraRoiChange(note);
            return;
        }
        // Feature-26: the box auto-tune grows the earthshine area from.
        if (drawingEarthshineBox_)
        {
            setEarthshineBox(region);
            showMessage("Earthshine box set; its area is outlined on the preview.", 4000);
            return;
        }
        if (timelapsePanel_ && timelapsePanel_->selectingAnchor())
        {
            timelapsePanel_->setLunarAnchor(region);
            chooseDrawing(Drawing::None);
            ui_->previewWidget->setAnchorBox(region);
            showMessage("Lunar time-lapse anchor region set.", 4000);
            return;
        }
        // Feature-11: in Sky mode a drag draws the mask, and guiding is off.
        if (detectionPanel_->drawingMask())
        {
            pendingShape_.clear();
            ui_->previewWidget->setPendingShape({});
            if (!detectionPanel_->addMaskRect(region))
                showMessage(QString("The mask is full: %1 shapes. Clear it to start again.").arg(DetectionMask::kMaxShapes), 5000);
            return;
        }
        // Feature-28: a drag locks only after Draw lock box, one box per press.
        if (guidingPanel_ && guidingPanel_->drawingLock())
        {
            chooseDrawing(Drawing::None);
            if (target() == Target::Sky || !session_->isRunning()) return;
            session_->guider().requestLock({region.x(), region.y(), region.width(), region.height()});
            showMessage("Locking on the selected region…", 3000);
            return;
        }
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

    applyTheme(loadUiTheme(), false);
    updateControls();
    updateStatusLabel();
    updateBlipReadout();
    // Feature-24: every control's tool tip is also its What's This help, and
    // the label naming it gets a clickable ⓘ.
    installControlHelp(this);
    QTimer::singleShot(0, this, &MainWindow::checkLegacyEventsNotice);
}

// Feature-23: the controls above the preview wrap onto another line when
// the window is narrow, instead of being squeezed until their text is cut
// off. Controls that belong together stay together on one line.
void MainWindow::arrangeTopControls()
{
    const auto together = [](QWidget* parent, std::initializer_list<QWidget*> widgets) {
        auto* box = new QWidget(parent);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        for (QWidget* widget : widgets) row->addWidget(widget);
        return box;
    };

    // A long recording name is shortened with "…" rather than widening the bar.
    auto* file = new ElidedLabel(ui_->fileLabel->text(), Qt::ElideMiddle, this);
    file->setObjectName("fileLabel");
    file->setMaximumWidth(220);
    file->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    delete ui_->fileLabel;
    ui_->fileLabel = file;

    // Remove the legacy stacked rows from centralWidget and replace them with
    // one full-width top command toolbar + a slim playback bar (hidden for cameras).
    ui_->verticalLayout->removeItem(ui_->sourceLayout);
    delete ui_->sourceLayout;
    ui_->sourceLayout = nullptr;
    delete ui_->line;
    delete ui_->line2;
    ui_->line = ui_->line2 = nullptr;
    ui_->verticalLayout->removeItem(ui_->controlsLayout);
    delete ui_->controlsLayout;
    ui_->controlsLayout = nullptr;

    commandToolBar_ = new QToolBar("Controls", this);
    commandToolBar_->setObjectName("commandToolBar");
    commandToolBar_->setMovable(false);
    commandToolBar_->setFloatable(false);
    commandToolBar_->toggleViewAction()->setVisible(false);
    // Keep the full-width command bar expanded inside QToolBar without ever
    // collapsing the whole bar into QToolBar's ">>" overflow popup.
    class CommandBarWidget final : public QWidget
    {
    public:
        using QWidget::QWidget;
        [[nodiscard]] QSize sizeHint() const override { return {1, QWidget::sizeHint().height()}; }
        [[nodiscard]] QSize minimumSizeHint() const override { return {1, QWidget::minimumSizeHint().height()}; }
    };
    auto* bar = new CommandBarWidget(commandToolBar_);
    bar->setObjectName("commandBar");
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* barRow = new QHBoxLayout(bar);
    barRow->setContentsMargins(6, 2, 6, 2);
    barRow->setSpacing(6);

    barRow->addWidget(ui_->sourceLabel);
    barRow->addWidget(ui_->sourceComboBox);
    barRow->addWidget(connectAtStartup_);
    recordingFileGroup_ = together(bar, {ui_->openFileButton, file});
    barRow->addWidget(recordingFileGroup_);
    barRow->addWidget(testingBadge_);

    auto* sep1 = new QFrame(bar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    barRow->addWidget(sep1);

    ui_->stopButton->setText("Stop");
    barRow->addWidget(ui_->startButton);
    barRow->addWidget(ui_->stopButton);
    barRow->addWidget(ui_->monitoringButton);
    barRow->addWidget(ui_->saveNowButton);
    barRow->addWidget(recordButton_);
    barRow->addWidget(recordFramesSpinBox_);
    barRow->addWidget(ui_->injectFlashButton);

    auto* sep2 = new QFrame(bar);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setFrameShadow(QFrame::Sunken);
    barRow->addWidget(sep2);

    QWidget* ends = makeSessionEndRow(bar);
    barRow->addWidget(ends);
    barRow->addStretch(1);
    barRow->addWidget(modeLabel_);

    commandToolBar_->addWidget(bar);
    bar->show();
    addToolBar(Qt::TopToolBarArea, commandToolBar_);

    // Recording / Synthetic playback controls live on a slim bar that hides completely for live cameras.
    playbackBar_ = new QWidget(ui_->centralWidget);
    playbackBar_->setObjectName("playbackControls");
    auto* pbRow = new QHBoxLayout(playbackBar_);
    pbRow->setContentsMargins(0, 0, 0, 0);
    pbRow->setSpacing(10);
    fpsGroup_ = together(playbackBar_, {ui_->fpsLabel, ui_->targetFpsSpinBox});
    recordingPlaybackGroup_ = together(playbackBar_, {ui_->speedLabel, ui_->speedComboBox, ui_->loopCheckBox, ui_->loopSecondsSpinBox});
    pbRow->addWidget(fpsGroup_);
    pbRow->addWidget(recordingPlaybackGroup_);
    pbRow->addStretch(1);
    ui_->verticalLayout->insertWidget(0, playbackBar_);

    // Labels name their controls, so each gets the control's ⓘ help (Feature-24).
    ui_->sourceLabel->setBuddy(ui_->sourceComboBox);
    ui_->fpsLabel->setBuddy(ui_->targetFpsSpinBox);
    ui_->speedLabel->setBuddy(ui_->speedComboBox);
    ui_->blipKindLabel->setBuddy(ui_->blipKindComboBox);
    ui_->blipFwhmLabel->setBuddy(ui_->blipFwhmSpinBox);
    ui_->blipAmplitudeLabel->setBuddy(ui_->blipAmplitudeSpinBox);
    ui_->blipFramesLabel->setBuddy(ui_->blipFramesSpinBox);

    // The test flash controls wrap the same way; its two readouts wrap their text.
    QGroupBox* blip = ui_->blipGroupBox;
    delete blip->layout();
    ui_->blipLayout = nullptr;
    auto* blipControls = new QWidget(blip);
    auto* blipFlow = new FlowLayout(blipControls, 16, 4);
    blipFlow->addWidget(together(blipControls, {ui_->blipKindLabel, ui_->blipKindComboBox}));
    blipFlow->addWidget(together(blipControls, {ui_->blipFwhmLabel, ui_->blipFwhmSpinBox}));
    blipFlow->addWidget(together(blipControls, {ui_->blipAmplitudeLabel, ui_->blipAmplitudeSpinBox}));
    blipFlow->addWidget(together(blipControls, {ui_->blipFramesLabel, ui_->blipFramesSpinBox}));
    blipFlow->addWidget(ui_->injectBlipButton);
    ui_->blipPositionLabel->setWordWrap(true);
    ui_->blipStatusLabel->setWordWrap(true);
    auto* readouts = new QHBoxLayout;
    readouts->addWidget(ui_->blipPositionLabel, 1);
    readouts->addWidget(ui_->blipStatusLabel, 2);
    auto* column = new QVBoxLayout(blip);
    column->addWidget(blipControls);
    column->addLayout(readouts);
}

void MainWindow::setupViewMenuExtras()
{
    viewMenu_->addSeparator();
    focusViewAction_ = viewMenu_->addAction("Focus view (hide panels)");
    focusViewAction_->setObjectName("focusViewAction");
    focusViewAction_->setCheckable(true);
    focusViewAction_->setShortcut(QKeySequence(Qt::Key_F11));
    focusViewAction_->setToolTip("Hide all side panels to maximize the live camera view (F11). Trigger again to restore.");
    connect(focusViewAction_, &QAction::toggled, this, &MainWindow::setFocusView);
    if (focusViewButton_)
        connect(focusViewButton_, &QToolButton::toggled, this, &MainWindow::setFocusView);

    QMenu* themeMenu = viewMenu_->addMenu("Theme");
    themeMenu->setObjectName("themeMenu");
    themeActions_ = new QActionGroup(this);
    themeActions_->setExclusive(true);
    auto addThemeAction = [&](const QString& text, const QString& objName, UiTheme theme) {
        QAction* act = themeMenu->addAction(text);
        act->setObjectName(objName);
        act->setCheckable(true);
        themeActions_->addAction(act);
        connect(act, &QAction::triggered, this, [this, theme] { applyTheme(theme, true); });
        return act;
    };
    themeSystemAction_ = addThemeAction("System (light)", "themeSystemAction", UiTheme::System);
    themeDarkAction_ = addThemeAction("Dark", "themeDarkAction", UiTheme::Dark);
    themeNightRedAction_ = addThemeAction("Red night vision", "themeNightRedAction", UiTheme::NightRed);

    resetLayoutAction_ = viewMenu_->addAction("Reset window layout");
    resetLayoutAction_->setObjectName("resetLayoutAction");
    resetLayoutAction_->setToolTip("Restore all panels to the default single right-hand sidebar layout.");
    connect(resetLayoutAction_, &QAction::triggered, this, &MainWindow::resetWindowLayout);
    viewMenu_->addSeparator();
}

void MainWindow::applyDefaultDockLayout()
{
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);

    const bool histWasVisible = histogramDock_ && !histogramDock_->isHidden();
    for (const auto& family : cameraFamilies_)
    {
        family.dock->setFloating(false);
        removeDockWidget(family.dock);
    }
    for (QDockWidget* dock : {detectionDock_, displayDock_, guidingDock_, timelapseDock_, histogramDock_})
    {
        if (!dock) continue;
        dock->setFloating(false);
        removeDockWidget(dock);
    }

    // Three distinct vertical slots in the single right-hand sidebar:
    //   1. Active Camera dock (top; hidden when non-camera source selected)
    //   2. Tabbed Detection | Display | Guiding | Time-lapse (middle)
    //   3. Compact Histogram dock (bottom; toggleable via View bar / View menu)
    QDockWidget* firstCameraDock = nullptr;
    for (const auto& family : cameraFamilies_)
    {
        addDockWidget(Qt::RightDockWidgetArea, family.dock);
        if (!firstCameraDock) firstCameraDock = family.dock;
        else tabifyDockWidget(firstCameraDock, family.dock);
    }

    if (detectionDock_) addDockWidget(Qt::RightDockWidgetArea, detectionDock_);
    if (displayDock_) addDockWidget(Qt::RightDockWidgetArea, displayDock_);
    if (guidingDock_) addDockWidget(Qt::RightDockWidgetArea, guidingDock_);
    if (timelapseDock_) addDockWidget(Qt::RightDockWidgetArea, timelapseDock_);
    if (detectionDock_ && displayDock_) tabifyDockWidget(detectionDock_, displayDock_);
    if (displayDock_ && guidingDock_) tabifyDockWidget(displayDock_, guidingDock_);
    if (guidingDock_ && timelapseDock_) tabifyDockWidget(guidingDock_, timelapseDock_);

    if (histogramDock_) addDockWidget(Qt::RightDockWidgetArea, histogramDock_);

    CameraFamilyPanel* selected = selectedCamera();
    for (const auto& family : cameraFamilies_)
        family.dock->setVisible(family.panel == selected);
    for (QDockWidget* dock : {detectionDock_, displayDock_, guidingDock_, timelapseDock_})
        if (dock) dock->show();
    if (detectionDock_) detectionDock_->raise();
    if (histogramDock_) histogramDock_->setVisible(histWasVisible);

    if (detectionDock_) resizeDocks({detectionDock_}, {350}, Qt::Horizontal);
    if (firstCameraDock && detectionDock_ && histogramDock_)
        resizeDocks({firstCameraDock, detectionDock_, histogramDock_}, {280, 340, 140}, Qt::Vertical);
    else if (detectionDock_ && histogramDock_)
        resizeDocks({detectionDock_, histogramDock_}, {520, 140}, Qt::Vertical);
}

void MainWindow::saveWindowLayout()
{
    // Never persist the temporary Focus-view hidden state as the observer's permanent dock layout.
    const bool wasFocus = focusView_;
    if (wasFocus) setFocusView(false);
    QSettings saved;
    saved.setValue("ui/windowGeometry", saveGeometry());
    saved.setValue("ui/windowState", saveState(2));
    saved.sync();
    if (wasFocus) setFocusView(true);
}

void MainWindow::restoreWindowLayout()
{
    QSettings saved;
    const QByteArray geom = saved.value("ui/windowGeometry").toByteArray();
    const QByteArray state = saved.value("ui/windowState").toByteArray();
    if (!geom.isEmpty())
    {
        restoreGeometry(geom);
        const QRect frame = frameGeometry();
        bool onScreen = false;
        for (const QScreen* screen : QGuiApplication::screens())
            if (screen && screen->availableGeometry().intersects(frame)) { onScreen = true; break; }
        if (!onScreen)
        {
            if (const QScreen* primary = QGuiApplication::primaryScreen())
            {
                const QRect avail = primary->availableGeometry();
                resize(std::min(1280, avail.width()), std::min(760, avail.height()));
                move(avail.topLeft() + QPoint(40, 40));
            }
        }
    }
    else
    {
        int w = 1280, h = 760;
        if (const QScreen* primary = QGuiApplication::primaryScreen())
        {
            const QRect avail = primary->availableGeometry();
            w = std::min(w, std::max(900, avail.width() - 60));
            h = std::min(h, std::max(660, avail.height() - 60));
        }
        resize(w, h);
    }
    if (!state.isEmpty())
        restoreState(state, 2);

    CameraFamilyPanel* selected = selectedCamera();
    for (const auto& family : cameraFamilies_)
    {
        const bool isSelected = family.panel == selected;
        family.dock->setVisible(isSelected);
        family.dock->toggleViewAction()->setVisible(isSelected);
    }
    if (histogramToggleButton_ && histogramDock_)
    {
        const QSignalBlocker blocked(histogramToggleButton_);
        histogramToggleButton_->setChecked(!histogramDock_->isHidden());
    }
}

void MainWindow::resetWindowLayout()
{
    if (focusView_) setFocusView(false);
    QSettings saved;
    saved.remove("ui/windowGeometry");
    saved.remove("ui/windowState");
    saved.sync();
    applyDefaultDockLayout();
    for (QDockWidget* dock : {detectionDock_, displayDock_, guidingDock_, timelapseDock_})
        if (dock) dock->show();
    if (detectionDock_) detectionDock_->raise();
    if (histogramDock_) histogramDock_->hide();
    CameraFamilyPanel* selected = selectedCamera();
    for (const auto& family : cameraFamilies_)
    {
        const bool isSelected = family.panel == selected;
        family.dock->setVisible(isSelected);
        family.dock->toggleViewAction()->setVisible(isSelected);
    }
    showMessage("Window layout reset to default.", 4000);
}

void MainWindow::setFocusView(bool on)
{
    if (focusView_ == on) return;
    focusView_ = on;
    if (focusViewAction_) { const QSignalBlocker b(focusViewAction_); focusViewAction_->setChecked(on); }
    if (focusViewButton_) { const QSignalBlocker b(focusViewButton_); focusViewButton_->setChecked(on); }
    if (on)
    {
        focusSavedDocks_.clear();
        auto hideIfOpen = [this](QDockWidget* dock) {
            if (dock && !dock->isHidden())
            {
                focusSavedDocks_.push_back(dock);
                dock->hide();
            }
        };
        for (const auto& family : cameraFamilies_) hideIfOpen(family.dock);
        for (QDockWidget* dock : {detectionDock_, displayDock_, guidingDock_, timelapseDock_, histogramDock_})
            hideIfOpen(dock);
    }
    else
    {
        CameraFamilyPanel* selected = selectedCamera();
        for (QDockWidget* dock : focusSavedDocks_)
        {
            if (!dock) continue;
            bool isCameraDock = false, isSelectedCameraDock = false;
            for (const auto& family : cameraFamilies_)
            {
                if (family.dock == dock)
                {
                    isCameraDock = true;
                    isSelectedCameraDock = (family.panel == selected);
                    break;
                }
            }
            if (!isCameraDock || isSelectedCameraDock) dock->show();
        }
        focusSavedDocks_.clear();
    }
}

void MainWindow::applyTheme(UiTheme theme, bool persist)
{
    applyUiTheme(theme);
    if (persist) saveUiTheme(theme);
    if (themeSystemAction_) { const QSignalBlocker b(themeSystemAction_); themeSystemAction_->setChecked(theme == UiTheme::System); }
    if (themeDarkAction_) { const QSignalBlocker b(themeDarkAction_); themeDarkAction_->setChecked(theme == UiTheme::Dark); }
    if (themeNightRedAction_) { const QSignalBlocker b(themeNightRedAction_); themeNightRedAction_->setChecked(theme == UiTheme::NightRed); }

    ui_->startButton->setStyleSheet(primaryActionStyle(theme));
    ui_->stopButton->setStyleSheet(stopActionStyle(theme));
    if (testingBadge_)
    {
        testingBadge_->setStyleSheet(theme == UiTheme::NightRed
            ? "background:#6b1414; color:#ff9999; font-weight:bold; padding:2px 6px; border-radius:3px;"
            : "background:#8a5a00; color:white; font-weight:bold; padding:2px 6px; border-radius:3px;");
    }
    if (previewHintLabel_)
    {
        previewHintLabel_->setStyleSheet(theme == UiTheme::NightRed ? "color: #9e2d2d;" : "color: #888888;");
    }
    if (detectionPanel_) detectionPanel_->refreshTheme();
    for (const auto& family : cameraFamilies_)
        if (CameraSettingsEditor* c = family.panel->controls()) c->refreshTheme();
    updateStatusLabel();
    if (histogramPanel_) histogramPanel_->update();
    ui_->previewWidget->update();
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
    if (session_->isRunning() || session_->timeLapseFinalizing())
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
    if (wheelGuard_)
    {
        if (QCoreApplication* app = QCoreApplication::instance())
            app->removeEventFilter(wheelGuard_);
    }
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
    if (timelapseDock_) disconnect(timelapseDock_, nullptr, this, nullptr);
    if (timelapsePanel_) disconnect(timelapsePanel_, nullptr, this, nullptr);
    if (guidingTimer_) guidingTimer_->stop();
    if (prepareTimer_) prepareTimer_->stop();
    // A recording being read is let finish; its result is dropped.
    if (preparing_.valid()) preparing_.wait();
    saveGuidingSettings();
    saveDetectionPanel();
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
    watch_ = savedWatch();
    detectionPanel_->setWatch(watch_);
    detectionPanel_->setSettings(savedDetectionSettings(watch_));
    session_->setDetectionSettings(detectionPanel_->settings());
    connect(detectionPanel_, &DetectionPanel::settingsChanged, this, [this] {
        session_->setDetectionSettings(detectionPanel_->settings());
        ui_->previewWidget->setMask(maskRects(detectionPanel_->settings().mask));
        onDetectionSettingsChangedForAutoTune();
    });
    connect(detectionPanel_, &DetectionPanel::watchRequested, this, &MainWindow::onWatchRequested);

    // Feature-15: the exposure follower.
    showSkyExtras();
    // Feature-36: the slow sky's time-lapse.
    detectionPanel_->setWatchTimelapse(savedWatchTimelapse());
    connect(detectionPanel_, &DetectionPanel::watchTimelapseChanged, this, [this] {
        saveWatchTimelapse(detectionPanel_->watchTimelapse());
        applyWatchTimelapse();
    });
    applyWatchTimelapse();
    connect(detectionPanel_, &DetectionPanel::skyExtrasChanged, this, [this] {
        saveSkyExtrasPanel();
        skyExposureState_ = {};
    });
    skyTimer_ = new QTimer(this);
    skyTimer_->setInterval(5000);
    connect(skyTimer_, &QTimer::timeout, this, [this] { checkSkyExposure(std::chrono::steady_clock::now()); });
    skyTimer_->start();
    connect(detectionPanel_, &DetectionPanel::maskDrawingChanged, this, [this](bool drawing) {
        chooseDrawing(drawing ? Drawing::Mask : Drawing::None);
        pendingShape_.clear();
        ui_->previewWidget->setPendingShape({});
        if (drawing) showMessage("Drag a rectangle, or click the corners of a shape and double-click to close it.", 6000);
    });
    connect(detectionPanel_, &DetectionPanel::displayChanged, this, &MainWindow::updateDetectionDisplay);
    connect(detectionDock_, &QDockWidget::visibilityChanged, this, &MainWindow::updateDetectionDisplay);
    updateDetectionDisplay();
    setupAutoTune();
    resize(1340, 820);
}

void MainWindow::saveDetectionPanel()
{
    if (detectionPanel_) saveDetectionSettings(detectionPanel_->watch(), detectionPanel_->settings());
}

void MainWindow::saveCapturePanel()
{
    if (displayPanel_) saveCaptureSettings(watch_, displayPanel_->captureSettings());
}

void MainWindow::onWatchRequested(Watch watch)
{
    const bool running = session_->isRunning();
    if (session_->timeLapseFinalizing() || (running && !session_->canChangePreviewWatch()) || watch == watch_)
    {
        detectionPanel_->setWatch(watch_);
        return;
    }
    // Keep the old watch's settings before the panels take the new ones. The
    // capture settings change with their key: both Moon watches share them.
    saveDetectionPanel();
    const bool captureChanges = !sameCaptureSettings(watch, watch_);
    if (captureChanges) saveCapturePanel();
    saveSkyExtrasPanel();
    const auto detection = savedDetectionSettings(watch);
    const auto capture = captureChanges ? savedCaptureSettings(watch) : displayPanel_->captureSettings();
    if (running && !session_->requestPreviewWatch(watch, detection, capture))
    {
        detectionPanel_->setWatch(watch_);
        return;
    }
    watch_ = watch;
    saveWatch(watch_);
    detectionPanel_->setWatch(watch_);
    detectionPanel_->setSettings(detection);
    if (captureChanges) displayPanel_->setCaptureSettings(capture);
    if (!running)
    {
        if (captureChanges) session_->setCaptureSettings(displayPanel_->captureSettings());
        session_->setWatch(watch_);
        session_->setDetectionSettings(detectionPanel_->settings());
    }
    stopCameraTune("The observing watch changed: tune again.");
    session_->cancelAutoTune("The observing watch changed: measure again.");
    if (autoTunePanel_)
    {
        autoTunePanel_->clearK("The observing watch changed: watch again.");
        autoTunePanel_->clearMinPatch("The observing watch changed: measure again.");
        tuneHasK_ = false;
        tuneHasMinPatch_ = false;
        tuneRestore_.reset();
        tuneHistory_.clear();
        autoTunePanel_->clearCamera("The observing watch changed: tune again.");
    }
    latestDetection_ = {};
    detectionPanel_->setView({});
    chooseDrawing(Drawing::None);
    showSkyExtras();
    applyWatchTimelapse();
    skyExposureState_ = {};
    showAutoTuneLongestExposure();
    applyTarget();
    renderPreview();
    updateControls();
    updateStatusLabel();
    showMessage(watch_ == Watch::Sky ? "Watching the sky for fast events: their settings are loaded. Guiding is for the Moon and is off."
        : watch_ == Watch::SkySlow ? "Watching the sky for slow and faint light: its settings are loaded. Guiding is for the Moon and is off."
        : watch_ == Watch::MoonShadows ? "Watching the Moon for shadows on the sunlit side: their settings are loaded."
        : "Watching the Moon for flashes on the earthlit side: their settings are loaded.", 6000);
}

void MainWindow::applyWatchTimelapse()
{
    const WatchTimelapse timelapse = detectionPanel_->watchTimelapse();
    session_->setWatchTimelapse(watch_ == Watch::SkySlow && timelapse.on ? timelapse.intervalSeconds : 0);
}

QString MainWindow::watchTimelapseReadout() const
{
    const SkyTimelapse::Status status = session_->watchTimelapseStatus();
    if (!status.active) return {};
    QString text = QString("Time-lapse: %1 frame%2 kept, one every %3 s.").arg(status.frames)
        .arg(status.frames == 1 ? "" : "s").arg(status.intervalSeconds, 0, 'g', 6);
    if (!status.problem.isEmpty()) text += " " + status.problem;
    return text;
}

// Feature-15, Feature-35: each sky watch's exposure follower.
void MainWindow::showSkyExtras()
{
    detectionPanel_->setSkyExtras(savedSkyExtras(watch_));
}

void MainWindow::saveSkyExtrasPanel()
{
    if (target() == Target::Sky) saveSkyExtras(watch_, detectionPanel_->skyExtras());
}

void MainWindow::checkSkyExposure(std::chrono::steady_clock::time_point now)
{
    const SkyExtras extras = detectionPanel_->skyExtras();
    if (target() != Target::Sky || !extras.follow)
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
    // Feature-26: auto-tune measures one settled configuration at a time, so
    // the follower waits, and its hold starts afresh when it resumes.
    if (autoTuneMeasuring())
    {
        skyExposureState_ = {};
        detectionPanel_->showFollow("Paused while auto-tune measures.");
        return;
    }
    // Every camera family numbers Exposure 0 and Gain 1, in microseconds and their own units.
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
    const auto measured = skyLevel(*latestFrame_, detectionPanel_->settings().mask);
    if (!measured)
    {
        // A fully masked sky has no level; reading it as black would keep
        // lengthening the exposure.
        skyExposureState_ = {};
        detectionPanel_->showFollow("The mask covers the whole frame: there is no sky to follow.");
        return;
    }
    const double level = *measured;
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
        session_->stop("the exposure follows the sky", true);
        startStream(Session::Mode::Monitoring);
        session_->finishInterruptedWatch();
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

// Feature-28: one drawing on the preview at a time, each with its own colour.
// Choosing one turns the others off; Drawing::None returns left-drag to panning.
void MainWindow::chooseDrawing(Drawing drawing)
{
    if (drawing != Drawing::EarthshineBox && drawingEarthshineBox_)
    {
        drawingEarthshineBox_ = false;
        if (autoTunePanel_) autoTunePanel_->setDrawingBox(false);
    }
    if (drawing != Drawing::LockBox && guidingPanel_) guidingPanel_->setDrawingLock(false);
    if (drawing != Drawing::Anchor && timelapsePanel_ && timelapsePanel_->selectingAnchor()) timelapsePanel_->cancelAnchorSelection();
    if (drawing != Drawing::Mask && detectionPanel_ && detectionPanel_->drawingMask()) detectionPanel_->setDrawingMask(false);
    if (drawing != Drawing::CameraRoi)
    {
        for (const auto& family : cameraFamilies_)
            if (CameraSettingsEditor* controls = family.panel->controls(); controls && controls->drawingRoi())
                controls->setDrawingRoi(false);
    }
    ui_->previewWidget->setDragMode(drawing == Drawing::None ? PreviewWidget::DragMode::Pan : PreviewWidget::DragMode::Region);
    switch (drawing)
    {
    case Drawing::None: break;
    case Drawing::LockBox: ui_->previewWidget->setDragColour(QColor(PreviewWidget::kLockColour)); break;
    case Drawing::EarthshineBox: ui_->previewWidget->setDragColour(QColor(PreviewWidget::kEarthshineColour)); break;
    case Drawing::Anchor: ui_->previewWidget->setDragColour(QColor("#ff99dd")); break;
    case Drawing::Mask: ui_->previewWidget->setDragColour(QColor(220, 70, 70)); break;
    case Drawing::CameraRoi: ui_->previewWidget->setDragColour(QColor(PreviewWidget::kCameraRoiColour)); break;
    }
    updatePreviewHint(drawing);
}

void MainWindow::updatePreviewHint(Drawing drawing)
{
    if (!previewHintLabel_) return;
    switch (drawing)
    {
    case Drawing::None:
        previewHintLabel_->setText("Mouse wheel: zoom · Drag: pan");
        break;
    case Drawing::LockBox:
        previewHintLabel_->setText("Drag: draw guiding lock box · Right/middle-drag: pan");
        break;
    case Drawing::EarthshineBox:
        previewHintLabel_->setText("Drag: draw earthshine box · Right/middle-drag: pan");
        break;
    case Drawing::Anchor:
        previewHintLabel_->setText("Drag: draw lunar anchor · Right/middle-drag: pan");
        break;
    case Drawing::Mask:
        previewHintLabel_->setText("Drag or click corners: draw mask · Right/middle-drag: pan");
        break;
    case Drawing::CameraRoi:
        previewHintLabel_->setText("Drag: draw camera ROI · Right/middle-drag: pan");
        break;
    }
}

void MainWindow::applyTarget()
{
    const bool sky = target() == Target::Sky;
    ui_->previewWidget->setMask(maskRects(detectionPanel_->settings().mask));
    if (guidingPanel_)
    {
        if (sky) session_->guider().requestUnlock();
        guidingPanel_->setEnabled(!sky);
        guidingDock_->setWindowTitle(sky ? "Guiding (Moon only)" : "Guiding");
    }
    if (timelapsePanel_)
    {
        if (sky)
        {
            timelapsePanel_->cancelAnchorSelection();
            ui_->previewWidget->setAnchorBox(std::nullopt);
        }
        else
        {
            ui_->previewWidget->setAnchorBox(timelapsePanel_->settings().lunarAnchor);
        }
    }
    // Feature-26: each target has its own aim; the earthshine outline is the Moon's.
    updateEarthshineArea();
    updateAutoTune();
    // Feature-39: match each camera panel's Exposure preset range to the active watch.
    const ExposureRange exposureRange = watch_ == Watch::MoonShadows ? ExposureRange::Planetary
        : watch_ == Watch::SkySlow ? ExposureRange::Sky : ExposureRange::Fast;
    for (const auto& family : cameraFamilies_)
        if (CameraSettingsEditor* controls = family.panel->controls())
            controls->setExposureRange(exposureRange);
}

void MainWindow::setupDisplayPanel()
{
    displayPanel_ = new DisplayPanel(this);
    displayDock_ = new QDockWidget("Display", this);
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
    displayPanel_->setCaptureSettings(savedCaptureSettings(watch_));
    session_->setCaptureSettings(displayPanel_->captureSettings());
    session_->setWatch(watch_);
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
    saveCapturePanel();
}

void MainWindow::setupHistogramPanel()
{
    histogramPanel_ = new HistogramPanel(this);
    histogramDock_ = new QDockWidget("Histogram", this);
    histogramDock_->setObjectName("histogramDock");
    histogramDock_->setWidget(histogramPanel_);
    addDockWidget(Qt::RightDockWidgetArea, histogramDock_);
    viewMenu_->addAction(histogramDock_->toggleViewAction());

    const QSettings saved;
    histogramPanel_->setOptions(saved.value("histogram/fitRange", false).toBool(),
        saved.value("histogram/logCounts", true).toBool());
    histogramDock_->setVisible(saved.value("histogram/visible", false).toBool());
    connect(histogramDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (histogramToggleButton_)
        {
            const QSignalBlocker blocked(histogramToggleButton_);
            histogramToggleButton_->setChecked(visible);
        }
        if (visible) updateHistogram(true);
    });
    if (histogramToggleButton_)
    {
        const QSignalBlocker blocked(histogramToggleButton_);
        histogramToggleButton_->setChecked(!histogramDock_->isHidden());
        connect(histogramToggleButton_, &QToolButton::toggled, this, [this](bool on) {
            if (histogramDock_) histogramDock_->setVisible(on);
        });
    }
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
    guidingPanel_->setGiveUpPercent(std::clamp(saved.value("guiding/giveUpPercent", 15.0).toDouble(), 5.0, 45.0));
    guidingPanel_->setAggressivenessPercent(std::clamp(saved.value("guiding/aggressivenessPercent",
        static_cast<int>(std::lround(GuideController::Settings{}.gain * 100.0))).toInt(), 30, 100));
    guidingPanel_->setMaxPulseMs(std::clamp(saved.value("guiding/maxPulseMs",
        GuideController::Settings{}.maxPulseMs).toInt(), 500, 5000));
    // Feature-32: how long a lost lock is searched for before guiding, and the session, end.
    guidingPanel_->setLostAfterSeconds(std::clamp(saved.value("guiding/lostAfterSeconds", LockTracker::kDefaultSearchSeconds)
        .toDouble(), LockTracker::kMinSearchSeconds, LockTracker::kMaxSearchSeconds));

    Guider& guider = session_->guider();
    guider.setLostAfter(guidingPanel_->lostAfterSeconds());
    guider.setCorrectionTuning(guidingPanel_->aggressivenessPercent() / 100.0, guidingPanel_->maxPulseMs());
    connect(guidingPanel_, &GuidingPanel::lockAtSelectionRequested, this, &MainWindow::onLockAtSelection);
    connect(guidingPanel_, &GuidingPanel::drawLockToggled, this, [this](bool on) {
        chooseDrawing(on ? Drawing::LockBox : Drawing::None);
        if (on) showMessage("Drag a box on the preview around a crater or other detail to lock guiding on it.", 6000);
    });
    connect(guidingPanel_, &GuidingPanel::unlockRequested, this, [&guider] { guider.requestUnlock(); });
    connect(guidingPanel_, &GuidingPanel::chooseDriverRequested, this, &MainWindow::onChooseMountDriver);
    connect(guidingPanel_, &GuidingPanel::connectRequested, this, &MainWindow::onConnectMount);
    connect(guidingPanel_, &GuidingPanel::disconnectRequested, this, [&guider] { guider.requestDisconnect(); });
    connect(guidingPanel_, &GuidingPanel::calibrateRequested, this, [&guider] { guider.requestCalibration(); });
    connect(guidingPanel_, &GuidingPanel::guidingRequested, this, [&guider](bool on) { guider.requestGuiding(on); });
    connect(guidingPanel_, &GuidingPanel::toleranceChanged, this, [this] { sendTolerance(); saveGuidingSettings(); });
    connect(guidingPanel_, &GuidingPanel::correctionTuningChanged, this, [this, &guider] {
        guider.setCorrectionTuning(guidingPanel_->aggressivenessPercent() / 100.0, guidingPanel_->maxPulseMs());
        saveGuidingSettings();
    });
    connect(guidingPanel_, &GuidingPanel::lostAfterChanged, this, [this, &guider] {
        guider.setLostAfter(guidingPanel_->lostAfterSeconds());
        saveGuidingSettings();
    });
    connect(guidingPanel_, &GuidingPanel::mountKindChanged, this, &MainWindow::saveGuidingSettings);

    // A hung driver call publishes nothing, so the panel also asks once a second.
    guidingTimer_ = new QTimer(this);
    guidingTimer_->setInterval(1000);
    connect(guidingTimer_, &QTimer::timeout, this, &MainWindow::onLockChanged);
    guidingTimer_->start();
}

void MainWindow::setupTimelapsePanel()
{
    timelapsePanel_ = new TimelapsePanel(this);
    timelapseDock_ = new QDockWidget("Time-lapse", this);
    timelapseDock_->setObjectName("timelapseDock");
    timelapseDock_->setWidget(timelapsePanel_);
    addDockWidget(Qt::RightDockWidgetArea, timelapseDock_);
    tabifyDockWidget(guidingDock_, timelapseDock_);
    viewMenu_->addAction(timelapseDock_->toggleViewAction());
    connect(timelapsePanel_, &TimelapsePanel::startRequested, this, &MainWindow::onStartTimeLapse);
    connect(timelapsePanel_, &TimelapsePanel::stopRequested, this, &MainWindow::onStopTimeLapse);
    connect(timelapsePanel_, &TimelapsePanel::resharpenRequested, this, &MainWindow::onOpenResharpen);
    connect(timelapsePanel_, &TimelapsePanel::configureTransitsRequested, this, [this] {
        if (watch_ != Watch::MoonShadows) onWatchRequested(Watch::MoonShadows);
        detectionDock_->show();
        detectionDock_->raise();
    });
    connect(timelapsePanel_, &TimelapsePanel::anchorSelectionChanged, this, [this](bool active) {
        chooseDrawing(active ? Drawing::Anchor : Drawing::None);
        if (active) showMessage("Drag a steady sunlit lunar region on the preview to set the time-lapse anchor.", 6000);
    });
    connect(timelapsePanel_, &TimelapsePanel::anchorChanged, this, [this](const std::optional<QRect>& rect) {
        ui_->previewWidget->setAnchorBox(rect);
    });
}

void MainWindow::onStartTimeLapse()
{
    if (session_->timeLapseFinalizing())
    {
        showMessage("Wait for the previous time-lapse video to finish finalizing.", 4000);
        return;
    }
    if (!ensureOutputFolderReady("time-lapse")) return;
    if (!session_->isRunning())
    {
        if (selectedCamera() && !validateCameraSettings()) return;
        startStream(Session::Mode::Preview);
        if (!session_->isRunning()) return;
    }
    if (session_->mode() != Session::Mode::Preview)
    {
        showMessage("Stop Monitoring before starting a time-lapse.", 4000);
        return;
    }
    if (session_->recordingStatus().active)
    {
        showMessage("Stop the active recording before starting a time-lapse.", 4000);
        return;
    }
    const TimelapseSettings tlSettings = timelapsePanel_->settings();
    if (!tlSettings.valid())
    {
        showMessage("Check time-lapse settings (capture window must be <= interval).", 5000);
        return;
    }
    // Feature-32: the first start with a mount asks what it does at the end.
    if (!askMountAtEnd()) return;
    timelapsePanel_->clearRunVisuals();
    QString error;
    const auto transitDetection = !tlSettings.watchTransits ? std::optional<DetectionSettings>{}
        : watch_ == Watch::MoonShadows ? detectionPanel_->settings() : savedDetectionSettings(Watch::MoonShadows);
    if (!session_->startTimeLapse(tlSettings, error, {}, {}, transitDetection))
    {
        showMessage(QString("Cannot start time-lapse: %1").arg(error), 5000);
        return;
    }
    beginSessionRun(true);
    timelapseDock_->raise();
    QString started = tlSettings.watchTransits ? "Time-lapse started with transit watching between stacks." : "Time-lapse started.";
    // Without an anchor a Moon time-lapse fits rotation over the whole frame,
    // where a passing cloud can rotate a stack wrongly.
    const bool unanchoredMoon = tlSettings.preset == TimelapsePreset::Moon
        && !(tlSettings.lunarAnchor && tlSettings.lunarAnchor->isValid());
    if (unanchoredMoon)
        started += " No lunar anchor: alignment uses the whole frame, so a passing cloud can rotate a stack wrongly. "
            "Select one before the next time-lapse.";
    showMessage(started, unanchoredMoon ? 15000 : 4000);
    updateControls();
    updateStatusLabel();
}

void MainWindow::onStopTimeLapse()
{
    sessionRun_.reset();
    session_->stopTimeLapse("stopped by the observer");
    showMessage("Stopping time-lapse and building video…", 4000);
    updateControls();
    updateStatusLabel();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    const bool finalizing = session_->timeLapseFinalizing();
    const bool timeLapsing = session_->mode() == Session::Mode::TimeLapse && !finalizing;
    if (!finalizing && !timeLapsing)
    {
        saveWindowLayout();
        QMainWindow::closeEvent(event);
        return;
    }
    event->ignore();
    if (closeAfterTimeLapse_)
    {
        showClosingProgress(); // Already waiting: bring the dialog back
        return;
    }
    if (timeLapsing && qEnvironmentVariable("QT_QPA_PLATFORM") != "offscreen")
    {
        QMessageBox box(this);
        box.setObjectName("closeDuringTimelapse");
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle("Time-lapse running");
        box.setText("A time-lapse is running.");
        box.setInformativeText("SeleneWatch will stop it, build its video from the stacks so far, "
            "and then close.");
        QPushButton* stopAndClose = box.addButton("Stop, build video and close", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != stopAndClose) return;
    }
    sessionRun_.reset();
    if (timeLapsing) session_->stopTimeLapse("the program closed");
    closeAfterTimeLapse_ = true;
    showClosingProgress();
    updateControls();
    updateStatusLabel();
}

void MainWindow::showClosingProgress()
{
    if (!closingProgress_)
    {
        closingProgress_ = new QProgressDialog(this);
        closingProgress_->setObjectName("closingTimelapseProgress");
        closingProgress_->setWindowTitle("Finishing time-lapse");
        closingProgress_->setWindowModality(Qt::WindowModal);
        closingProgress_->setAutoClose(false);
        closingProgress_->setAutoReset(false);
        closingProgress_->setMinimumDuration(0);
        closingProgress_->setCancelButtonText("Close without video");
        connect(closingProgress_, &QProgressDialog::canceled, this, [this] {
            // FITS stacks stay on disk; Re-sharpen → Rebuild video only makes the video later.
            session_->cancelTimeLapseExport();
            showMessage("Video abandoned; SeleneWatch closes when the time-lapse has stopped.");
        });
    }
    updateClosingProgress(session_->timeLapseStatus());
    closingProgress_->show();
    closingProgress_->raise();
}

void MainWindow::updateClosingProgress(const TimelapseStatus& status)
{
    if (!closingProgress_ || !closeAfterTimeLapse_ || closingProgress_->wasCanceled()) return;
    QString text = "Building the time-lapse video before SeleneWatch closes…";
    if (status.exportTotal > 0)
    {
        closingProgress_->setRange(0, status.exportTotal);
        closingProgress_->setValue(std::min(status.exportDone, status.exportTotal));
        text += QString("\n%1 / %2 frames encoded.").arg(status.exportDone).arg(status.exportTotal);
    }
    else
    {
        closingProgress_->setRange(0, 0); // Busy: the last stack is still being made
    }
    closingProgress_->setLabelText(text);
}

void MainWindow::onTimeLapseFinished(const TimelapseFinishResult& result)
{
    if (timelapsePanel_) timelapsePanel_->onSessionFinished(result);
    // Feature-32: timelapse.txt is written; what the mount did is added to it.
    writeTimeLapseMountLine(result.sessionDir);
    if (closeAfterTimeLapse_)
    {
        if (closingProgress_) closingProgress_->hide();
        // Close after this notification has been handled; closeEvent then finds
        // no time-lapse and lets the window go.
        QTimer::singleShot(0, this, [this] {
            closeAfterTimeLapse_ = false;
            close();
        });
        return;
    }
    if (result.ok)
    {
        showMessage(QString("Time-lapse complete: %1 stacks (%2 skipped) saved to %3.")
            .arg(result.stacksCompleted).arg(result.periodsSkipped)
            .arg(QDir::toNativeSeparators(result.sessionDir)), 8000);
    }
    else
    {
        showMessage(QString("Time-lapse finished with export warning: %1").arg(result.exportResult.error), 8000);
    }
    updateControls();
    updateStatusLabel();
}

void MainWindow::onOpenResharpen(const QString& sessionDir)
{
    ResharpenDialog dialog(session_->timelapsesFolder(), sessionDir, this);
    connect(&dialog, &ResharpenDialog::profilesChanged, timelapsePanel_, &TimelapsePanel::refreshProfiles);
    dialog.exec();
}

void MainWindow::saveGuidingSettings()
{
    if (!guidingPanel_) return;
    QSettings saved;
    saved.setValue("guiding/ascomDriver", ascomDriver_);
    saved.setValue("guiding/mountKind", guidingPanel_->mountKind() == GuidingPanel::MountKind::Ascom ? 1 : 0);
    saved.setValue("guiding/tolerancePercent", guidingPanel_->tolerancePercent());
    saved.setValue("guiding/giveUpPercent", guidingPanel_->giveUpPercent());
    saved.setValue("guiding/aggressivenessPercent", guidingPanel_->aggressivenessPercent());
    saved.setValue("guiding/maxPulseMs", guidingPanel_->maxPulseMs());
    saved.setValue("guiding/lostAfterSeconds", guidingPanel_->lostAfterSeconds());
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
    auto* resharpen = menu->addAction("Re-sharpen time-lapse…");
    resharpen->setObjectName("resharpenAction");
    connect(resharpen, &QAction::triggered, this, [this] { onOpenResharpen(); });
    auto* folder = menu->addAction("Output folder…");
    folder->setObjectName("outputFolderAction");
    connect(folder, &QAction::triggered, this, &MainWindow::onOutputFolder);
    auto* phone = menu->addAction("Status on your phone…");
    phone->setObjectName("remoteStatusAction");
    phone->setToolTip("Show what SeleneWatch is doing on a read-only page for your phone's browser, on the local network");
    connect(phone, &QAction::triggered, this, &MainWindow::openRemoteStatus);
    menu->addSeparator();
    auto* lastReport = menu->addAction("Open last nightly report");
    lastReport->setToolTip("Open the most recently finished monitoring report in your browser.");
    connect(lastReport, &QAction::triggered, this, [this] {
        QFileInfo newest;
        for (Target target : {Target::Moon, Target::Sky})
        {
            const QDir parent(eventsFolder(session_->outputRoot(), target));
            for (const auto& watch : parent.entryList({"session-*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
            {
                const QFileInfo file(parent.filePath(watch + "/report.html"));
                if (!file.isFile()) continue;
                if (!newest.exists() || file.lastModified() > newest.lastModified()) newest = file;
                break;
            }
        }
        if (!newest.exists() || !QDesktopServices::openUrl(QUrl::fromLocalFile(newest.absoluteFilePath())))
            showMessage("No nightly report could be opened in the selected output folder.");
    });
    reportOffer_ = new QWidget(this);
    auto* reportRow = new QHBoxLayout(reportOffer_);
    reportRow->setContentsMargins(0, 0, 0, 0);
    auto* openReport = new QPushButton("Open nightly report", reportOffer_);
    auto* later = new QPushButton("Later", reportOffer_);
    openReport->setObjectName("openNightlyReport");
    later->setObjectName("deferNightlyReport");
    openReport->setToolTip("Open the finished monitoring report in your browser.");
    later->setToolTip("Dismiss this offer. The report remains in the Session menu.");
    reportRow->addWidget(openReport);
    reportRow->addWidget(later);
    statusBar()->addPermanentWidget(reportOffer_);
    reportOffer_->hide();
    connect(openReport, &QPushButton::clicked, this, [this] {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(reportOffer_->property("reportPath").toString())))
            showMessage("The nightly report could not be opened in the browser.");
    });
    connect(openReport, &QPushButton::clicked, reportOffer_, &QWidget::hide);
    connect(later, &QPushButton::clicked, reportOffer_, &QWidget::hide);

    auto* events = menu->addAction("Open the output folder");
    events->setObjectName("openEventsAction");
    events->setToolTip("Events (moon, sky), recordings, and time-lapses");
    connect(events, &QAction::triggered, this, [this] {
        QDir().mkpath(session_->outputRoot());
        QDesktopServices::openUrl(QUrl::fromLocalFile(session_->outputRoot()));
    });

    // Record, beside Save now: every frame until pressed again or frame limit reached (Feature-14 / Feature-17).
    // While recording it is a red Stop button showing the recording's length (Feature-33).
    recordButton_ = new QPushButton("Record", this);
    recordButton_->setObjectName("recordButton");
    recordButton_->setIcon(recordMark(false));
    recordButton_->setToolTip(kRecordTip);
    // Wide enough for "Stop 0:00:00", so the row does not move as the time grows.
    recordButton_->setMinimumWidth(recordButton_->fontMetrics().horizontalAdvance("Stop 0:00:00")
        + recordButton_->iconSize().width() + 40);
    recordFramesSpinBox_ = new QSpinBox(this);
    recordFramesSpinBox_->setObjectName("recordFramesSpinBox");
    recordFramesSpinBox_->setRange(0, 10000000);
    recordFramesSpinBox_->setValue(0);
    recordFramesSpinBox_->setSpecialValueText("Unlimited");
    recordFramesSpinBox_->setSuffix(" frames");
    recordFramesSpinBox_->setToolTip("Optional frame count limit for manual recording (Unlimited = record until stopped).");
    const int saveIdx = ui_->controlsLayout->indexOf(ui_->saveNowButton);
    ui_->controlsLayout->insertWidget(saveIdx + 1, recordButton_);
    ui_->controlsLayout->insertWidget(saveIdx + 2, recordFramesSpinBox_);
    connect(recordButton_, &QPushButton::clicked, this, &MainWindow::onRecordClicked);

    session_->setSite(savedSite());
    const QSettings saved;
    session_->setClockCheck(true, saved.value("time/ntp", false).toBool()
        ? saved.value("time/ntpServer", "pool.ntp.org").toString().toStdString() : std::string());
}

void MainWindow::setupHelpMenu()
{
    releaseChecker_ = new ReleaseChecker(SELENEWATCH_RELEASE, this);
    auto* updateNotice = new QToolButton(menuBar());
    updateNotice->setObjectName("updateNoticeButton");
    updateNotice->setCursor(Qt::PointingHandCursor);
    updateNotice->setToolTip("A newer SeleneWatch release is available. Open GitHub releases to download it.");
    updateNotice->setStyleSheet("QToolButton { background: palette(highlight); color: palette(highlighted-text); "
        "border: none; border-radius: 7px; padding: 4px 10px; font-weight: bold; }");
    menuBar()->setCornerWidget(updateNotice, Qt::TopRightCorner);
    updateNotice->hide();
    connect(releaseChecker_, &ReleaseChecker::newerReleaseAvailable, this, [updateNotice](const QString& tag) {
        updateNotice->setText(QString("New version %1 available").arg(tag));
        updateNotice->show();
    });
    connect(updateNotice, &QToolButton::clicked, this, [] {
        QDesktopServices::openUrl(ReleaseChecker::releasesUrl());
    });

    auto* menu = menuBar()->addMenu("Help");
    auto* manual = menu->addAction("User Manual");
    manual->setObjectName("userManualAction");
    manual->setShortcut(QKeySequence::HelpContents);
    manual->setToolTip("Open the packaged SeleneWatch User Manual.");
    connect(manual, &QAction::triggered, this, [this] { showManual(); });

    auto* whatsThis = QWhatsThis::createAction(this);
    whatsThis->setObjectName("whatsThisAction");
    whatsThis->setText("What's This? (click a control)");
    whatsThis->setToolTip("Then click any control to read what it does. The ⓘ beside a control's name shows the same text.");
    menu->addAction(whatsThis);
    auto* tips = menu->addAction("Tips");
    tips->setObjectName("tipsAction");
    tips->setToolTip("Short tips on features and workflows you may not have tried yet.");
    connect(tips, &QAction::triggered, this, &MainWindow::showTips);

    auto* acknowledgements = menu->addAction("Acknowledgements");
    acknowledgements->setObjectName("acknowledgementsAction");
    connect(acknowledgements, &QAction::triggered, this, &MainWindow::showAcknowledgements);

    menu->addSeparator();
    auto* about = menu->addAction("About SeleneWatch");
    about->setObjectName("aboutAction");
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
    auto* aboutQt = menu->addAction("About Qt");
    aboutQt->setObjectName("aboutQtAction");
    connect(aboutQt, &QAction::triggered, this, [this] { QMessageBox::aboutQt(this); });
}

void MainWindow::showManual(const QString& section)
{
    if (!manualDialog_) manualDialog_ = new ManualDialog(this);
    manualDialog_->openSection(section);
}

void MainWindow::showTips()
{
    if (!tipsDialog_)
    {
        tipsDialog_ = new TipsDialog(this, [this](const QString& section) {
            showManual(section);
        });
    }
    tipsDialog_->open();
}

void MainWindow::maybeShowStartupTips()
{
    if (TipsDialog::wantedAtStartup()) showTips();
}

void MainWindow::checkForUpdates()
{
    releaseChecker_->check();
}

QString MainWindow::acknowledgementsMarkdown()
{
    QFile file(":/ACKNOWLEDGEMENTS.md");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void MainWindow::showAcknowledgements()
{
    if (!acknowledgementsDialog_) acknowledgementsDialog_ = new AcknowledgementsDialog(this);
    acknowledgementsDialog_->show();
    acknowledgementsDialog_->raise();
    acknowledgementsDialog_->activateWindow();
}

void MainWindow::showAbout()
{
    if (!aboutDialog_)
    {
        aboutDialog_ = new AboutDialog(this, SELENEWATCH_RELEASE, SELENEWATCH_REVISION,
            [this] { showAcknowledgements(); },
            [this] { QMessageBox::aboutQt(this); });
    }
    aboutDialog_->show();
    aboutDialog_->raise();
    aboutDialog_->activateWindow();
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
    // Feature-32: dawn and the Moon's altitude follow the site's position.
    updateSessionEndRow(std::chrono::system_clock::now());
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

void MainWindow::onOutputFolder()
{
    if (session_->isRunning() || session_->timeLapseFinalizing())
    {
        showMessage("Stop the stream before changing the output folder.", 4000);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Output folder");
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(
        "Everything SeleneWatch saves goes into subfolders of this single folder:\n"
        "events/moon/, events/sky/, recordings/, and timelapses/.\n"
        "Changing the folder affects new output only; existing files are not moved.",
        &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* row = new QHBoxLayout;
    auto* path = new QLineEdit(QDir::toNativeSeparators(session_->outputRoot()), &dialog);
    path->setObjectName("outputFolderPath");
    path->setMinimumWidth(380);
    auto* choose = new QPushButton("Choose…", &dialog);
    auto* standard = new QPushButton("Default", &dialog);
    standard->setToolTip(QDir::toNativeSeparators(defaultOutputFolder()));
    row->addWidget(path, 1);
    row->addWidget(choose);
    row->addWidget(standard);
    layout->addLayout(row);
    auto* infoLabel = new QLabel(&dialog);
    infoLabel->setObjectName("outputFolderInfo");
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);
    const auto refreshInfo = [&] {
        const QString candidate = path->text().trimmed().isEmpty()
            ? defaultOutputFolder()
            : QDir::cleanPath(QDir::fromNativeSeparators(path->text().trimmed()));
        const auto free = freeBytesAt(candidate);
        QString msg = free
            ? QString("Free disk space: %1 GiB").arg(static_cast<double>(*free) / kGiB, 0, 'f', 1)
            : QString("Free disk space: unknown (drive/location not found)");
        if (isCloudSyncedPath(candidate))
        {
            msg += "\nWarning: this folder appears to be inside a cloud-synced folder (OneDrive/Dropbox/Drive). "
                   "High-rate FITS/SER capture can saturate uploads or lock files.";
        }
        infoLabel->setText(msg);
    };
    refreshInfo();
    connect(path, &QLineEdit::textChanged, &dialog, refreshInfo);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(choose, &QPushButton::clicked, &dialog, [&dialog, path] {
        const QString chosen = QFileDialog::getExistingDirectory(&dialog, "Output folder", path->text());
        if (!chosen.isEmpty()) path->setText(QDir::toNativeSeparators(chosen));
    });
    connect(standard, &QPushButton::clicked, &dialog, [path] {
        path->setText(QDir::toNativeSeparators(defaultOutputFolder()));
    });
    if (dialog.exec() != QDialog::Accepted) return;
    QString error;
    if (!applyOutputFolder(QDir::fromNativeSeparators(path->text().trimmed()), error))
        QMessageBox::warning(this, "Output folder", error);
}

bool MainWindow::applyOutputFolder(const QString& folder, QString& error)
{
    if (session_->isRunning() || session_->timeLapseFinalizing())
    {
        error = "Stop the stream before changing the output folder.";
        return false;
    }
    const QString def = QDir::cleanPath(defaultOutputFolder());
    const QString target = folder.trimmed().isEmpty() ? def : QDir::cleanPath(QDir(folder.trimmed()).absolutePath());
    const OutputFolderCheck check = checkOutputFolder(target, 0, true);
    if (!check.ok)
    {
        error = QString("%1 The output folder is unchanged.").arg(check.error);
        return false;
    }
    if (!customOutputRoot_)
    {
        const QString stored = target.compare(def, Qt::CaseInsensitive) == 0 ? QString() : target;
        QSettings saved;
        saved.setValue("output/folder", stored);
        saved.setValue("settingsVersion", kCurrentSettingsVersion);
        saved.sync();
    }
    session_->setOutputRoot(target);
    QString msg = QString("Output folder set to %1 (for new output).").arg(QDir::toNativeSeparators(target));
    if (isCloudSyncedPath(target))
        msg += " Warning: cloud-synced folders can interfere with high-rate capture.";
    showMessage(msg, 6000);
    return true;
}

bool MainWindow::recreateOutputFolder(QString& error)
{
    const uint64_t reserve = displayPanel_ ? displayPanel_->captureSettings().diskReserveBytes : 0;
    const QString root = session_->outputRoot();
    const OutputFolderCheck recreated = checkOutputFolder(root, reserve, true);
    if (!recreated.ok)
    {
        error = recreated.error;
        showMessage(error, 8000);
        return false;
    }
    if (recreated.belowReserve && recreated.freeBytes)
    {
        showMessage(QString("Recreated %1. Warning: %2 GiB free, below the disk reserve of %3 GiB.")
            .arg(QDir::toNativeSeparators(root))
            .arg(static_cast<double>(*recreated.freeBytes) / kGiB, 0, 'f', 1)
            .arg(static_cast<double>(reserve) / kGiB, 0, 'f', 0), 8000);
    }
    else
    {
        showMessage(QString("Recreated output folder %1.").arg(QDir::toNativeSeparators(root)), 6000);
    }
    return true;
}

bool MainWindow::ensureOutputFolderReady(const QString& actionName)
{
    const uint64_t reserve = displayPanel_ ? displayPanel_->captureSettings().diskReserveBytes : 0;
    const OutputFolderCheck check = checkOutputFolder(session_->outputRoot(), reserve, false);
    if (check.ok)
    {
        if (check.belowReserve && check.freeBytes)
        {
            showMessage(QString("Warning: %1 GiB free on %2, below the disk reserve of %3 GiB.")
                .arg(static_cast<double>(*check.freeBytes) / kGiB, 0, 'f', 1)
                .arg(QDir::toNativeSeparators(session_->outputRoot()))
                .arg(static_cast<double>(reserve) / kGiB, 0, 'f', 0), 8000);
        }
        return true;
    }
    showMessage(check.error, 8000);
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen")
    {
        return false;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Output folder unavailable");
    box.setText(QString("Cannot start %1: %2").arg(actionName, check.error));
    if (session_->isRunning())
        box.setInformativeText("You can recreate the current folder in place, or stop the stream first to choose a different folder.");
    QPushButton* recreateBtn = nullptr;
    if (check.folderMissing && !check.mountMissing)
        recreateBtn = box.addButton("Recreate folder", QMessageBox::AcceptRole);
    QPushButton* chooseBtn = box.addButton("Choose folder…", QMessageBox::ActionRole);
    if (session_->isRunning())
    {
        chooseBtn->setEnabled(false);
        chooseBtn->setToolTip("Stop the stream first to choose a different output folder.");
    }
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (recreateBtn && box.clickedButton() == recreateBtn)
    {
        QString error;
        if (!recreateOutputFolder(error))
        {
            QMessageBox::warning(this, "Output folder", error);
            return false;
        }
        return true;
    }
    if (box.clickedButton() == chooseBtn)
    {
        onOutputFolder();
        return checkOutputFolder(session_->outputRoot(), reserve, false).ok;
    }
    return false;
}

void MainWindow::checkLegacyEventsNotice()
{
    if (customOutputRoot_ || qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen"
        || qEnvironmentVariableIsSet("SELENEWATCH_SETTINGS_NAME"))
    {
        return;
    }
    QSettings saved;
    if (saved.value("output/legacyNoticeShown", false).toBool()) return;
    const QString legacyEvents = QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).filePath("events"));
    const QDir legacyDir(legacyEvents);
    const bool hasLegacyEvents = legacyDir.exists()
        && !legacyDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot).isEmpty()
        && legacyEvents.compare(QDir::cleanPath(session_->outputRoot()), Qt::CaseInsensitive) != 0;
    const QString legacyRec = saved.value("output/legacyRecordingFolder").toString();
    if (!hasLegacyEvents && legacyRec.isEmpty())
    {
        saved.setValue("output/legacyNoticeShown", true);
        return;
    }
    saved.setValue("output/legacyNoticeShown", true);
    QString text = QString("New SeleneWatch output is now saved in %1.")
        .arg(QDir::toNativeSeparators(session_->outputRoot()));
    if (hasLegacyEvents)
        text += QString("\n\nExisting files in %1 were left where they are.").arg(QDir::toNativeSeparators(legacyEvents));
    if (!legacyRec.isEmpty())
        text += QString("\n\nPrevious recordings in %1 were left where they are.").arg(QDir::toNativeSeparators(legacyRec));
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle("Output folder location");
    box.setText(text);
    QPushButton* openBtn = hasLegacyEvents ? box.addButton("Open old folder", QMessageBox::ActionRole) : nullptr;
    box.addButton(QMessageBox::Ok);
    box.exec();
    if (openBtn && box.clickedButton() == openBtn)
        QDesktopServices::openUrl(QUrl::fromLocalFile(legacyEvents));
}

void MainWindow::onRecordClicked()
{
    const bool recording = session_->recordingStatus().active;
    if (!recording && !ensureOutputFolderReady("recording"))
    {
        updateControls();
        return;
    }
    const uint64_t maxFrames = (!recording && recordFramesSpinBox_)
        ? static_cast<uint64_t>(recordFramesSpinBox_->value())
        : 0;
    if (!session_->requestRecording(!recording, maxFrames))
        showMessage("Start Preview or Monitoring before recording.", 4000);
    updateControls();
}

void MainWindow::onRecordingChanged()
{
    const auto status = session_->recordingStatus();
    if (!status.active && !status.endReason.empty())
        showMessage(QString("Recording %1 ended: %2. %3 frames.")
            .arg(status.name, QString::fromStdString(status.endReason)).arg(status.frames));
    else if (status.active)
        showMessage(QString("Recording %1to %2.")
            .arg(status.frameLimit > 0 ? QString("up to %1 frames ").arg(status.frameLimit) : QString("every frame "))
            .arg(QDir::toNativeSeparators(status.parentFolder)));
    updateControls();
    updateStatusLabel();
}

// Feature-33: Record turns into a red Stop button that shows the recording's
// length; the length is stream time, as in the RECORDING line.
void MainWindow::updateRecordButton()
{
    const auto status = session_->recordingStatus();
    recordButton_->setText(status.active ? "Stop " + recordingLength(status.streamSeconds) : QString("Record"));
    if (status.active == recordButtonStops_) return;
    recordButtonStops_ = status.active;
    recordButton_->setIcon(recordMark(status.active));
    recordButton_->setToolTip(status.active ? "Stops the recording. The time is the recording's length." : kRecordTip);
    recordButton_->setStyleSheet(status.active
        ? "QPushButton { background:#b3261e; color:white; font-weight:bold; border:1px solid #7a1912;"
          " border-radius:4px; padding:3px 10px; }"
          " QPushButton:hover { background:#c8352c; } QPushButton:pressed { background:#8f1e17; }"
        : QString());
}

QString MainWindow::recordingReadout() const
{
    const auto status = session_->recordingStatus();
    if (status.active)
    {
        if (status.frameLimit > 0)
        {
            return QString("● RECORDING %1/%2 frames (%3 s, %4 GB at %5 MB/s).")
                .arg(status.frames).arg(status.frameLimit).arg(status.streamSeconds, 0, 'f', 0)
                .arg(static_cast<double>(status.bytes) / 1e9, 0, 'f', 2).arg(status.megabytesPerSecond, 0, 'f', 0);
        }
        return QString("● RECORDING %1 s, %2 frames, %3 GB written at %4 MB/s.")
            .arg(status.streamSeconds, 0, 'f', 0).arg(status.frames)
            .arg(static_cast<double>(status.bytes) / 1e9, 0, 'f', 2).arg(status.megabytesPerSecond, 0, 'f', 0);
    }
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
    session_->guider().setGiveUpTolerance(guidingPanel_->giveUpPercent() / 100.0 * frameShortSide_);
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
    // Feature-32: guiding that stopped by itself ends a guided session.
    if (sessionRun_ && all.guide.unplannedStops > sessionRun_->guideStops)
    {
        sessionRun_->guideStops = all.guide.unplannedStops;
        endSession("guiding stopped: " + QString::fromStdString(all.guide.message));
    }
    checkTrackingStop(all.guide);
    updateSessionEndRow(std::chrono::system_clock::now());
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
    modeLabel_ = new ElidedLabel("STOPPED", Qt::ElideRight, this);
    modeLabel_->setObjectName("captureModeLabel");
    modeLabel_->setMinimumWidth(84);
    modeLabel_->setAlignment(Qt::AlignCenter);
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

void MainWindow::setupPreviewZoomBar()
{
    auto* zoomBar = new QWidget(ui_->centralWidget);
    zoomBar->setObjectName("previewZoomBar");
    auto* row = new QHBoxLayout(zoomBar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->addWidget(new QLabel("View:", zoomBar));

    zoomFitButton_ = new QToolButton(zoomBar);
    zoomFitButton_->setObjectName("previewZoomFit");
    zoomFitButton_->setText("Fit");
    zoomFitButton_->setCheckable(true);
    zoomFitButton_->setChecked(true);
    zoomFitButton_->setToolTip("Fit the live frame to the preview window.");

    zoom100Button_ = new QToolButton(zoomBar);
    zoom100Button_->setObjectName("previewZoom100");
    zoom100Button_->setText("100%");
    zoom100Button_->setCheckable(true);
    zoom100Button_->setToolTip("Show actual sensor pixels (1:1).");

    zoom200Button_ = new QToolButton(zoomBar);
    zoom200Button_->setObjectName("previewZoom200");
    zoom200Button_->setText("200%");
    zoom200Button_->setCheckable(true);
    zoom200Button_->setToolTip("Zoom to 200% of actual pixels.");

    zoomReadoutLabel_ = new QLabel("Fit", zoomBar);
    zoomReadoutLabel_->setObjectName("previewZoomLabel");

    previewHintLabel_ = new ElidedLabel("Mouse wheel: zoom · Drag: pan", Qt::ElideRight, zoomBar);
    previewHintLabel_->setObjectName("previewHintLabel");
    previewHintLabel_->setStyleSheet("color: #888888;");
    previewHintLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    histogramToggleButton_ = new QToolButton(zoomBar);
    histogramToggleButton_->setObjectName("previewHistogramToggle");
    histogramToggleButton_->setText("Histogram");
    histogramToggleButton_->setCheckable(true);
    histogramToggleButton_->setToolTip("Show or hide the live native-ADU histogram in the right sidebar.");

    focusViewButton_ = new QToolButton(zoomBar);
    focusViewButton_->setObjectName("previewFocusView");
    focusViewButton_->setText("Focus view");
    focusViewButton_->setCheckable(true);
    focusViewButton_->setToolTip("Hide all side panels to maximize the live camera view (F11). Click again to restore.");

    row->addWidget(zoomFitButton_);
    row->addWidget(zoom100Button_);
    row->addWidget(zoom200Button_);
    row->addSpacing(6);
    row->addWidget(zoomReadoutLabel_);
    row->addSpacing(8);
    row->addWidget(previewHintLabel_, 1);
    row->addWidget(histogramToggleButton_);
    row->addWidget(focusViewButton_);

    const int previewIndex = ui_->verticalLayout->indexOf(ui_->previewWidget);
    ui_->verticalLayout->insertWidget(std::max(0, previewIndex), zoomBar);

    connect(zoomFitButton_, &QToolButton::clicked, this, [this] { ui_->previewWidget->setFitToWindow(); });
    connect(zoom100Button_, &QToolButton::clicked, this, [this] { ui_->previewWidget->setZoom(1.0); });
    connect(zoom200Button_, &QToolButton::clicked, this, [this] { ui_->previewWidget->setZoom(2.0); });
    connect(ui_->previewWidget, &PreviewWidget::zoomChanged, this, &MainWindow::updateZoomControls);
}

void MainWindow::updateZoomControls(double effectiveZoom, bool fitToWindow)
{
    if (!zoomFitButton_ || !zoom100Button_ || !zoom200Button_ || !zoomReadoutLabel_) return;
    const QSignalBlocker bFit(zoomFitButton_);
    const QSignalBlocker b100(zoom100Button_);
    const QSignalBlocker b200(zoom200Button_);
    zoomFitButton_->setChecked(fitToWindow);
    zoom100Button_->setChecked(!fitToWindow && std::abs(effectiveZoom - 1.0) < 0.01);
    zoom200Button_->setChecked(!fitToWindow && std::abs(effectiveZoom - 2.0) < 0.01);
    const int pct = std::max(1, static_cast<int>(std::lround(effectiveZoom * 100.0)));
    zoomReadoutLabel_->setText(fitToWindow ? QString("Fit (%1%)").arg(pct) : QString("%1%").arg(pct));
}

void MainWindow::setupBlipControls()
{
    const QSignalBlocker blocked(ui_->blipKindComboBox);
    ui_->blipKindComboBox->clear();
    ui_->blipKindComboBox->addItem("Bright flash", static_cast<int>(BlipKind::BrightFlash));
    ui_->blipKindComboBox->addItem("Dark transit", static_cast<int>(BlipKind::DarkTransit));
    ui_->blipKindComboBox->addItem("Moving streak (meteor)", static_cast<int>(BlipKind::MovingStreak));
    ui_->blipKindComboBox->addItem("Blinking mover (aircraft)", static_cast<int>(BlipKind::BlinkingMover));
    ui_->blipKindComboBox->addItem("Lightning", static_cast<int>(BlipKind::Lightning));
    ui_->blipKindComboBox->addItem("ISS silhouette (transit)", static_cast<int>(BlipKind::IssSilhouette));
    connect(ui_->blipKindComboBox, &QComboBox::currentIndexChanged, this, &MainWindow::onBlipKindChanged);
}

BlipKind MainWindow::selectedBlipKind() const
{
    const QVariant kindData = ui_->blipKindComboBox->currentData();
    return kindData.isValid() ? static_cast<BlipKind>(kindData.toInt()) : BlipKind::BrightFlash;
}

void MainWindow::onBlipKindChanged()
{
    const BlipKind kind = selectedBlipKind();
    ui_->blipFwhmSpinBox->setEnabled(kind != BlipKind::Lightning);
    // Feature-48: the ISS is sized by its length and darkens by a share of the light.
    const bool iss = kind == BlipKind::IssSilhouette;
    ui_->blipFwhmLabel->setText(iss ? "Length:" : "FWHM:");
    ui_->blipFwhmSpinBox->setToolTip(iss
        ? "The station's length in original frame pixels. The default is the ISS about 45° up (41″ long) at the "
          "image scale of Session → Site and instrument, or 120 px without one. Overhead it is about 54″."
        : "Full width at half maximum of the Gaussian, in original frame pixels");
    ui_->blipAmplitudeLabel->setText(iss ? "Blocks:" : "Peak:");
    ui_->blipAmplitudeSpinBox->setSuffix(iss ? " %" : " ADU");
    ui_->blipAmplitudeSpinBox->setToolTip(iss
        ? "The share of the light the station's outline blocks. Against the sunlit Moon it is nearly black."
        : "Change in native sample units (ADU): 12-bit units for a 12-bit frame. Values clamp at the stream's limits.");
    switch (kind)
    {
    case BlipKind::BrightFlash:
        ui_->blipFwhmSpinBox->setValue(3.0);
        ui_->blipAmplitudeSpinBox->setValue(std::min(100.0, ui_->blipAmplitudeSpinBox->maximum()));
        ui_->blipFramesSpinBox->setValue(2);
        break;
    case BlipKind::DarkTransit:
        ui_->blipFwhmSpinBox->setValue(10.0);
        ui_->blipAmplitudeSpinBox->setValue(std::min(120.0, ui_->blipAmplitudeSpinBox->maximum()));
        ui_->blipFramesSpinBox->setValue(6);
        break;
    case BlipKind::MovingStreak:
        ui_->blipFwhmSpinBox->setValue(2.5);
        ui_->blipAmplitudeSpinBox->setValue(std::min(120.0, ui_->blipAmplitudeSpinBox->maximum()));
        ui_->blipFramesSpinBox->setValue(6);
        break;
    case BlipKind::BlinkingMover:
        ui_->blipFwhmSpinBox->setValue(3.0);
        ui_->blipAmplitudeSpinBox->setValue(std::min(140.0, ui_->blipAmplitudeSpinBox->maximum()));
        ui_->blipFramesSpinBox->setValue(55);
        break;
    case BlipKind::Lightning:
        ui_->blipAmplitudeSpinBox->setValue(std::min(50.0, ui_->blipAmplitudeSpinBox->maximum()));
        ui_->blipFramesSpinBox->setValue(2);
        break;
    case BlipKind::IssSilhouette:
    {
        const std::optional<double> scale = savedSite().arcsecPerPixel();
        ui_->blipFwhmSpinBox->setValue(scale && *scale > 0 ? std::clamp(41.0 / *scale, 4.0, 1000.0) : 120.0);
        ui_->blipAmplitudeSpinBox->setValue(95.0);
        ui_->blipFramesSpinBox->setValue(4);
        break;
    }
    }
    updateBlipReadout();
}

void MainWindow::showMessage(const QString& text, int timeoutMs)
{
    messageLabel_->setText(text);
    // Feature-47: the status page shows the last message, after it has gone.
    if (!text.isEmpty())
    {
        lastMessage_ = text;
        lastMessageTime_ = QDateTime::currentDateTimeUtc();
    }
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
    auto* scroll=new QScrollArea(dock);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(panel);
    dock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea,dock);
    dock->hide();
    QAction* toggle=dock->toggleViewAction();
    toggle->setVisible(false);
    viewMenu_->addAction(toggle);
    connect(panel,&CameraFamilyPanel::applyRequested,this,&MainWindow::onApplyCamera);
    connect(panel,&CameraFamilyPanel::liveControlsRequested,this,&MainWindow::onApplyLiveCamera);
    connect(panel,&CameraFamilyPanel::availabilityChanged,this,&MainWindow::updateControls);
    if (CameraSettingsEditor* controls = panel->controls())
    {
        controls->setExposureRange(watch_ == Watch::MoonShadows ? ExposureRange::Planetary
            : watch_ == Watch::SkySlow ? ExposureRange::Sky : ExposureRange::Fast);
        connect(controls, &CameraSettingsEditor::roiDrawingChanged, this, [this, panel](bool drawing) {
            if (drawing && selectedCamera() != panel) return;
            chooseDrawing(drawing ? Drawing::CameraRoi : Drawing::None);
            if (drawing) showMessage("Drag a rectangle on the preview to set the camera ROI.", 5000);
        });
        connect(controls, &CameraSettingsEditor::roiApplyRequested, this, [this, panel](const QString& note) {
            if (selectedCamera() != panel) return;
            applyCameraRoiChange(note);
        });
        connect(controls, &CameraSettingsEditor::roiAdjusted, this, [this, panel](const QString& note) {
            if (selectedCamera() != panel || note.isEmpty()) return;
            showMessage(note, 5000);
        });
    }
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
    // Feature-26: what auto-tune may do follows the stream's state.
    updateAutoTune();
    const bool running = session_->isRunning();
    const bool recording = recordingSelected();
    const bool monitoring = session_->mode()==Session::Mode::Monitoring;
    const bool timeLapsing = session_->mode()==Session::Mode::TimeLapse;
    const bool timeLapseFinalizing = session_->timeLapseFinalizing();
    const bool busy = running || timeLapseFinalizing;
    const bool pending = session_->modeChangePending();

    CameraFamilyPanel* selected = selectedCamera();
    const QString sourceKey = ui_->sourceComboBox->currentData().toString();
    const bool chosen = !sourceKey.isEmpty();
    const bool syntheticSelected = sourceKey == "synthetic";
    ui_->startButton->setEnabled(!busy && chosen && (!selected || selected->hasCamera()));
    ui_->startButton->setVisible(!running);
    if (connectAtStartup_)
    {
        connectAtStartup_->setVisible(selected != nullptr);
        connectAtStartup_->setEnabled(!busy && selected);
        const QSignalBlocker blocked(connectAtStartup_);
        connectAtStartup_->setChecked(selected && startupCamera_ == selected->sourceKey());
    }
    if (recordingFileGroup_) recordingFileGroup_->setVisible(recording);
    if (testingAction_) testingAction_->setEnabled(!busy);
    ui_->stopButton->setEnabled(running);
    ui_->stopButton->setVisible(running);
    ui_->monitoringButton->setEnabled(running && !timeLapsing && !timeLapseFinalizing && !session_->sourceFinished() && !pending);
    ui_->monitoringButton->setVisible(running && !timeLapsing && !timeLapseFinalizing);
    ui_->monitoringButton->setText(monitoring?"Stop monitoring":"Start monitoring");
    ui_->saveNowButton->setEnabled(running && monitoring && !pending && !session_->sourceFinished() && !session_->manualSavePending());
    ui_->saveNowButton->setVisible(running && monitoring);
    if (recordButton_)
    {
        updateRecordButton();
        recordButton_->setEnabled(session_->recordingStatus().active
            || (running && !timeLapsing && !timeLapseFinalizing && !pending && !session_->sourceFinished()));
    }
    if (recordFramesSpinBox_)
        recordFramesSpinBox_->setEnabled(!session_->recordingStatus().active && !timeLapsing && !timeLapseFinalizing);
    ui_->injectFlashButton->setEnabled(running && !timeLapsing && !timeLapseFinalizing && synthetic_ != nullptr);
    ui_->injectFlashButton->setVisible(testingTools_ && syntheticSelected);

    ui_->sourceComboBox->setEnabled(!busy);
    const bool watchEditable = !busy || (!timeLapseFinalizing && session_->canChangePreviewWatch());
    if (detectionPanel_)
    {
        detectionPanel_->setTargetEditable(watchEditable);
        detectionPanel_->setWatchTimelapseEditable(!monitoring);
        const bool tlTransits = timeLapsing && timelapsePanel_ && timelapsePanel_->settings().watchTransits;
        QString detail;
        if (tlTransits)
        {
            switch (session_->timeLapseStatus().transitWatch)
            {
            case TransitWatchState::Watching: detail = "watching between bursts"; break;
            case TransitWatchState::Saving: detail = "saving transit clip"; break;
            case TransitWatchState::Capturing: detail = "paused during burst capture"; break;
            case TransitWatchState::Stacking: detail = "paused during stacking"; break;
            case TransitWatchState::Off:
            case TransitWatchState::Finished: break;
            }
        }
        detectionPanel_->setTimeLapseState(timeLapsing, tlTransits, detail);
    }
    ui_->openFileButton->setEnabled(!busy && recording);
    ui_->loopCheckBox->setEnabled(!busy && recording);
    ui_->loopSecondsSpinBox->setEnabled(!busy && recording && ui_->loopCheckBox->isChecked());

    // The speed may change while a recording plays.
    ui_->speedComboBox->setEnabled(recording);
    ui_->targetFpsSpinBox->setEnabled(!selected);
    if (fpsGroup_) fpsGroup_->setVisible(!selected && chosen);
    if (recordingPlaybackGroup_) recordingPlaybackGroup_->setVisible(recording);
    if (playbackBar_) playbackBar_->setVisible(!selected && chosen);
    if (displayPanel_) displayPanel_->setRunning(running, recording || selected);
    for (const auto& family : cameraFamilies_)
    {
        const bool thisFamily = family.panel == selected;
        family.dock->setEnabled(thisFamily);
        family.panel->setRunning(running && thisFamily, running && (monitoring || timeLapsing || pending));
    }
    if (guidingPanel_) guidingPanel_->setAvailable(running, ui_->previewWidget->selectedPosition().has_value());
    if (timelapsePanel_)
    {
        const bool hasLock = session_->guider().status().lock.state == LockState::Locked;
        timelapsePanel_->setStreamInfo(running, monitoring || pending, session_->recordingStatus().active, timeLapsing,
            measuredFps_, lastFrameWidth_, lastFrameHeight_, lastFrameBpp_, hasLock, timeLapseFinalizing);
        timelapsePanel_->setTransitOptions(target() == Target::Moon,
            hasSavedDetectionSettings(Watch::MoonShadows), watchEditable);
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
    if (guidingPanel_) guidingPanel_->setLogFailed(writer.guidingWriteFailures > 0);
}

void MainWindow::updateStreamReadout()
{
    updateBufferReadout();
    updateBlipReadout();
    if (session_->recordingStatus().active) updateRecordButton();
    if (session_->recordingStatus().active || session_->schedule() || session_->watchTimelapseStatus().active) updateStatusLabel();

    const auto now = std::chrono::steady_clock::now();
    const uint64_t acquired = session_->acquiredFrames();
    const double elapsed = std::chrono::duration<double>(now - lastFpsSampleTime_).count();

    if (elapsed > 0.0)
    {
        const double fps = static_cast<double>(acquired - lastAcquiredSample_) / elapsed;
        if (fps > 0.1) measuredFps_ = fps;
        fpsLabel_->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
    }
    if (session_->mode() == Session::Mode::TimeLapse && timelapsePanel_)
        timelapsePanel_->updateStatus(session_->timeLapseStatus());

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
        const auto m = session_->mode();
        statusLabel_->setText(m == Session::Mode::Monitoring ? "Status: Monitoring"
            : m == Session::Mode::TimeLapse ? "Status: Time-lapse" : "Status: Preview");
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
    const bool timeLapsing=session_->mode()==Session::Mode::TimeLapse;
    const bool running=session_->isRunning();
    detectionPanel_->setMonitoring(monitoring);
    modeLabel_->setStyleSheet(modePillStyle(monitoring, timeLapsing, running));
    QString pill;
    QString tip;
    if (session_->previewWatchChangePending())
    {
        pill = "PREVIEW · Applying watch…";
        tip = "Applying the selected watch.";
    }
    else if (session_->modeChangePending())
    {
        pill = monitoring ? "Stopping monitoring…" : "Starting monitoring…";
        tip = monitoring ? "Finishing the current capture before returning to Preview."
                         : "Applying the capture boundary at the next frame.";
    }
    else if (!running)
    {
        pill = "STOPPED";
        tip = "Select an input and start Preview to tune it.";
    }
    else if (timeLapsing)
    {
        pill = timelapsePanel_->settings().watchTransits ? "TIME-LAPSE · Transits" : "TIME-LAPSE";
        tip = timelapsePanel_->settings().watchTransits
            ? "Transit watching between bursts and stacks."
            : "Stacking bursts into FITS. Flash detection is OFF.";
    }
    else if (monitoring)
    {
        pill = recordFailed_ ? "MONITORING · Storage error" : "MONITORING";
        tip = recordFailed_ ? "Recording failed. Check the reported storage error."
                            : "Event capture ON. Stop monitoring to return to Preview and tune camera settings.";
    }
    else
    {
        pill = "PREVIEW";
        tip = "Capture OFF. Tune the camera and detector, then start monitoring. Previously queued clips can finish writing.";
    }
    if (const QString recording = recordingReadout(); !recording.isEmpty())
    {
        pill += " · " + recording;
        tip += "\n" + recording;
    }
    if (const QString timelapse = watchTimelapseReadout(); !timelapse.isEmpty())
    {
        pill += " · " + timelapse;
        tip += "\n" + timelapse;
    }
    modeLabel_->setText(pill);
    modeLabel_->setToolTip(tip);
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
    const BlipKind kind = selectedBlipKind();
    const bool wholeFrame = (kind == BlipKind::Lightning);
    const auto centre = ui_->previewWidget->selectedPosition();
    const bool busy = session_->blipBusy();
    const bool ready = wholeFrame || centre.has_value();
    ui_->injectBlipButton->setEnabled(session_->isRunning() && !session_->sourceFinished() && ready && !busy);
    if (wholeFrame)
    {
        ui_->blipPositionLabel->setText("Whole frame (no pixel selection needed)");
    }
    else
    {
        ui_->blipPositionLabel->setText(centre
            ? QString("Centre: (%1, %2)").arg(centre->x()).arg(centre->y())
            : QString("Click the preview to place a flash"));
    }
    ui_->blipStatusLabel->setText(QString("Next peak: %1 ADU. %2")
        .arg(ui_->blipAmplitudeSpinBox->value(), 0, 'f', 1)
        .arg(busy ? "Injecting test frames; edits apply to the next flash."
                  : "Injected frames are marked as test data."));
    if (!wholeFrame && centre && latestDetection_.probeX == centre->x() && latestDetection_.probeY == centre->y()
        && latestDetection_.probeNoise > 0)
    {
        ui_->blipStatusLabel->setText(ui_->blipStatusLabel->text()
            + QString("  %1 sigma at centre (noise %2 ADU).")
                .arg(ui_->blipAmplitudeSpinBox->value() / latestDetection_.probeNoise, 0, 'f', 1)
                .arg(latestDetection_.probeNoise, 0, 'f', 2));
    }
    // Feature-31: the flashes watch does not test the sunlit side; a test
    // flash placed there is not caught, and the readout says why.
    const double ceiling = detectionPanel_ ? detectionPanel_->settings().ignoreAboveShare : 0;
    if (!wholeFrame && centre && ceiling > 0 && latestFrame_)
    {
        TuneArea pixel;
        pixel.width = latestFrame_->width;
        pixel.height = latestFrame_->height;
        pixel.inside.assign(static_cast<size_t>(pixel.width) * pixel.height, 0);
        if (centre->x() < pixel.width && centre->y() < pixel.height)
        {
            pixel.inside[static_cast<size_t>(centre->y()) * pixel.width + centre->x()] = 1;
            const auto brightness = measureBrightness(*latestFrame_, pixel, false);
            if (brightness && brightness->level > ceiling)
                ui_->blipStatusLabel->setText(ui_->blipStatusLabel->text()
                    + QString("  This pixel is at %1 % of full scale, above Ignore brighter than (%2 %): the flashes "
                        "watch does not test it.").arg(100 * brightness->level, 0, 'f', 0).arg(100 * ceiling, 0, 'f', 0));
        }
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
        if (CameraSettingsEditor* controls = camera->controls()) controls->alignRequestedRoi();
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
        source->setScene(target() == Target::Sky ? SyntheticScene::Sky : SyntheticScene::Moon);
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
    session_->setWatch(watch_);
    session_->setDetectionSettings(detectionPanel_->settings());
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
    QTimer::singleShot(0, this, &MainWindow::checkLegacyEventsNotice);
}

void MainWindow::onStopClicked()
{
    // Feature-32: the observer's stop leaves the mount as it is.
    sessionRun_.reset();
    endNote_.clear();
    finishStream();
}

void MainWindow::onMonitoringClicked()
{
    if (!session_->isRunning() || session_->modeChangePending()) return;
    const bool monitoring=session_->mode()==Session::Mode::Monitoring;
    if (!monitoring && !ensureOutputFolderReady("monitoring")) return;
    // Feature-32: the first start with a mount asks what it does at the end;
    // the observer's own stop leaves it as it is.
    if (!monitoring && !askMountAtEnd()) return;
    if (monitoring) sessionRun_.reset();
    if (!monitoring && activeCamera())
    {
        // Reopen with the requested controls, then read them back before any
        // evidence frame. Never assign live Preview readbacks to buffered data.
        if (!validateCameraSettings()) return;
        bufferReadoutTimer_->stop();
        session_->stop("monitoring settings boundary");
        startStream(Session::Mode::Monitoring);
        if (session_->isRunning()) beginSessionRun(false);
        return;
    }
    if (session_->requestMonitoring(!monitoring))
    {
        if (!monitoring) beginSessionRun(false);
        updateControls(); updateStatusLabel();
    }
}

void MainWindow::finishStream(const std::string& reason)
{
    bufferReadoutTimer_->stop();
    session_->stop(reason);
    chooseDrawing(Drawing::None);
    saveDetectionPanel();
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
    if (focusView_ && selected) setFocusView(false);
    for (const auto& family : cameraFamilies_)
    {
        const bool isSelected = family.panel == selected;
        family.dock->setVisible(isSelected && !focusView_);
        family.dock->toggleViewAction()->setVisible(isSelected);
        if (!isSelected) continue;
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
    applyCameraSettings({});
}

void MainWindow::applyCameraSettings(const QString& note)
{
    if (session_->mode()==Session::Mode::Monitoring || session_->mode()==Session::Mode::TimeLapse
        || session_->modeChangePending()) return;
    QString finalNote = note;
    if (CameraFamilyPanel* camera = selectedCamera(); camera && camera->hasCamera() && camera->controls())
    {
        const AlignedRoi aligned = camera->controls()->alignRequestedRoi();
        if (aligned.adjusted && finalNote.isEmpty())
        {
            finalNote = QString("ROI rounded to %1 × %2 at (%3, %4) (%5).")
                .arg(aligned.width).arg(aligned.height).arg(aligned.x).arg(aligned.y)
                .arg(camera->controls()->alignmentHint());
        }
    }
    if (!validateCameraSettings()) return;
    if (session_->isRunning())
    {
        bufferReadoutTimer_->stop(); session_->stop("camera settings changed");
        startStream(Session::Mode::Preview);
        if (session_->isRunning() && !finalNote.isEmpty()) showMessage(finalNote, 5000);
    }
    else if (CameraFamilyPanel* camera = selectedCamera())
    {
        std::string error;
        if (camera->readBack(error))
            showMessage(finalNote.isEmpty() ? "Camera settings read back; ready to Start" : finalNote, 5000);
        else
            showMessage(QString::fromStdString(error));
    }
    updateControls();
}

void MainWindow::applyCameraRoiChange(const QString& note)
{
    if (session_->mode()==Session::Mode::Monitoring || session_->mode()==Session::Mode::TimeLapse
        || session_->modeChangePending()) return;
    CameraFamilyPanel* camera = selectedCamera();
    if (!camera || !camera->hasCamera() || !camera->controls()) return;
    if (session_->isRunning() && session_->mode()==Session::Mode::Preview)
    {
        if (camera->controls()->requestedGeometry() == camera->controls()->actualGeometry())
        {
            if (!note.isEmpty()) showMessage(note, 4000);
            return;
        }
        applyCameraSettings(note);
    }
    else if (!session_->isRunning())
    {
        if (!note.isEmpty()) showMessage(note + " Click Start or Apply settings to use it.", 5000);
    }
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
    const BlipKind kind = selectedBlipKind();
    const auto centre = ui_->previewWidget->selectedPosition();
    if (kind != BlipKind::Lightning && !centre)
    {
        return;
    }
    const int cx = centre ? centre->x() : std::max(0, lastFrameWidth_ / 2);
    const int cy = centre ? centre->y() : std::max(0, lastFrameHeight_ / 2);
    double vx = 0.0, vy = 0.0;
    if (kind == BlipKind::DarkTransit) { vx = 8.0; vy = 2.0; }
    else if (kind == BlipKind::MovingStreak) { vx = 18.0; vy = 10.0; }
    else if (kind == BlipKind::BlinkingMover) { vx = 2.5; vy = 0.5; }
    else if (kind == BlipKind::IssSilhouette)
    {
        // Feature-48: at its real speed for this stream's frame rate, slightly downhill.
        const double travel = issTravelPerFrame(ui_->blipFwhmSpinBox->value(), 1.0 / std::max(1.0, measuredFps_));
        vx = travel * 0.97;
        vy = travel * 0.24;
    }
    const BlipSettings settings{cx, cy, ui_->blipFwhmSpinBox->value(),
        ui_->blipAmplitudeSpinBox->value(), ui_->blipFramesSpinBox->value(), kind, vx, vy};
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
        lastFrameWidth_ = frame->width;
        lastFrameHeight_ = frame->height;
        lastFrameBpp_ = static_cast<int>(bytesPerPixel(frame->format));
        updateFrameLabel(*frame);
        const bool firstFrame = !latestFrame_;
        latestFrame_ = frame;
        // Feature-26: auto-tune measures once the stream's first frame is in.
        if (firstFrame) updateAutoTune();
        if (const int side = std::min(frame->width, frame->height); side != frameShortSide_)
        {
            frameShortSide_ = side;
            guidingPanel_->setFrameShortSide(side);
            sendTolerance();
        }
        renderPreview();
        updateHistogram();
        // Feature-26: the earthshine area, once a frame arrives after the box was drawn.
        if (autoTunePanel_ && earthshineBox_ && earthshineArea_.columns == 0) updateEarthshineArea();
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
    ++remoteCounts_.candidates;
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
    // Feature-47: what the status page counts. A clip folder is named by its
    // first trigger's UTC time.
    if (!result.folder.isEmpty())
    {
        ++remoteCounts_.saved;
        if (!result.group.isEmpty()) ++remoteCounts_.groups[result.group];
        QDateTime time = QDateTime::fromString(QFileInfo(result.folder).fileName(), "yyyyMMdd-HHmmss-zzz");
        time.setTimeZone(QTimeZone::UTC);
        remoteCounts_.lastClip = RemoteStatus::Clip{time.isValid() ? time : QDateTime::currentDateTimeUtc(), result.group, result.groupFacts};
        remoteCounts_.lastClipFolder = result.folder;
        remoteCounts_.lastClipFrame = QSize(lastFrameWidth_, lastFrameHeight_);
    }
    else if (!result.notSaved.isEmpty() || result.diskReserve) ++remoteCounts_.notSaved;
    if (result.recordFailed)
    {
        recordFailed_ = true;
    }

    if (result.diskReserve)
    {
        // Feature-11: saving stops before the drive fills. The candidates are still logged.
        ++clipsNotSaved_;
        updateCandidateReadout();
        const QString free = QString("%1 GiB free on the output drive, below the disk reserve of %2 GiB")
            .arg(static_cast<double>(result.freeBytes) / kGiB, 0, 'f', 1)
            .arg(static_cast<double>(displayPanel_->captureSettings().diskReserveBytes) / kGiB, 0, 'f', 0);
        showMessage("Saving stopped: " + free + ". Candidates are still logged.");
        // Feature-32: Monitoring cannot save evidence any more, so it ends.
        if (sessionRun_ && !sessionRun_->timeLapse) endSession("the disk reserve: " + free);
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
    endSession("the recording ended");
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
    endSession("the source failed: " + message);
}
