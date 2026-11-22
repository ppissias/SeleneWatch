#include "ui/guiding_panel.h"
#include "ui/widgets/readout_label.h"

#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimeZone>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <numbers>

namespace {

QString signedPixels(double value)
{
    return (value >= 0 ? "+" : "−") + QString::number(std::abs(value), 'f', 2);
}

// Degrees per second as a multiple of the sidereal rate.
QString siderealMultiple(double degreesPerSecond)
{
    return QString::number(degreesPerSecond * 3600.0 / 15.041, 'f', 2) + "× sidereal";
}

// A rate in pixels per second and its direction on the image, 0 degrees to the right, 90 down.
QString rateText(ImageVector perMillisecond)
{
    const ImageVector perSecond = perMillisecond * 1000.0;
    const double angle = std::atan2(perSecond.y, perSecond.x) * 180.0 / std::numbers::pi;
    return QString("%1 px/s at %2°").arg(perSecond.length(), 0, 'f', 2).arg(angle, 0, 'f', 0);
}

QLabel* wrapped(QWidget* parent, const char* objectName = nullptr)
{
    auto* label = new QLabel(parent);
    label->setWordWrap(true);
    if (objectName) label->setObjectName(objectName);
    return label;
}

// A readout that keeps room for its longest texts; see ReadoutLabel.
QLabel* readout(QWidget* parent, const char* objectName, const QStringList& longest)
{
    auto* label = new ReadoutLabel(longest, parent);
    if (objectName) label->setObjectName(objectName);
    return label;
}

QString driftText(const LockStatus& lock, std::optional<double> arcsecPerPixel)
{
    if (lock.estimateValid)
    {
        QString text = QString("dx %1 px, dy %2 px (median of %3)")
                           .arg(signedPixels(lock.estimateDx), signedPixels(lock.estimateDy)).arg(lock.estimateSamples);
        if (arcsecPerPixel)
            text += QString("; %1″").arg(std::hypot(lock.estimateDx, lock.estimateDy) * *arcsecPerPixel, 0, 'f', 1);
        return text;
    }
    if (lock.state == LockState::Off) return "—";
    return lock.state == LockState::Locked ? "Collecting measurements" : "No estimate while the lock is lost";
}

// Feature-41/43: live drift speed over recent unpulsed matches, decomposed along
// RA and Dec once calibrated so the observer can tune polar alignment live.
// While actively guiding (pulsing faster than 4 s), falls back to the active
// drift rate inside GuideController so the readout does not vanish.
QString driftSpeedText(const LockStatus& lock, const GuideStatus& guide)
{
    if (lock.state == LockState::Off) return "—";
    auto formatAxes = [&](double raPx, double decPx) {
        const double speedPx = std::hypot(raPx, decPx);
        QString text;
        if (guide.calibration && guide.calibration->arcsecPerPixel)
        {
            const double scale = *guide.calibration->arcsecPerPixel;
            text = QString("%1 px/s (RA %2″/s, Dec %3″/s)")
                       .arg(speedPx, 0, 'f', 2)
                       .arg(signedPixels(raPx * scale), signedPixels(decPx * scale));
        }
        else
        {
            text = QString("%1 px/s (RA %2, Dec %3 px/s)")
                       .arg(speedPx, 0, 'f', 2)
                       .arg(signedPixels(raPx), signedPixels(decPx));
        }
        if (guide.calibration)
        {
            const double raSpeed = guide.calibration->west.length() * 1000.0;
            const double decSpeed = guide.calibration->north.length() * 1000.0;
            if ((raSpeed > 0 && std::abs(raPx) > 0.7 * raSpeed) || (decSpeed > 0 && std::abs(decPx) > 0.7 * decSpeed))
                text += " — fast";
        }
        return text;
    };
    if (!lock.driftRateValid)
    {
        if (guide.mode == GuideMode::Guiding && guide.driftEstimateValid)
            return formatAxes(guide.driftRaPxPerSec, guide.driftDecPxPerSec);
        return lock.state == LockState::Locked ? "Measuring…" : "—";
    }
    const double speedPx = std::hypot(lock.driftRateDx, lock.driftRateDy);
    if (guide.calibration)
    {
        const ImageVector& w = guide.calibration->west;
        const ImageVector& n = guide.calibration->north;
        const double det = w.x * n.y - w.y * n.x;
        if (std::abs(det) >= 1e-12)
        {
            const double raPx = (lock.driftRateDx * n.y - lock.driftRateDy * n.x) / det * w.length();
            const double decPx = (w.x * lock.driftRateDy - w.y * lock.driftRateDx) / det * n.length();
            return formatAxes(raPx, decPx);
        }
    }
    return QString("%1 px/s (dx %2, dy %3)")
        .arg(speedPx, 0, 'f', 2)
        .arg(signedPixels(lock.driftRateDx), signedPixels(lock.driftRateDy));
}

QString matchText(const std::optional<LockMatch>& match)
{
    if (!match) return "—";
    QString text = QString("dx %1, dy %2 px, score %3").arg(signedPixels(match->dx), signedPixels(match->dy))
                       .arg(match->score, 0, 'f', 3);
    if (!match->valid) text += " — not used: " + QString::fromStdString(match->reason);
    return text;
}

QString regionText(const LockStatus& lock)
{
    if (lock.state == LockState::Off) return "—";
    return QString("%1, %2, %3 x %4 px, binned %5 x %5; contrast %6, structure %7")
        .arg(lock.region.x).arg(lock.region.y).arg(lock.region.width).arg(lock.region.height)
        .arg(lock.binning).arg(lock.quality.contrast, 0, 'f', 0).arg(lock.quality.structureRatio, 0, 'f', 2);
}

QString mountText(const GuideStatus& guide)
{
    if (guide.mountNotResponding)
    {
        return "<span style='color:#ff6060'><b>Mount not responding.</b> A driver call has not returned. "
               "Acquisition and detection continue.</span>";
    }
    if (!guide.mountConnected)
    {
        return guide.mountError.empty() ? QString("Not connected")
                                        : "<span style='color:#ff6060'>" + QString::fromStdString(guide.mountError).toHtmlEscaped() + "</span>";
    }
    const MountState& m = guide.mount;
    QString text = QString("<b>%1</b>: %2, %3, %4, pier %5")
                       .arg(QString::fromStdString(guide.mountName).toHtmlEscaped(),
                           m.tracking ? "tracking" : "<span style='color:#ff6060'>not tracking</span>",
                           m.slewing ? "<span style='color:#ff6060'>slewing</span>" : "not slewing",
                           m.parked ? "<span style='color:#ff6060'>parked</span>" : "not parked",
                           pierSideName(m.pierSide));
    if (!m.canPulseGuide) text += ", <span style='color:#ff6060'>cannot pulse-guide</span>";
    if (m.altAzimuth) text += ", <span style='color:#ff6060'>alt-azimuth</span>";
    if (m.guideRateRa && m.guideRateDec)
        text += QString("; guide rate RA %1, Dec %2").arg(siderealMultiple(*m.guideRateRa), siderealMultiple(*m.guideRateDec));
    if (!guide.mountError.empty())
        text += "<br><span style='color:#ff6060'>" + QString::fromStdString(guide.mountError).toHtmlEscaped() + "</span>";
    return text;
}

QString calibrationText(const GuideStatus& guide)
{
    if (guide.mode == GuideMode::Calibrating) return "Calibrating: " + QString::fromStdString(guide.calibrationProgress);
    if (!guide.calibration) return "Not calibrated";
    const GuideCalibration& c = *guide.calibration;
    QString text = QString("West %1; north %2; axes %3° apart; drift %4 px/s; pier %5")
                       .arg(rateText(c.west), rateText(c.north)).arg(c.axisAngleDegrees, 0, 'f', 0)
                       .arg(c.drift.length(), 0, 'f', 2).arg(pierSideName(c.pierSide));
    if (guide.guideSpeedPixelsPerFrame > 0)
        text += QString("; a pulse moves %1 px per frame").arg(guide.guideSpeedPixelsPerFrame, 0, 'f', 2);
    if (c.arcsecPerPixel) text += QString("; %1″ per px").arg(*c.arcsecPerPixel, 0, 'f', 2);
    return text;
}

QString guidingText(const GuideStatus& guide)
{
    QString text = guideModeName(guide.mode);
    text[0] = text[0].toUpper();
    if (guide.mode == GuideMode::Guiding)
        text = QString("<b>Guiding</b>: RA error %1 px, Dec error %2 px")
                   .arg(signedPixels(guide.raErrorPixels), signedPixels(guide.decErrorPixels));
    if (guide.pulses > 0 && guide.lastPulse)
    {
        text += QString("; %1 pulses, last %2 %3 ms at %4 UTC")
                    .arg(guide.pulses)
                    .arg(guideDirectionName(guide.lastPulse->direction))
                    .arg(guide.lastPulse->milliseconds)
                    .arg(QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(
                             guide.lastPulseTime.time_since_epoch()).count(), QTimeZone::UTC).toString("HH:mm:ss"));
    }
    return text;
}

// The widest values the readouts are expected to show in a session, so they
// can keep room for them: offsets within the lock's search radius, pulses of
// at most 1.5 s, a few arcseconds per pixel. A rare state (a mount error, a
// mount that cannot guide) may still take more.
GuiderStatus widestStatus()
{
    GuiderStatus status;
    LockStatus& lock = status.lock;
    lock.state = LockState::Searching;
    lock.region = {1279, 1023, 512, 512};
    lock.binning = 8;
    lock.quality.contrast = 999;
    lock.quality.structureRatio = 0.99;
    lock.estimateValid = true;
    lock.estimateDx = lock.estimateDy = -99.99;
    lock.estimateSamples = 99;
    lock.driftRateValid = true;
    lock.driftRateDx = lock.driftRateDy = -9.99;
    // The longest reason FeatureLock gives for a match it does not use.
    lock.lastMatch = LockMatch{-99.99, -99.99, 0.999, false, "the best match is on the edge of the search window"};

    GuideStatus& guide = status.guide;
    guide.mountConnected = true;
    guide.mountName = "Celestron Telescope Driver";
    guide.mount.connected = true;
    guide.mount.tracking = true;
    guide.mount.canPulseGuide = true;
    guide.mount.guideRateRa = guide.mount.guideRateDec = 0.0042;
    guide.mode = GuideMode::Guiding;
    GuideCalibration calibration;
    calibration.west = calibration.north = ImageVector{-0.01414, -0.01414};
    calibration.drift = ImageVector{-0.99, -0.99};
    calibration.axisAngleDegrees = 179;
    calibration.arcsecPerPixel = 2.99;
    guide.calibration = calibration;
    guide.guideSpeedPixelsPerFrame = 0.25;
    guide.raErrorPixels = guide.decErrorPixels = -99.99;
    guide.pulses = 9999;
    guide.lastPulse = GuidePulse{GuideDirection::North, 5000};
    return status;
}

} // namespace

GuidingPanel::GuidingPanel(QWidget* parent)
    : QWidget(parent)
{
    // The lock.
    auto* lockBox = new QGroupBox("Feature lock", this);
    auto* hint = new QLabel("Press <b>Draw lock box</b>, then drag a rectangle on the image around a crater or other "
        "detail to lock on it; or click a pixel and press <b>Lock at selection</b>. The lock measures how far the field "
        "drifts. Its box is drawn in cyan, labelled <i>Guiding lock</i>.", lockBox);
    hint->setWordWrap(true);
    // The readouts keep room for their longest texts, and the lock message
    // for two lines, so the controls below them stay where they are.
    const GuiderStatus widest = widestStatus();
    state_ = wrapped(lockBox, "lockStateLabel");
    drift_ = readout(lockBox, "lockDriftLabel", {driftText(widest.lock, widest.guide.calibration->arcsecPerPixel)});
    drift_->setToolTip("Median of the valid offsets of the last few seconds of source time, "
        "relative to where the feature was locked. Positive is right and down in the frame.");
    driftSpeed_ = readout(lockBox, "lockDriftSpeedLabel", {driftSpeedText(widest.lock, widest.guide)});
    driftSpeed_->setToolTip("Live drift speed fitted over up to 20 s of unpulsed lock. "
        "Once calibrated, splits drift along RA and Dec so you can tune polar alignment live "
        "(note: the Moon itself moves up to ~0.25″/s in Dec). Resets after each guide pulse and feeds drift compensation.");
    match_ = readout(lockBox, "lockMatchLabel", {matchText(widest.lock.lastMatch)});
    region_ = readout(lockBox, nullptr, {regionText(widest.lock)});
    message_ = readout(lockBox, "lockMessageLabel", {ReadoutLabel::blankLines(2)});
    message_->setStyleSheet("color: #e0a030;");
    logProblem_ = new QLabel("guiding.txt could not be written. Guiding continues; its record is incomplete.", lockBox);
    logProblem_->setWordWrap(true);
    logProblem_->setStyleSheet("color: #ff6060;");
    logProblem_->hide();
    auto* lockForm = new QFormLayout;
    lockForm->addRow("State", state_);
    lockForm->addRow("Drift", drift_);
    lockForm->addRow("Drift speed", driftSpeed_);
    lockForm->addRow("Last match", match_);
    lockForm->addRow("Region", region_);
    // Feature-28: a drag on the preview locks only after this is pressed, so
    // it is not mistaken for auto-tune's earthshine box or the sky mask.
    drawLock_ = new QPushButton("Draw lock box", lockBox);
    drawLock_->setObjectName("drawLockBox");
    drawLock_->setCheckable(true);
    drawLock_->setToolTip("While on, the next rectangle dragged on the preview becomes the guiding lock box. It turns "
        "off by itself after one box, or when another drawing (earthshine box, mask, anchor) is chosen.");
    lockButton_ = new QPushButton("Lock at selection", lockBox);
    lockButton_->setObjectName("lockAtSelectionButton");
    lockButton_->setToolTip(QString("Lock on a %1 x %1 pixel box centred on the selected pixel.").arg(kDefaultLockSide));
    unlockButton_ = new QPushButton("Unlock", lockBox);
    unlockButton_->setObjectName("unlockButton");
    unlockButton_->setToolTip("Releases the feature lock. Guiding stops using it until a new region is locked.");
    auto* lockButtons = new QHBoxLayout;
    lockButtons->addWidget(drawLock_);
    lockButtons->addWidget(lockButton_);
    lockButtons->addWidget(unlockButton_);
    lockButtons->addStretch();
    auto* lockLayout = new QVBoxLayout(lockBox);
    lockLayout->addWidget(hint);
    lockLayout->addLayout(lockForm);
    lockLayout->addWidget(message_);
    lockLayout->addLayout(lockButtons);

    // The mount.
    auto* mountBox = new QGroupBox("Mount", this);
    mountKind_ = new QComboBox(mountBox);
    mountKind_->setObjectName("mountKindCombo");
    mountKind_->addItem("Simulated mount (synthetic source)");
    mountKind_->addItem("ASCOM telescope");
    mountKind_->setToolTip("The simulated mount moves the synthetic Moon; use it with the Synthetic source. "
        "An ASCOM telescope is any mount with an ASCOM driver installed.");
    chooseButton_ = new QPushButton("Choose…", mountBox);
    chooseButton_->setObjectName("chooseMountButton");
    chooseButton_->setToolTip("Opens the ASCOM Platform's telescope chooser.");
    driver_ = wrapped(mountBox, "mountDriverLabel");
    connectButton_ = new QPushButton("Connect", mountBox);
    connectButton_->setObjectName("connectMountButton");
    connectButton_->setToolTip("Connects to the chosen mount (simulated or ASCOM) so guiding can send pulses to it.");
    disconnectButton_ = new QPushButton("Disconnect", mountBox);
    disconnectButton_->setObjectName("disconnectMountButton");
    disconnectButton_->setToolTip("Disconnects the mount. Guiding pulses stop.");
    mountState_ = readout(mountBox, "mountStateLabel", {mountText(widest.guide), mountText(GuideStatus{.mountNotResponding = true})});
    auto* driverRow = new QHBoxLayout;
    driverRow->addWidget(chooseButton_);
    driverRow->addWidget(driver_, 1);
    auto* mountButtons = new QHBoxLayout;
    mountButtons->addWidget(connectButton_);
    mountButtons->addWidget(disconnectButton_);
    mountButtons->addStretch();
    auto* mountLayout = new QVBoxLayout(mountBox);
    mountLayout->addWidget(mountKind_);
    mountLayout->addLayout(driverRow);
    mountLayout->addLayout(mountButtons);
    mountLayout->addWidget(mountState_);

    // Calibration and guiding.
    auto* guideBox = new QGroupBox("Guiding", this);
    auto* guideHint = new QLabel("Only guide pulses are sent, and a tracking stop when a session ends if you chose it "
        "(Ends → Change…): never a slew or a park. Calibrate with the lock held, then start guiding. Guiding is off at "
        "every start.", guideBox);
    guideHint->setWordWrap(true);
    calibrateButton_ = new QPushButton("Calibrate", guideBox);
    calibrateButton_->setObjectName("calibrateButton");
    calibrateButton_->setToolTip("Pulses west, east, north (after taking up backlash), and south, and measures how the image moves.");
    calibration_ = readout(guideBox, "calibrationLabel", {calibrationText(widest.guide)});
    tolerance_ = new QDoubleSpinBox(guideBox);
    tolerance_->setObjectName("toleranceSpinBox");
    tolerance_->setRange(0.5, 20.0);
    tolerance_->setSingleStep(0.5);
    tolerance_->setDecimals(1);
    tolerance_->setSuffix(" % of field");
    tolerance_->setValue(3.0);
    tolerance_->setToolTip("Nothing is sent while the drift stays within this distance on both mount axes. "
        "Make it several times the seeing's jitter.");
    toleranceReadout_ = wrapped(guideBox);
    giveUp_ = new QDoubleSpinBox(guideBox);
    giveUp_->setObjectName("giveUpSpinBox");
    giveUp_->setRange(5.0, 45.0);
    giveUp_->setSingleStep(1.0);
    giveUp_->setDecimals(1);
    giveUp_->setSuffix(" % of field");
    giveUp_->setValue(15.0);
    giveUp_->setToolTip("Higher drift limit before guiding gives up when strong polar-alignment drift is only being "
        "held or slowed. Reversed axes still stop immediately; if drift crosses this limit and corrections cannot "
        "reduce it, guiding stops safely.");
    giveUpReadout_ = wrapped(guideBox);
    aggressiveness_ = new QSpinBox(guideBox);
    aggressiveness_->setObjectName("guideAggressivenessSpinBox");
    aggressiveness_->setRange(30, 100);
    aggressiveness_->setSingleStep(5);
    aggressiveness_->setSuffix(" %");
    aggressiveness_->setValue(static_cast<int>(std::lround(GuideController::Settings{}.gain * 100.0)));
    aggressiveness_->setToolTip("Share of the measured drift removed by each correction, plus steady-drift compensation. "
        "Raise toward 90–100 % when polar alignment is rough; lower if seeing causes over-corrections.");
    maxPulse_ = new QSpinBox(guideBox);
    maxPulse_->setObjectName("guideMaxPulseSpinBox");
    maxPulse_->setRange(500, 5000);
    maxPulse_->setSingleStep(250);
    maxPulse_->setSuffix(" ms");
    maxPulse_->setValue(GuideController::Settings{}.maxPulseMs);
    maxPulse_->setToolTip("Longest single guide pulse SeleneWatch may send on one axis. "
        "Longer pulses help keep up with polar-alignment drift and clear declination backlash faster.");
    guideButton_ = new QPushButton("Start guiding", guideBox);
    guideButton_->setObjectName("guideButton");
    guideButton_->setToolTip("Starts or stops pulse guiding on the locked feature. Needs a connected, calibrated mount.");
    // Feature-32: a lost lock is searched for this long before guiding, and the session, end.
    lostAfter_ = new QSpinBox(guideBox);
    lostAfter_->setObjectName("lockLostAfter");
    lostAfter_->setRange(static_cast<int>(LockTracker::kMinSearchSeconds), static_cast<int>(LockTracker::kMaxSearchSeconds));
    lostAfter_->setSingleStep(30);
    lostAfter_->setSuffix(" s");
    lostAfter_->setValue(static_cast<int>(LockTracker::kDefaultSearchSeconds));
    lostAfter_->setToolTip("While guiding, a lock that finds no match (a cloud, the feature out of view) is searched for "
        "this long. If it is not found again, guiding stops, and so does the session: Monitoring or the time-lapse ends "
        "and the mount is left as chosen under Ends → Change…. A guiding stop for a safety reason (the mount stopped "
        "tracking, changed its side of pier, did not answer) ends the session at once.");
    auto* endsNote = new QLabel("While guiding, a lost lock (after this time) or a guiding stop ends the session.", guideBox);
    endsNote->setObjectName("guidingEndsSession");
    endsNote->setWordWrap(true);
    guiding_ = readout(guideBox, "guidingLabel", {guidingText(widest.guide)});
    guideMessage_ = wrapped(guideBox, "guideMessageLabel");
    guideMessage_->setStyleSheet("color: #e0a030;");
    auto* toleranceRow = new QHBoxLayout;
    toleranceRow->addWidget(tolerance_);
    toleranceRow->addWidget(toleranceReadout_, 1);
    auto* giveUpRow = new QHBoxLayout;
    giveUpRow->addWidget(giveUp_);
    giveUpRow->addWidget(giveUpReadout_, 1);
    auto* guideButtons = new QHBoxLayout;
    guideButtons->addWidget(calibrateButton_);
    guideButtons->addWidget(guideButton_);
    guideButtons->addStretch();
    auto* guideForm = new QFormLayout;
    guideForm->addRow("Calibration", calibration_);
    guideForm->addRow("Tolerance", toleranceRow);
    guideForm->addRow("Give up at", giveUpRow);
    guideForm->addRow("Aggressiveness", aggressiveness_);
    guideForm->addRow("Max pulse", maxPulse_);
    guideForm->addRow("Lock lost after", lostAfter_);
    guideForm->addRow("Status", guiding_);
    auto* guideLayout = new QVBoxLayout(guideBox);
    guideLayout->addWidget(guideHint);
    guideLayout->addLayout(guideButtons);
    guideLayout->addLayout(guideForm);
    guideLayout->addWidget(endsNote);
    guideLayout->addWidget(guideMessage_);

    // The readouts keep room for long texts, so the panel scrolls when the
    // dock is shorter than all of it, instead of squeezing the readouts.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    layout->addWidget(lockBox);
    layout->addWidget(mountBox);
    layout->addWidget(guideBox);
    layout->addWidget(logProblem_);
    layout->addStretch();

    connect(drawLock_, &QPushButton::toggled, this, &GuidingPanel::drawLockToggled);
    connect(lockButton_, &QPushButton::clicked, this, &GuidingPanel::lockAtSelectionRequested);
    connect(unlockButton_, &QPushButton::clicked, this, &GuidingPanel::unlockRequested);
    connect(chooseButton_, &QPushButton::clicked, this, &GuidingPanel::chooseDriverRequested);
    connect(connectButton_, &QPushButton::clicked, this, &GuidingPanel::connectRequested);
    connect(disconnectButton_, &QPushButton::clicked, this, &GuidingPanel::disconnectRequested);
    connect(calibrateButton_, &QPushButton::clicked, this, &GuidingPanel::calibrateRequested);
    connect(guideButton_, &QPushButton::clicked, this, [this] { emit guidingRequested(mode_ != GuideMode::Guiding); });
    connect(tolerance_, &QDoubleSpinBox::valueChanged, this, [this] {
        updateTolerance();
        emit toleranceChanged();
    });
    connect(giveUp_, &QDoubleSpinBox::valueChanged, this, [this] {
        updateTolerance();
        emit toleranceChanged();
    });
    connect(aggressiveness_, &QSpinBox::valueChanged, this, &GuidingPanel::correctionTuningChanged);
    connect(maxPulse_, &QSpinBox::valueChanged, this, &GuidingPanel::correctionTuningChanged);
    connect(lostAfter_, &QSpinBox::valueChanged, this, &GuidingPanel::lostAfterChanged);
    connect(mountKind_, &QComboBox::currentIndexChanged, this, [this] {
        updateMountControls();
        emit mountKindChanged();
    });
    setStatus({});
    setAvailable(false, false);
    updateTolerance();
}

void GuidingPanel::setStatus(const GuiderStatus& status)
{
    const LockStatus& lock = status.lock;
    const GuideStatus& guide = status.guide;
    locked_ = lock.state != LockState::Off;
    connected_ = guide.mountConnected;
    mode_ = guide.mode;
    calibrated_ = guide.calibration.has_value();
    arcsecPerPixel_ = guide.calibration ? guide.calibration->arcsecPerPixel : std::nullopt;

    unlockButton_->setEnabled(locked_);
    QString state = lockStateName(lock.state);
    state[0] = state[0].toUpper();
    state_->setText("<b>" + state + "</b>");
    drift_->setText(driftText(lock, arcsecPerPixel_));
    driftSpeed_->setText(driftSpeedText(lock, guide));
    match_->setText(matchText(lock.lastMatch));
    region_->setText(regionText(lock));
    message_->setText(QString::fromStdString(lock.message));

    mountState_->setText(mountText(guide));
    calibration_->setText(calibrationText(guide));
    guiding_->setText(guidingText(guide));
    guideMessage_->setText(QString::fromStdString(guide.message));
    guideMessage_->setVisible(!guide.message.empty());
    updateMountControls();
    updateTolerance();
}

void GuidingPanel::updateMountControls()
{
    const bool ascom = mountKind() == MountKind::Ascom;
    mountKind_->setEnabled(!connected_);
    chooseButton_->setVisible(ascom);
    driver_->setVisible(ascom);
    chooseButton_->setEnabled(ascom && ascomAvailable_ && !connected_);
    connectButton_->setEnabled(!connected_ && (!ascom || (ascomAvailable_ && !driver_->property("progId").toString().isEmpty())));
    disconnectButton_->setEnabled(connected_);
    calibrateButton_->setEnabled(connected_ && locked_ && mode_ != GuideMode::Calibrating);
    guideButton_->setText(mode_ == GuideMode::Guiding ? "Stop guiding" : "Start guiding");
    guideButton_->setEnabled(mode_ == GuideMode::Guiding || (connected_ && locked_ && calibrated_ && mode_ == GuideMode::Idle));
}

void GuidingPanel::updateTolerance()
{
    if (frameShortSide_ <= 0)
    {
        toleranceReadout_->setText("—");
        giveUpReadout_->setText("—");
        return;
    }
    auto format = [this](double percent) {
        const double pixels = percent / 100.0 * frameShortSide_;
        QString text = QString("%1 px").arg(pixels, 0, 'f', 1);
        if (arcsecPerPixel_) text += QString(", %1″").arg(pixels * *arcsecPerPixel_, 0, 'f', 1);
        return text;
    };
    toleranceReadout_->setText(format(tolerancePercent()));
    giveUpReadout_->setText(format(giveUpPercent()));
}

void GuidingPanel::setAvailable(bool running, bool pixelSelected)
{
    lockButton_->setEnabled(running && pixelSelected);
    drawLock_->setEnabled(running);
    if (!running) setDrawingLock(false);
}

void GuidingPanel::setDrawingLock(bool on)
{
    const QSignalBlocker blocked(drawLock_);
    drawLock_->setChecked(on);
}

bool GuidingPanel::drawingLock() const { return drawLock_->isChecked(); }

void GuidingPanel::setLogFailed(bool failed)
{
    logProblem_->setVisible(failed);
}

void GuidingPanel::setAscomAvailable(bool available)
{
    ascomAvailable_ = available;
    if (!available)
    {
        mountKind_->setItemText(1, "ASCOM telescope (ASCOM Platform not found)");
        driver_->setText("Install the ASCOM Platform and your mount's driver to use a real mount.");
    }
    updateMountControls();
}

void GuidingPanel::setAscomDriver(const QString& progId)
{
    driver_->setProperty("progId", progId);
    if (ascomAvailable_) driver_->setText(progId.isEmpty() ? QString("No driver chosen") : progId);
    updateMountControls();
}

void GuidingPanel::setFrameShortSide(int pixels)
{
    frameShortSide_ = pixels;
    updateTolerance();
}

GuidingPanel::MountKind GuidingPanel::mountKind() const
{
    return mountKind_->currentIndex() == 1 ? MountKind::Ascom : MountKind::Simulated;
}

void GuidingPanel::setMountKind(MountKind kind)
{
    mountKind_->setCurrentIndex(kind == MountKind::Ascom ? 1 : 0);
    updateMountControls();
}

double GuidingPanel::tolerancePercent() const
{
    return tolerance_->value();
}

void GuidingPanel::setTolerancePercent(double percent)
{
    tolerance_->setValue(percent);
}

double GuidingPanel::giveUpPercent() const
{
    return giveUp_->value();
}

void GuidingPanel::setGiveUpPercent(double percent)
{
    giveUp_->setValue(percent);
}

int GuidingPanel::aggressivenessPercent() const
{
    return aggressiveness_->value();
}

void GuidingPanel::setAggressivenessPercent(int percent)
{
    aggressiveness_->setValue(percent);
}

int GuidingPanel::maxPulseMs() const
{
    return maxPulse_->value();
}

void GuidingPanel::setMaxPulseMs(int milliseconds)
{
    maxPulse_->setValue(milliseconds);
}

double GuidingPanel::lostAfterSeconds() const
{
    return lostAfter_->value();
}

void GuidingPanel::setLostAfterSeconds(double seconds)
{
    lostAfter_->setValue(static_cast<int>(std::lround(seconds)));
}
