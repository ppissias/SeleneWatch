// Feature-26: auto-tune's part of the window. The panel holds the
// suggestions and the K results; the window runs each measurement through
// the session, applies what the observer accepts, and logs it to
// candidates.txt like the exposure follower's changes. Feature-34: once the
// observer asks, the window tunes the camera itself, round by round.

#include "ui/mainwindow.h"
#include "ui_mainwindow.h"
#include "ui/auto_tune_panel.h"
#include "ui/cameras/camera_family_panel.h"
#include "ui/cameras/camera_settings_editor.h"
#include "ui/detection_panel.h"
#include "recording/metadata_text.h"
#include "sky/sky_schedule.h"
#include "detection/target_defaults.h"
#include "settings/watch_settings.h"

#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// Every camera family numbers its controls this way (Feature-15).
constexpr int kExposureControl = 0, kGainControl = 1, kOffsetControl = 2;

QString rowText(const TuneRow& row)
{
    return QString("%1=%2").arg(QString::fromStdString(row.name), QString::fromStdString(row.suggested));
}

} // namespace

void MainWindow::setupAutoTune()
{
    // Feature-28: its own window beside the main one, so opening it leaves the
    // Detection dock as it is. Closing it ends a box being drawn and hides the
    // earthshine outline; a measurement in progress goes on.
    autoTuneWindow_ = new QDialog(this, Qt::Window);
    autoTuneWindow_->setObjectName("autoTuneWindow");
    autoTuneWindow_->setWindowTitle("Auto-tune");
    auto* scroll = new QScrollArea(autoTuneWindow_);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    autoTunePanel_ = new AutoTunePanel;
    autoTunePanel_->setContentsMargins(8, 8, 8, 8);
    scroll->setWidget(autoTunePanel_);
    auto* windowLayout = new QVBoxLayout(autoTuneWindow_);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->addWidget(scroll);
    autoTuneWindow_->resize(480, 760);
    if (const QByteArray geometry = QSettings().value("autotune/geometry").toByteArray(); !geometry.isEmpty())
        autoTuneWindow_->restoreGeometry(geometry);
    connect(autoTuneWindow_, &QDialog::finished, this, [this] {
        QSettings().setValue("autotune/geometry", autoTuneWindow_->saveGeometry());
        drawingEarthshineBox_ = false;
        autoTunePanel_->setDrawingBox(false);
        updateEarthshineArea();
    });
    tuneReadbackTimer_ = new QTimer(this);
    tuneReadbackTimer_->setSingleShot(true);
    tuneReadbackTimer_->setInterval(10'000);
    connect(tuneReadbackTimer_, &QTimer::timeout, this, [this] {
        if (tunePhase_ != TunePhase::WaitingForCamera) return;
        tunePhase_ = TunePhase::Idle;
        leaveRefining();
        autoTunePanel_->setProgress("The camera did not report its new settings within 10 s. Tune again when the "
            "preview has settled.", 0, false);
        updateAutoTune();
    });
    if (const QRect saved = QSettings().value("autotune/earthshineBox").toRect(); saved.isValid()) earthshineBox_ = saved;
    // Feature-35: each sky watch's longest exposure.
    showAutoTuneLongestExposure();
    connect(autoTunePanel_, &AutoTunePanel::skyMaxExposureChanged, this, [this](double milliseconds) {
        saveLongestExposureMs(watch_, milliseconds);
    });

    connect(detectionPanel_, &DetectionPanel::autoTuneRequested, this, [this] {
        autoTuneWindow_->show();
        autoTuneWindow_->raise();
        autoTuneWindow_->activateWindow();
        updateEarthshineArea();
        updateAutoTune();
    });
    connect(autoTunePanel_, &AutoTunePanel::drawBoxToggled, this, [this](bool on) {
        drawingEarthshineBox_ = on;
        if (!on) return;
        chooseDrawing(Drawing::EarthshineBox);
        showMessage("Drag a box on the preview over the earthshine.", 6000);
    });
    connect(autoTunePanel_, &AutoTunePanel::boxOnlyToggled, this, [this] {
        stopCameraTune("The measured area changed: tuning stopped.");
        autoTunePanel_->clearCamera("The measured area changed: measure again.");
        autoTunePanel_->clearK("The measured area changed: watch again.");
        tuneHasK_ = false;
        updateEarthshineArea();
    });
    connect(autoTunePanel_, &AutoTunePanel::tuneCameraRequested, this, &MainWindow::startCameraTune);
    connect(autoTunePanel_, &AutoTunePanel::restoreCameraRequested, this, &MainWindow::restoreAutoTuneCamera);
    connect(autoTunePanel_, &AutoTunePanel::applyCameraRequested, this, &MainWindow::applyAutoTuneCamera);
    connect(autoTunePanel_, &AutoTunePanel::watchRequested, this, [this](bool higher) { startKWatch(higher); });
    connect(autoTunePanel_, &AutoTunePanel::tryKRequested, this, [this](double k) { startKWatch(false, k); });
    connect(autoTunePanel_, &AutoTunePanel::applyKRequested, this, &MainWindow::applyAutoTuneK);
    connect(autoTunePanel_, &AutoTunePanel::checkFlashRequested, this, &MainWindow::startFlashCheck);
    // Feature-30: the minimum patch, and the seeing it assumes.
    autoTunePanel_->setSeeingArcsec(QSettings().value("autotune/seeing", 2.5).toDouble());
    connect(autoTunePanel_, &AutoTunePanel::seeingChanged, this, [this](double arcsec) {
        QSettings().setValue("autotune/seeing", arcsec);
        updateFlashWidth();
    });
    connect(autoTunePanel_, &AutoTunePanel::minPatchRequested, this, &MainWindow::startMinPatch);
    connect(autoTunePanel_, &AutoTunePanel::applyMinPatchRequested, this, &MainWindow::applyAutoTuneMinPatch);
    connect(autoTunePanel_, &AutoTunePanel::cancelRequested, this, [this] {
        tuneReadbackTimer_->stop();
        if (tunePhase_ == TunePhase::MeasuringCamera || tunePhase_ == TunePhase::WaitingForCamera) leaveRefining();
        if (tunePhase_ == TunePhase::WaitingForCamera) autoTunePanel_->setProgress("Cancelled.", 0, false);
        tunePhase_ = TunePhase::Idle;
        session_->cancelAutoTune("Cancelled by the observer. Nothing was changed by the measurement.");
        updateAutoTune();
    });
    updateAutoTune();
}

void MainWindow::showAutoTuneLongestExposure()
{
    if (!autoTunePanel_ || target() != Target::Sky) return;
    autoTunePanel_->setSkyMaxExposureMs(savedLongestExposureMs(watch_));
}

TuneWorkflow MainWindow::autoTuneWorkflow() const
{
    return tuneWorkflow(target(), detectionPanel_->settings());
}

bool MainWindow::autoTuneShown() const
{
    return autoTuneWindow_ && autoTuneWindow_->isVisible();
}

bool MainWindow::autoTuneMeasuring() const
{
    return (session_ && session_->autoTuneBusy()) || tunePhase_ == TunePhase::WaitingForCamera;
}

TuneCamera MainWindow::tuneCamera() const
{
    TuneCamera camera;
    const CameraFamilyPanel* family = activeCamera();
    const CameraSettingsEditor* controls = family ? family->controls() : nullptr;
    if (!controls) return camera;
    const auto read = [controls](int id) -> std::optional<TuneControl> {
        const auto value = controls->controlValue(id);
        const auto range = controls->controlRange(id);
        if (!value || !range) return std::nullopt;
        return TuneControl{*value, range->first, range->second};
    };
    camera.exposure = read(kExposureControl);
    camera.gain = read(kGainControl);
    camera.offset = read(kOffsetControl);
    return camera;
}

QString MainWindow::twilightAdvice() const
{
    const TuneWorkflow workflow = autoTuneWorkflow();
    const ObservingSite site = savedSite();
    if (!site.hasPosition())
        return workflow == TuneWorkflow::Sky ? "Auto-tune is best run in full darkness, not at dusk or dawn: the sky "
            "keeps changing then. (With the site's position, Session → Site and instrument, the Sun's altitude is shown.)"
            : QString();
    const double altitude = sunAltitudeDegrees(*site.latitudeDeg, *site.longitudeDeg, std::chrono::system_clock::now());
    if (workflow == TuneWorkflow::Sky && altitude > -18)
        return QString("The Sun is at %1°: the sky will still get darker. Auto-tune is best run in full darkness (Sun "
            "below -18°). With Follow the sky's brightness on, the follower lengthens the exposure as it darkens; "
            "otherwise run auto-tune again later.").arg(altitude, 0, 'f', 1);
    if (workflow != TuneWorkflow::Sky && altitude > -12)
        return QString("The Sun is at %1°: the sky around the Moon will get darker. Run auto-tune again once it is "
            "dark.").arg(altitude, 0, 'f', 1);
    return {};
}

void MainWindow::updateAutoTune()
{
    if (!autoTunePanel_) return;
    AutoTunePanel::Situation situation;
    situation.workflow = autoTuneWorkflow();
    situation.preview = session_->isRunning() && session_->mode() == Session::Mode::Preview
        && !session_->sourceFinished() && !session_->modeChangePending() && latestFrame_;
    situation.camera = activeCamera() && activeCamera()->controls() && tuneCamera().exposure;
    situation.recording = session_->recordingStatus().active;
    situation.testingTools = testingTools_;
    situation.busy = autoTuneMeasuring();
    situation.haveBox = earthshineBox_.has_value();
    situation.canRestore = tuneRestore_.has_value();
    situation.twilight = twilightAdvice();
    autoTunePanel_->setSituation(situation);
    updateFlashWidth();
}

void MainWindow::setEarthshineBox(QRect box)
{
    earthshineBox_ = box;
    QSettings().setValue("autotune/earthshineBox", box);
    drawingEarthshineBox_ = false;
    if (autoTunePanel_)
    {
        stopCameraTune("The earthshine box changed: tuning stopped.");
        autoTunePanel_->setDrawingBox(false);
        autoTunePanel_->clearCamera("The earthshine box changed: measure again.");
        autoTunePanel_->clearK("The earthshine box changed: watch again.");
    }
    tuneHasK_ = false;
    updateEarthshineArea();
    updateAutoTune();
}

// The earthshine area from the latest frame, outlined on the preview while
// the panel is shown (Moon flashes only).
void MainWindow::updateEarthshineArea()
{
    const bool shown = autoTunePanel_ && autoTuneShown()
        && autoTuneWorkflow() == TuneWorkflow::MoonFlashes && earthshineBox_;
    if (!shown)
    {
        ui_->previewWidget->setTuneArea({}, 1, std::nullopt);
        return;
    }
    if (!latestFrame_)
    {
        ui_->previewWidget->setTuneArea({}, 1, earthshineBox_);
        autoTunePanel_->setAreaText("The area is found once frames arrive.");
        return;
    }
    const QRect box = *earthshineBox_;
    earthshineArea_ = findEarthshine(*latestFrame_, {box.x(), box.y(), box.width(), box.height()});
    if (autoTunePanel_->boxOnly())
    {
        ui_->previewWidget->setTuneArea({}, 1, box);
        autoTunePanel_->setAreaText("Measuring only your box.");
        return;
    }
    // The area cell by cell: a stronger colour on its edge, so its outline shows.
    const EarthshineArea& area = earthshineArea_;
    QImage cells(area.columns, area.rows, QImage::Format_ARGB32);
    cells.fill(Qt::transparent);
    const auto in = [&](int c, int r) {
        return c >= 0 && r >= 0 && c < area.columns && r < area.rows && area.cells[static_cast<size_t>(r) * area.columns + c];
    };
    for (int r = 0; r < area.rows; ++r)
        for (int c = 0; c < area.columns; ++c)
        {
            if (!in(c, r)) continue;
            const bool edge = !in(c - 1, r) || !in(c + 1, r) || !in(c, r - 1) || !in(c, r + 1);
            cells.setPixelColor(c, r, edge ? QColor(195, 140, 255, 190) : QColor(195, 140, 255, 50));
        }
    ui_->previewWidget->setTuneArea(cells, EarthshineArea::kCell, box);
    autoTunePanel_->setAreaText(area.boxOnly ? QString::fromStdString(area.problem)
        : QString("Earthshine area: %1 % of the frame, outlined on the preview. The box is at %2 % of full scale, the sky "
            "beyond the Moon's edge at %3 %. If the outline is wrong, draw the box again or measure only your box.")
              .arg(100 * area.shareOfFrame, 0, 'f', 0).arg(100 * area.boxLevel, 0, 'f', 1).arg(100 * area.skyLevel, 0, 'f', 1));
}

TuneArea MainWindow::autoTuneArea() const
{
    const int width = latestFrame_ ? latestFrame_->width : lastFrameWidth_;
    const int height = latestFrame_ ? latestFrame_->height : lastFrameHeight_;
    switch (autoTuneWorkflow())
    {
    case TuneWorkflow::Sky:
        return skyArea(detectionPanel_->settings().mask, width, height);
    case TuneWorkflow::MoonTransits:
        return {};
    case TuneWorkflow::MoonFlashes:
        break;
    }
    if (!earthshineBox_) return {};
    if (autoTunePanel_->boxOnly() || earthshineArea_.columns == 0)
    {
        TuneArea area;
        area.width = width;
        area.height = height;
        area.inside.assign(static_cast<size_t>(width) * height, 0);
        const QRect box = earthshineBox_->intersected(QRect(0, 0, width, height));
        for (int y = box.top(); y <= box.bottom(); ++y)
            for (int x = box.left(); x <= box.right(); ++x) area.inside[static_cast<size_t>(y) * width + x] = 1;
        return area;
    }
    return earthshineArea_.pixels(width, height);
}

// Moon: a saturated lock box cannot be followed (design point 9).
std::vector<std::string> MainWindow::guidingWarnings() const
{
    if (target() != Target::Moon || !latestFrame_) return {};
    const LockStatus lock = session_->guider().status().lock;
    if (lock.state == LockState::Off || lock.region.width <= 0 || lock.region.height <= 0) return {};
    TuneArea region;
    region.width = latestFrame_->width;
    region.height = latestFrame_->height;
    region.inside.assign(static_cast<size_t>(region.width) * region.height, 0);
    for (int y = std::max(0, lock.region.y); y < std::min(region.height, lock.region.y + lock.region.height); ++y)
        for (int x = std::max(0, lock.region.x); x < std::min(region.width, lock.region.x + lock.region.width); ++x)
            region.inside[static_cast<size_t>(y) * region.width + x] = 1;
    const auto measured = measureBrightness(*latestFrame_, region, false);
    if (!measured || measured->atMax <= 0.05) return {};
    return {QString("%1 % of the guiding lock box is saturated: guiding cannot follow a flat white area. Lock on an "
        "unsaturated feature.").arg(100 * measured->atMax, 0, 'f', 0).toStdString()};
}

// Feature-34: Tune camera. The loop's measurements start afresh; the
// settings from before the first loop stay for Restore.
void MainWindow::startCameraTune()
{
    if (!latestFrame_ || session_->recordingStatus().active) return;
    tuneHistory_.clear();
    tuneStage_ = CameraStage::Reaching;
    updateEarthshineArea();
    tuneArea_ = autoTuneArea();
    if (!tuneRestore_) tuneRestore_ = tuneCamera();
    autoTunePanel_->clearCameraRounds();
    autoTunePanel_->clearCamera({});
    startCameraMeasurement();
}

// One round's measurement of the area's brightness.
void MainWindow::startCameraMeasurement()
{
    if (!latestFrame_) return;
    TuneWatchRequest request;
    request.kind = TuneWatchRequest::Kind::Brightness;
    request.seconds = 2;
    request.area = tuneArea_;
    request.brightest = autoTuneWorkflow() == TuneWorkflow::MoonTransits;
    request.settings = detectionPanel_->settings();
    const uint64_t id = session_->startAutoTune(std::move(request));
    if (!id)
    {
        tunePhase_ = TunePhase::Idle;
        autoTunePanel_->setProgress("Auto-tune measures only in Preview.", 0, false);
        updateAutoTune();
        return;
    }
    tuneWatchId_ = id;
    tunePhase_ = TunePhase::MeasuringCamera;
    tuneMeasuredCamera_ = tuneCamera();
    autoTunePanel_->setProgress(QString("Round %1: measuring the brightness…").arg(tuneHistory_.size() + 1), 0, true);
    updateAutoTune();
}

// A round's measurement is in: set the next exposure or gain, or end the
// loop with the verdict and the rows left for the observer.
void MainWindow::onCameraMeasured(const TuneWatchStatus& status)
{
    const TuneCamera camera = tuneCamera();
    if (camera != tuneMeasuredCamera_)
    {
        tuneStage_ = CameraStage::Reaching;
        autoTunePanel_->clearCamera("The camera settings changed during the measurement: tuning stopped.");
        autoTunePanel_->setProgress({}, 0, false);
        return;
    }
    if (!status.brightness || !camera.exposure)
    {
        finishCameraTune(status, "Nothing to tune: see above.");
        return;
    }
    tuneHistory_.push_back(TuneMeasurement{camera.exposure->value, camera.gain ? camera.gain->value : 0, *status.brightness});
    const int round = static_cast<int>(tuneHistory_.size());
    const QString measured = QString("round=%1 measured_level=%2% measured_noise=%3%").arg(round)
        .arg(100 * status.brightness->level, 0, 'f', 2).arg(100 * status.brightness->noise, 0, 'f', 3);
    if (tuneStage_ == CameraStage::Confirming)
    {
        autoTunePanel_->addCameraRound(QString("%1. The chosen settings: level %2 %.").arg(round)
            .arg(100 * status.brightness->level, 0, 'f', 1));
        finishCameraTune(status, tuneResult_);
        return;
    }
    if (tuneStage_ == CameraStage::Refining)
    {
        refineCameraGain(status, measured);
        return;
    }

    const CameraStep step = nextCameraStep(autoTuneWorkflow(), camera, tuneHistory_, autoTunePanel_->skyMaxExposureUs());
    autoTunePanel_->addCameraRound(QString("%1. %2").arg(round).arg(QString::fromStdString(step.text)));
    if (step.outcome == CameraStep::Outcome::Move && round < kMaxCameraRounds)
    {
        setCameraLive(step.exposureUs, step.gain, measured);
        return;
    }
    if (step.outcome == CameraStep::Outcome::Move)
    {
        finishCameraTune(status, "Stopped after %1 without reaching the aim: the brightness may be changing (clouds, "
            "twilight). Tune again later.");
        return;
    }
    if (step.outcome == CameraStep::Outcome::OutOfReach)
    {
        finishCameraTune(status, "Stopped after %1: the closest the camera can get. Go on to step 2, or change the setup.");
        return;
    }
    // In the aim. Stage 2 compares gains by their noise, where that applies.
    if (!refinesGain(autoTuneWorkflow()) || !camera.gain || camera.gain->maximum <= camera.gain->minimum
        || status.brightness->noise <= 0)
    {
        finishCameraTune(status, "Tuned in %1: in the aim. Go on to step 2.");
        return;
    }
    tuneStage_ = CameraStage::Refining;
    tuneExposureUs_ = camera.exposure->value;
    tuneReachedGain_ = camera.gain->value;
    tuneRefineRounds_ = 0;
    refineCameraGain(status, measured);
}

// Feature-34, stage 2: the gain that lets the faintest flash or meteor
// through, by comparing the background's signal-to-noise at the tuned
// exposure; then the chosen settings are set and measured once more.
void MainWindow::refineCameraGain(const TuneWatchStatus& status, const QString& measured)
{
    const GainRefinement refinement = refineGain(autoTuneWorkflow(), tuneCamera(), tuneExposureUs_, tuneHistory_,
        autoTunePanel_->skyMaxExposureUs());
    // The round that reached the aim is already listed: stage 2 starts from it.
    autoTunePanel_->addCameraRound((tuneRefineRounds_ == 0 ? QString("Stage 2, comparing gains by their noise. ")
        : QString("%1. ").arg(tuneHistory_.size())) + QString::fromStdString(refinement.text));
    if (refinement.action != GainRefinement::Action::Choose && tuneRefineRounds_ < kMaxRefineRounds)
    {
        ++tuneRefineRounds_;
        setCameraLive(refinement.exposureUs, refinement.gain, measured + " stage=refine");
        return;
    }
    double gain = tuneReachedGain_;
    if (refinement.action == GainRefinement::Action::Choose)
    {
        gain = refinement.gain;
        QStringList compared;
        for (const GainCandidate& c : refinement.candidates)
            compared << QString("gain=%1 level=%2% black=%3% noise=%4% snr=%5").arg(c.gain, 0, 'g', 6)
                .arg(100 * c.level, 0, 'f', 2).arg(100 * c.black, 0, 'f', 2).arg(100 * c.noise, 0, 'f', 3)
                .arg(c.signalToNoise(), 0, 'f', 1);
        session_->logRecord(QString("%1 autotune_gain_choice target=%2 exposure_ms=%3 gain=%4 compared=\"%5\"\n")
            .arg(utcText(std::chrono::system_clock::now()), target() == Target::Sky ? "sky" : "moon")
            .arg(tuneExposureUs_ / 1000, 0, 'g', 6).arg(gain, 0, 'g', 6).arg(compared.join("; ")));
        tuneResult_ = "Tuned in %1: in the aim, at the gain with the faintest threshold. Go on to step 2.";
    }
    else
        tuneResult_ = "Tuned in %1: in the aim. Comparing gains took too many rounds, so the gain first found in the aim "
            "is kept. Go on to step 2.";
    const TuneCamera camera = tuneCamera();
    tuneStage_ = CameraStage::Confirming;
    if (camera.exposure && camera.exposure->value == tuneExposureUs_ && camera.gain && camera.gain->value == gain)
    {
        finishCameraTune(status, tuneResult_);
        return;
    }
    setCameraLive(tuneExposureUs_, gain, measured + " stage=chosen");
}

// Feature-34: a loop stopped while refining leaves the camera at the
// settings stage 1 reached, not at a dark or probe round's.
void MainWindow::leaveRefining()
{
    if (tuneStage_ == CameraStage::Reaching) return;
    tuneStage_ = CameraStage::Reaching;
    CameraSettingsEditor* controls = activeCamera() ? activeCamera()->controls() : nullptr;
    if (!controls || session_->mode() != Session::Mode::Preview) return;
    const TuneCamera camera = tuneCamera();
    if (camera.exposure && camera.exposure->value != tuneExposureUs_) controls->setControlValue(kExposureControl, tuneExposureUs_);
    if (camera.gain && camera.gain->value != tuneReachedGain_) controls->setControlValue(kGainControl, tuneReachedGain_);
    session_->logRecord(QString("%1 autotune_camera target=%2 stage=stopped applied=\"Exposure=%3 ms; Gain=%4\"\n")
        .arg(utcText(std::chrono::system_clock::now()), target() == Target::Sky ? "sky" : "moon")
        .arg(tuneExposureUs_ / 1000, 0, 'g', 6).arg(tuneReachedGain_, 0, 'g', 6));
}

// The loop ends: the verdict, warnings, and the rows left for the observer.
void MainWindow::finishCameraTune(const TuneWatchStatus& status, const QString& result)
{
    tuneStage_ = CameraStage::Reaching;
    const SkyExtras extras = detectionPanel_->skyExtras();
    CameraTuneInput input;
    input.workflow = autoTuneWorkflow();
    input.camera = tuneCamera();
    input.measured = status.brightness;
    input.skyMaxExposureUs = autoTunePanel_->skyMaxExposureUs();
    input.follow = extras.follow;
    input.followFromMs = extras.minExposureMs;
    input.followToMs = extras.maxExposureMs;
    input.followMaxGain = extras.maxGain;
    input.followGainStep = extras.gainStep;
    CameraSuggestion suggestion = suggestCamera(input);
    // The loop owns exposure and gain; the Offset and the follower's limits stay the observer's.
    std::erase_if(suggestion.rows, [](const TuneRow& row) {
        return row.kind == TuneRow::Kind::Exposure || row.kind == TuneRow::Kind::Gain;
    });
    for (std::string& warning : guidingWarnings()) suggestion.warnings.push_back(std::move(warning));
    autoTunePanel_->setCameraSuggestion(suggestion);
    const int rounds = static_cast<int>(tuneHistory_.size());
    autoTunePanel_->setProgress(result.contains("%1") ? result.arg(QString("%1 round%2").arg(rounds).arg(rounds == 1 ? "" : "s"))
        : result, 1, false);
    updateAutoTune();
}

// Feature-34: sets the loop's next exposure and gain through the camera
// settings editor, as the observer would, logs them, and waits for the
// camera to report them back.
void MainWindow::setCameraLive(double exposureUs, double gain, const QString& record)
{
    CameraFamilyPanel* family = activeCamera();
    CameraSettingsEditor* controls = family ? family->controls() : nullptr;
    if (!controls || session_->mode() != Session::Mode::Preview || session_->recordingStatus().active)
    {
        autoTunePanel_->setProgress("Tuning stopped: the camera can be changed only in Preview, without a recording.", 0, false);
        updateAutoTune();
        return;
    }
    const TuneCamera before = tuneCamera();
    bool changed = false;
    QStringList applied;
    if (before.exposure && exposureUs != before.exposure->value)
    {
        changed |= controls->setControlValue(kExposureControl, exposureUs);
        applied << QString("Exposure=%1 ms").arg(exposureUs / 1000, 0, 'g', 6);
    }
    if (before.gain && gain != before.gain->value)
    {
        changed |= controls->setControlValue(kGainControl, gain);
        applied << QString("Gain=%1").arg(gain, 0, 'g', 6);
    }
    session_->logRecord(QString("%1 autotune_camera target=%2 %3 applied=\"%4\"\n").arg(utcText(std::chrono::system_clock::now()),
        target() == Target::Sky ? "sky" : "moon", record, applied.join("; ")));
    if (!changed)
    {
        autoTunePanel_->setProgress("Tuning stopped: the camera did not take the new setting.", 0, false);
        updateAutoTune();
        return;
    }
    // The editor sends the changed controls to the camera after its short
    // pause; the next round starts when the camera reports them back.
    tunePhase_ = TunePhase::WaitingForCamera;
    tuneReadbackTimer_->start();
    autoTunePanel_->clearK("The camera settings changed: watch again with the new ones.");
    autoTunePanel_->clearMinPatch("The camera settings changed: measure again.");
    tuneHasK_ = false;
    tuneHasMinPatch_ = false;
    autoTunePanel_->setProgress("Waiting for the camera to report " + applied.join(", ") + "…", 0, true);
    updateAutoTune();
}

// Feature-34: the loop stops when what it measures changes.
void MainWindow::stopCameraTune(const QString& why)
{
    if (tunePhase_ != TunePhase::MeasuringCamera && tunePhase_ != TunePhase::WaitingForCamera) return;
    tuneReadbackTimer_->stop();
    tunePhase_ = TunePhase::Idle;
    leaveRefining();
    session_->cancelAutoTune(why.toStdString());
    autoTunePanel_->setProgress(why, 0, false);
}

// Feature-34: the exposure and gain from before Tune camera changed them.
void MainWindow::restoreAutoTuneCamera()
{
    CameraFamilyPanel* family = activeCamera();
    CameraSettingsEditor* controls = family ? family->controls() : nullptr;
    if (!tuneRestore_ || autoTuneMeasuring() || !controls || session_->mode() != Session::Mode::Preview
        || session_->recordingStatus().active) return;
    QStringList applied;
    if (tuneRestore_->exposure)
    {
        controls->setControlValue(kExposureControl, tuneRestore_->exposure->value);
        applied << QString("Exposure=%1 ms").arg(tuneRestore_->exposure->value / 1000, 0, 'g', 6);
    }
    if (tuneRestore_->gain)
    {
        controls->setControlValue(kGainControl, tuneRestore_->gain->value);
        applied << QString("Gain=%1").arg(tuneRestore_->gain->value, 0, 'g', 6);
    }
    session_->logRecord(QString("%1 autotune_camera target=%2 restore=yes applied=\"%3\"\n").arg(
        utcText(std::chrono::system_clock::now()), target() == Target::Sky ? "sky" : "moon", applied.join("; ")));
    tuneRestore_.reset();
    tuneHistory_.clear();
    tuneStage_ = CameraStage::Reaching;
    autoTunePanel_->clearCameraRounds();
    autoTunePanel_->clearCamera("Restored: " + applied.join(", ") + ".");
    autoTunePanel_->clearK("The camera settings changed: watch again.");
    autoTunePanel_->clearMinPatch("The camera settings changed: measure again.");
    tuneHasK_ = false;
    tuneHasMinPatch_ = false;
    autoTunePanel_->setProgress({}, 0, false);
    showMessage("Auto-tune restored: " + applied.join(", "), 6000);
    updateAutoTune();
}

// The rows the loop leaves to the observer: the Offset and the follower's
// limits. A new Offset shifts the black level, so the loop runs again.
void MainWindow::applyAutoTuneCamera()
{
    CameraFamilyPanel* family = activeCamera();
    CameraSettingsEditor* controls = family ? family->controls() : nullptr;
    if (!controls || session_->mode() != Session::Mode::Preview || session_->recordingStatus().active) return;
    if (tuneCamera() != tuneMeasuredCamera_)
    {
        autoTunePanel_->clearCamera("The camera settings changed since the measurement: tune again.");
        return;
    }
    const std::vector<TuneRow> rows = autoTunePanel_->tickedCameraRows();
    if (rows.empty()) return;
    bool cameraChanged = false, followerChanged = false;
    SkyExtras extras = detectionPanel_->skyExtras();
    QStringList applied;
    for (const TuneRow& row : rows)
    {
        switch (row.kind)
        {
        case TuneRow::Kind::Exposure: cameraChanged |= controls->setControlValue(kExposureControl, row.value); break;
        case TuneRow::Kind::Gain: cameraChanged |= controls->setControlValue(kGainControl, row.value); break;
        case TuneRow::Kind::Offset: cameraChanged |= controls->setControlValue(kOffsetControl, row.value); break;
        case TuneRow::Kind::FollowExposure:
            extras.minExposureMs = row.value;
            extras.maxExposureMs = row.value2;
            followerChanged = true;
            break;
        case TuneRow::Kind::FollowGain:
            extras.maxGain = row.value;
            extras.gainStep = row.value2;
            followerChanged = true;
            break;
        }
        applied << rowText(row);
    }
    if (followerChanged)
    {
        detectionPanel_->setSkyExtras(extras);
        saveSkyExtrasPanel();
        skyExposureState_ = {};
    }
    const QString measured = tuneHistory_.empty() ? QString()
        : QString(" measured_level=%1%").arg(100 * tuneHistory_.back().brightness.level, 0, 'f', 2);
    session_->logRecord(QString("%1 autotune_camera target=%2%3 applied=\"%4\"\n").arg(utcText(std::chrono::system_clock::now()),
        target() == Target::Sky ? "sky" : "moon", measured, applied.join("; ")));
    if (cameraChanged)
    {
        // The earlier measurements were taken at the old black level.
        tuneHistory_.clear();
        tuneStage_ = CameraStage::Reaching;
        autoTunePanel_->addCameraRound("Applied " + applied.join(", ") + ": tuning again.");
        tunePhase_ = TunePhase::WaitingForCamera;
        tuneReadbackTimer_->start();
        autoTunePanel_->clearCamera("Applied: " + applied.join(", ") + ".");
        autoTunePanel_->clearK("The camera settings changed: watch again with the new ones.");
        tuneHasK_ = false;
        autoTunePanel_->setProgress("Waiting for the camera to report its new settings…", 0, true);
    }
    else
    {
        autoTunePanel_->clearCamera("Applied: " + applied.join(", ") + ".");
        autoTunePanel_->setProgress({}, 0, false);
    }
    showMessage("Auto-tune applied: " + applied.join(", "), 6000);
    updateAutoTune();
}

void MainWindow::onCameraReadbackForAutoTune()
{
    if (!autoTunePanel_) return;
    if (tunePhase_ == TunePhase::WaitingForCamera)
    {
        tuneReadbackTimer_->stop();
        const StreamInfo info = session_->streamInfo();
        session_->logRecord(QString("%1 autotune_readback exposure=\"%2\" gain=\"%3\" offset=\"%4\"\n")
            .arg(utcText(std::chrono::system_clock::now()), QString::fromStdString(info.exposure),
                QString::fromStdString(info.gain), QString::fromStdString(info.offset)));
        tunePhase_ = TunePhase::Idle;
        startCameraMeasurement();
        return;
    }
    // The observer changed the camera: what was measured no longer holds,
    // and the settings from before the loop are no longer what Restore means.
    // (A second readback of the loop's own values changes nothing.)
    if (!tuneHistory_.empty() && tuneCamera() != tuneMeasuredCamera_)
    {
        autoTunePanel_->clearCamera("The camera settings changed: tune again.");
        tuneHistory_.clear();
        tuneRestore_.reset();
    }
    if (tuneHasK_ || tuneHasMinPatch_)
    {
        autoTunePanel_->clearK("The camera settings changed: watch again.");
        autoTunePanel_->clearMinPatch("The camera settings changed: measure again.");
        tuneHasK_ = false;
        tuneHasMinPatch_ = false;
    }
    updateAutoTune();
}

void MainWindow::onDetectionSettingsChangedForAutoTune()
{
    if (!autoTunePanel_) return;
    // The watch in progress is cancelled by the session; the loop stops with it.
    stopCameraTune("The detection settings changed: tuning stopped.");
    if (tuneHasK_ && detectionPanel_->settings() != tuneBoundDetection_)
    {
        autoTunePanel_->clearK("The detection settings changed after the watch: watch again.");
        tuneHasK_ = false;
    }
    if (tuneHasMinPatch_ && detectionPanel_->settings() != tuneBoundDetection_)
    {
        autoTunePanel_->clearMinPatch("The detection settings changed after the watch: measure again.");
        tuneHasMinPatch_ = false;
    }
    updateEarthshineArea();
    updateAutoTune();
}

void MainWindow::startKWatch(bool higher, std::optional<double> onlyK)
{
    if (onlyK)
    {
        // Try K: an ordinary, visible change of the Preview's K, then a watch of it.
        DetectionSettings settings = detectionPanel_->settings();
        settings.sigma = *onlyK;
        detectionPanel_->setSettings(settings);
        session_->setDetectionSettings(detectionPanel_->settings());
        saveDetectionPanel();
    }
    updateEarthshineArea();
    const DetectionSettings settings = detectionPanel_->settings();
    TuneWatchRequest request;
    request.kind = TuneWatchRequest::Kind::Sensitivity;
    request.seconds = autoTunePanel_->watchSeconds();
    request.area = autoTuneArea();
    request.brightest = autoTuneWorkflow() == TuneWorkflow::MoonTransits;
    request.settings = settings;
    request.kValues = onlyK ? std::vector<double>{*onlyK}
        : kValuesToTest(higher ? autoTunePanel_->highestTestedK() + 1 : settings.sigma, 4);
    const uint64_t id = session_->startAutoTune(std::move(request));
    if (!id)
    {
        autoTunePanel_->setProgress("Auto-tune watches only in Preview.", 0, false);
        return;
    }
    tuneWatchId_ = id;
    tunePhase_ = TunePhase::WatchingK;
    tuneBoundDetection_ = settings;
    tuneBoundCamera_ = tuneCamera();
    autoTunePanel_->setProgress("Watching…", 0, true);
    updateAutoTune();
}

void MainWindow::applyAutoTuneK()
{
    const auto k = autoTunePanel_->suggestedK();
    if (!k || session_->mode() == Session::Mode::Monitoring) return;
    if (detectionPanel_->settings() != tuneBoundDetection_ || tuneCamera() != tuneBoundCamera_)
    {
        autoTunePanel_->clearK("A camera or detection setting changed since the watch: watch again.");
        tuneHasK_ = false;
        return;
    }
    const double before = detectionPanel_->settings().sigma;
    session_->logRecord(QString("%1 autotune_k target=%2 k_from=%3 k_to=%4 %5\n").arg(utcText(std::chrono::system_clock::now()),
        target() == Target::Sky ? "sky" : "moon").arg(before, 0, 'f', 1).arg(*k, 0, 'f', 1).arg(autoTunePanel_->kRecord()));
    DetectionSettings settings = detectionPanel_->settings();
    settings.sigma = *k;
    detectionPanel_->setSettings(settings);
    session_->setDetectionSettings(detectionPanel_->settings());
    saveDetectionPanel();
    autoTunePanel_->clearK(QString("Applied K %1 (it was %2).").arg(*k, 0, 'f', 1).arg(before, 0, 'f', 1));
    tuneHasK_ = false;
    showMessage(QString("Auto-tune applied K %1.").arg(*k, 0, 'f', 1), 6000);
    updateAutoTune();
}

QPoint MainWindow::tuneFlashPoint() const
{
    // The flash goes where the observer clicked, or into the measured area.
    QPoint at(latestFrame_->width / 2, latestFrame_->height / 2);
    if (const auto selected = ui_->previewWidget->selectedPosition()) return *selected;
    if (autoTuneWorkflow() == TuneWorkflow::MoonFlashes && earthshineBox_) return earthshineBox_->center();
    const TuneArea area = autoTuneArea();
    if (!area.wholeFrame() && !area.inside[static_cast<size_t>(at.y()) * area.width + at.x()])
        for (size_t i = 0; i < area.inside.size(); ++i)
            if (area.inside[i]) return QPoint(static_cast<int>(i % area.width), static_cast<int>(i / area.width));
    return at;
}

void MainWindow::startFlashCheck()
{
    if (!testingTools_ || !latestFrame_ || session_->recordingStatus().active) return;
    const QPoint at = tuneFlashPoint();
    TuneWatchRequest request;
    request.kind = TuneWatchRequest::Kind::FlashCheck;
    request.settings = detectionPanel_->settings();
    request.flash = BlipSettings{at.x(), at.y(), ui_->blipFwhmSpinBox->value(), ui_->blipAmplitudeSpinBox->value(),
        ui_->blipFramesSpinBox->value(), BlipKind::BrightFlash};
    const uint64_t id = session_->startAutoTune(std::move(request));
    if (!id)
    {
        autoTunePanel_->setCheckResult("The flash check needs Preview, no recording and no other test flash.", std::nullopt);
        return;
    }
    tuneWatchId_ = id;
    tunePhase_ = TunePhase::CheckingFlash;
    autoTunePanel_->setCheckResult(QString("Checking with a %1 ADU test flash at (%2, %3)…")
        .arg(ui_->blipAmplitudeSpinBox->value(), 0, 'f', 0).arg(at.x()).arg(at.y()), std::nullopt);
    updateAutoTune();
}

void MainWindow::onAutoTuneChanged()
{
    if (!autoTunePanel_) return;
    const TuneWatchStatus status = session_->autoTuneStatus();
    if (status.id != tuneWatchId_ || tunePhase_ == TunePhase::Idle || tunePhase_ == TunePhase::WaitingForCamera)
    {
        updateAutoTune();
        return;
    }
    switch (status.state)
    {
    case TuneWatchStatus::State::Idle:
        break;
    case TuneWatchStatus::State::Waiting:
    case TuneWatchStatus::State::Measuring:
        autoTunePanel_->setProgress(status.state == TuneWatchStatus::State::Waiting || status.note.empty()
            ? QString::fromStdString(status.note.empty() ? "Measuring…" : status.note)
            : QString::fromStdString(status.note), status.progress, true);
        if (status.state == TuneWatchStatus::State::Measuring && tunePhase_ == TunePhase::WatchingMinPatch)
            autoTunePanel_->setProgress(QString("Watching minimum patches: %1 of %2 s, %3 frames.").arg(status.seconds, 0, 'f', 0)
                .arg(status.requestedSeconds, 0, 'f', 0).arg(status.frames), status.progress, true);
        if (status.state == TuneWatchStatus::State::Measuring && tunePhase_ == TunePhase::WatchingK)
            autoTunePanel_->setProgress(QString("Watching: %1 of %2 s, %3 frames.%4").arg(status.seconds, 0, 'f', 0)
                .arg(status.requestedSeconds, 0, 'f', 0).arg(status.frames)
                .arg(status.note.empty() ? QString() : " " + QString::fromStdString(status.note)), status.progress, true);
        break;
    case TuneWatchStatus::State::Cancelled:
        if (tunePhase_ == TunePhase::CheckingFlash) autoTunePanel_->setCheckResult(QString::fromStdString(status.note), std::nullopt);
        if (tunePhase_ == TunePhase::MeasuringFootprint || tunePhase_ == TunePhase::WatchingMinPatch)
            autoTunePanel_->setFootprintText(QString::fromStdString(status.note));
        autoTunePanel_->setProgress(QString::fromStdString(status.note), 0, false);
        tunePhase_ = TunePhase::Idle;
        break;
    case TuneWatchStatus::State::Finished:
    {
        const TunePhase phase = tunePhase_;
        tunePhase_ = TunePhase::Idle;
        if (phase == TunePhase::MeasuringCamera) onCameraMeasured(status);
        else if (phase == TunePhase::WatchingK)
        {
            autoTunePanel_->setKResults(status);
            tuneHasK_ = true;
            autoTunePanel_->setProgress(QString("Watched %1 s of source time over %2 frames.").arg(status.seconds, 0, 'f', 1)
                .arg(status.frames), 1, false);
        }
        else if (phase == TunePhase::MeasuringFootprint)
        {
            // The footprint, found or not; the watch of minimum patches follows.
            tuneFootprint_ = status.footprintPixels;
            autoTunePanel_->setFootprintText(QString::fromStdString(status.checkText));
            startMinPatchWatch();
            return;
        }
        else if (phase == TunePhase::WatchingMinPatch)
        {
            autoTunePanel_->setMinPatchResults(status, tuneFootprint_);
            tuneHasMinPatch_ = true;
            autoTunePanel_->setProgress(QString("Watched %1 s of source time over %2 frames.").arg(status.seconds, 0, 'f', 1)
                .arg(status.frames), 1, false);
        }
        else if (phase == TunePhase::CheckingFlash)
        {
            autoTunePanel_->setCheckResult(QString::fromStdString(status.checkText), status.recovered);
            autoTunePanel_->setProgress({}, 0, false);
        }
        break;
    }
    }
    updateAutoTune();
}

// Feature-30: a real flash is as wide as a star; without an image scale the
// Test flash FWHM is the assumption.
// The image scale: from Site and instrument, or its focal length with the
// pixel size the camera reports when none was entered.
std::optional<double> MainWindow::tuneImageScale(bool& pixelFromCamera) const
{
    ObservingSite site = savedSite();
    pixelFromCamera = false;
    const CameraFamilyPanel* camera = activeCamera() ? activeCamera() : selectedCamera();
    if (site.pixelSizeUm <= 0 && camera && camera->pixelSizeUm() > 0)
    {
        site.pixelSizeUm = camera->pixelSizeUm();
        pixelFromCamera = true;
    }
    return site.arcsecPerPixel();
}

FlashWidth MainWindow::tuneFlashWidth() const
{
    bool pixelFromCamera = false;
    return flashWidth(autoTunePanel_->seeingArcsec(), tuneImageScale(pixelFromCamera), ui_->blipFwhmSpinBox->value());
}

void MainWindow::updateFlashWidth()
{
    if (!autoTunePanel_) return;
    const FlashWidth width = tuneFlashWidth();
    bool pixelFromCamera = false;
    const auto scale = tuneImageScale(pixelFromCamera);
    QString text;
    if (width.fromSeeing)
        text = QString("A flash is about %1 px wide (%2″ seeing at %3″ per pixel%4).").arg(width.pixels, 0, 'f', 1)
            .arg(autoTunePanel_->seeingArcsec(), 0, 'f', 1).arg(*scale, 0, 'f', 2)
            .arg(pixelFromCamera ? ", with the pixel size the camera reports" : "");
    else if (savedSite().focalLengthMm > 0)
        text = QString("A flash is assumed %1 px wide (the Test flash FWHM). Enter the pixel size in Session → Site and "
            "instrument, or connect the camera, for a width from your seeing.").arg(width.pixels, 0, 'f', 1);
    else
        text = QString("A flash is assumed %1 px wide (the Test flash FWHM). Enter the focal length in Session → Site and "
            "instrument for a width from your seeing.").arg(width.pixels, 0, 'f', 1);
    autoTunePanel_->setFlashWidthText(text);
}

void MainWindow::startMinPatch()
{
    if (!latestFrame_ || autoTuneWorkflow() != TuneWorkflow::MoonFlashes || session_->recordingStatus().active) return;
    updateEarthshineArea();
    const QPoint at = tuneFlashPoint();
    TuneWatchRequest request;
    request.kind = TuneWatchRequest::Kind::Footprint;
    request.settings = detectionPanel_->settings();
    request.flash = BlipSettings{at.x(), at.y(), tuneFlashWidth().pixels, 0, 2, BlipKind::BrightFlash};
    const uint64_t id = session_->startAutoTune(std::move(request));
    if (!id)
    {
        autoTunePanel_->setProgress("The footprint flash needs Preview, no recording and no other test flash.", 0, false);
        return;
    }
    tuneWatchId_ = id;
    tunePhase_ = TunePhase::MeasuringFootprint;
    tuneFootprint_.reset();
    autoTunePanel_->clearMinPatch({});
    autoTunePanel_->setFootprintText(QString("Measuring the footprint of a flash at (%1, %2)…").arg(at.x()).arg(at.y()));
    autoTunePanel_->setProgress("Measuring the footprint…", 0, true);
    updateAutoTune();
}

void MainWindow::startMinPatchWatch()
{
    const DetectionSettings settings = detectionPanel_->settings();
    TuneWatchRequest request;
    request.kind = TuneWatchRequest::Kind::Sensitivity;
    request.seconds = autoTunePanel_->minPatchSeconds();
    request.area = autoTuneArea();
    request.settings = settings;
    request.minPixelValues = minPatchValuesToTest(settings.minPixels);
    const uint64_t id = session_->startAutoTune(std::move(request));
    if (!id)
    {
        tunePhase_ = TunePhase::Idle;
        autoTunePanel_->setProgress("Auto-tune watches only in Preview.", 0, false);
        updateAutoTune();
        return;
    }
    tuneWatchId_ = id;
    tunePhase_ = TunePhase::WatchingMinPatch;
    tuneBoundDetection_ = settings;
    tuneBoundCamera_ = tuneCamera();
    autoTunePanel_->setProgress("Watching minimum patches…", 0, true);
    updateAutoTune();
}

void MainWindow::applyAutoTuneMinPatch()
{
    const auto minPixels = autoTunePanel_->suggestedMinPatch();
    if (!minPixels || session_->mode() == Session::Mode::Monitoring) return;
    if (detectionPanel_->settings() != tuneBoundDetection_ || tuneCamera() != tuneBoundCamera_)
    {
        autoTunePanel_->clearMinPatch("A camera or detection setting changed since the watch: measure again.");
        tuneHasMinPatch_ = false;
        return;
    }
    DetectionSettings settings = detectionPanel_->settings();
    const int before = settings.minPixels;
    const FlashWidth width = tuneFlashWidth();
    session_->logRecord(QString("%1 autotune_min_patch target=moon k=%2 min_from=%3 min_to=%4 flash_width_px=%5 "
        "flash_width_from=%6 %7\n").arg(utcText(std::chrono::system_clock::now())).arg(settings.sigma, 0, 'f', 1).arg(before)
        .arg(*minPixels).arg(width.pixels, 0, 'f', 1).arg(width.fromSeeing ? "seeing" : "test_flash")
        .arg(autoTunePanel_->minPatchRecord()));
    settings.minPixels = *minPixels;
    settings.maxPixels = std::max(settings.maxPixels, settings.minPixels);
    detectionPanel_->setSettings(settings);
    session_->setDetectionSettings(detectionPanel_->settings());
    saveDetectionPanel();
    autoTunePanel_->clearMinPatch(QString("Applied minimum patch %1 px (it was %2 px).").arg(*minPixels).arg(before));
    tuneHasMinPatch_ = false;
    showMessage(QString("Auto-tune applied minimum patch %1 px.").arg(*minPixels), 6000);
    updateAutoTune();
}
