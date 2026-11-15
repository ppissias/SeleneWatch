#include "guiding_panel.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
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

} // namespace

GuidingPanel::GuidingPanel(QWidget* parent)
    : QWidget(parent)
{
    // The lock.
    auto* lockBox = new QGroupBox("Feature lock", this);
    auto* hint = new QLabel("Drag a rectangle on the image around a crater or other detail to lock on it, "
        "or click a pixel and press <b>Lock at selection</b>. The lock measures how far the field drifts.", lockBox);
    hint->setWordWrap(true);
    state_ = wrapped(lockBox, "lockStateLabel");
    drift_ = wrapped(lockBox, "lockDriftLabel");
    drift_->setToolTip("Median of the valid offsets of the last few seconds of source time, "
        "relative to where the feature was locked. Positive is right and down in the frame.");
    match_ = wrapped(lockBox, "lockMatchLabel");
    region_ = wrapped(lockBox);
    message_ = wrapped(lockBox, "lockMessageLabel");
    message_->setStyleSheet("color: #e0a030;");
    logProblem_ = new QLabel("guiding.txt could not be written. Guiding continues; its record is incomplete.", lockBox);
    logProblem_->setWordWrap(true);
    logProblem_->setStyleSheet("color: #ff6060;");
    logProblem_->hide();
    auto* lockForm = new QFormLayout;
    lockForm->addRow("State", state_);
    lockForm->addRow("Drift", drift_);
    lockForm->addRow("Last match", match_);
    lockForm->addRow("Region", region_);
    lockButton_ = new QPushButton("Lock at selection", lockBox);
    lockButton_->setObjectName("lockAtSelectionButton");
    lockButton_->setToolTip(QString("Lock on a %1 x %1 pixel box centred on the selected pixel.").arg(kDefaultLockSide));
    unlockButton_ = new QPushButton("Unlock", lockBox);
    unlockButton_->setObjectName("unlockButton");
    auto* lockButtons = new QHBoxLayout;
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
    disconnectButton_ = new QPushButton("Disconnect", mountBox);
    disconnectButton_->setObjectName("disconnectMountButton");
    mountState_ = wrapped(mountBox, "mountStateLabel");
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
    auto* guideHint = new QLabel("Only guide pulses are sent: never a slew, a park, or a tracking change. "
        "Calibrate with the lock held, then start guiding. Guiding is off at every start.", guideBox);
    guideHint->setWordWrap(true);
    calibrateButton_ = new QPushButton("Calibrate", guideBox);
    calibrateButton_->setObjectName("calibrateButton");
    calibrateButton_->setToolTip("Pulses west, east, north (after taking up backlash), and south, and measures how the image moves.");
    calibration_ = wrapped(guideBox, "calibrationLabel");
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
    guideButton_ = new QPushButton("Start guiding", guideBox);
    guideButton_->setObjectName("guideButton");
    guiding_ = wrapped(guideBox, "guidingLabel");
    guideMessage_ = wrapped(guideBox, "guideMessageLabel");
    guideMessage_->setStyleSheet("color: #e0a030;");
    auto* toleranceRow = new QHBoxLayout;
    toleranceRow->addWidget(tolerance_);
    toleranceRow->addWidget(toleranceReadout_, 1);
    auto* guideButtons = new QHBoxLayout;
    guideButtons->addWidget(calibrateButton_);
    guideButtons->addWidget(guideButton_);
    guideButtons->addStretch();
    auto* guideForm = new QFormLayout;
    guideForm->addRow("Calibration", calibration_);
    guideForm->addRow("Tolerance", toleranceRow);
    guideForm->addRow("Status", guiding_);
    auto* guideLayout = new QVBoxLayout(guideBox);
    guideLayout->addWidget(guideHint);
    guideLayout->addLayout(guideButtons);
    guideLayout->addLayout(guideForm);
    guideLayout->addWidget(guideMessage_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(lockBox);
    layout->addWidget(mountBox);
    layout->addWidget(guideBox);
    layout->addWidget(logProblem_);
    layout->addStretch();

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
    locked_ = lock.state != LockState::Off;
    unlockButton_->setEnabled(locked_);
    QString state = lockStateName(lock.state);
    state[0] = state[0].toUpper();
    state_->setText("<b>" + state + "</b>");

    if (lock.estimateValid)
    {
        QString text = QString("dx %1 px, dy %2 px (median of %3)")
                           .arg(signedPixels(lock.estimateDx), signedPixels(lock.estimateDy)).arg(lock.estimateSamples);
        if (arcsecPerPixel_)
            text += QString("; %1″").arg(std::hypot(lock.estimateDx, lock.estimateDy) * *arcsecPerPixel_, 0, 'f', 1);
        drift_->setText(text);
    }
    else if (locked_)
    {
        drift_->setText(lock.state == LockState::Locked ? "Collecting measurements" : "No estimate while the lock is lost");
    }
    else
    {
        drift_->setText("—");
    }

    if (lock.lastMatch)
    {
        const LockMatch& match = *lock.lastMatch;
        QString text = QString("dx %1, dy %2 px, score %3").arg(signedPixels(match.dx), signedPixels(match.dy))
                           .arg(match.score, 0, 'f', 3);
        if (!match.valid) text += " — not used: " + QString::fromStdString(match.reason);
        match_->setText(text);
    }
    else
    {
        match_->setText("—");
    }

    if (locked_)
    {
        region_->setText(QString("%1, %2, %3 x %4 px, binned %5 x %5; contrast %6, structure %7")
            .arg(lock.region.x).arg(lock.region.y).arg(lock.region.width).arg(lock.region.height)
            .arg(lock.binning).arg(lock.quality.contrast, 0, 'f', 0).arg(lock.quality.structureRatio, 0, 'f', 2));
    }
    else
    {
        region_->setText("—");
    }
    message_->setText(QString::fromStdString(lock.message));
    message_->setVisible(!lock.message.empty());

    const GuideStatus& guide = status.guide;
    connected_ = guide.mountConnected;
    mode_ = guide.mode;
    calibrated_ = guide.calibration.has_value();
    arcsecPerPixel_ = guide.calibration ? guide.calibration->arcsecPerPixel : std::nullopt;

    if (guide.mountNotResponding)
    {
        mountState_->setText("<span style='color:#ff6060'><b>Mount not responding.</b> A driver call has not returned. "
                             "Acquisition and detection continue.</span>");
    }
    else if (!guide.mountConnected)
    {
        mountState_->setText(guide.mountError.empty() ? QString("Not connected")
                                                      : "<span style='color:#ff6060'>" + QString::fromStdString(guide.mountError).toHtmlEscaped() + "</span>");
    }
    else
    {
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
        mountState_->setText(text);
    }

    if (guide.mode == GuideMode::Calibrating)
    {
        calibration_->setText("Calibrating: " + QString::fromStdString(guide.calibrationProgress));
    }
    else if (guide.calibration)
    {
        const GuideCalibration& c = *guide.calibration;
        QString text = QString("West %1; north %2; axes %3° apart; drift %4 px/s; pier %5")
                           .arg(rateText(c.west), rateText(c.north)).arg(c.axisAngleDegrees, 0, 'f', 0)
                           .arg(c.drift.length(), 0, 'f', 2).arg(pierSideName(c.pierSide));
        if (guide.guideSpeedPixelsPerFrame > 0)
            text += QString("; a pulse moves %1 px per frame").arg(guide.guideSpeedPixelsPerFrame, 0, 'f', 2);
        if (c.arcsecPerPixel) text += QString("; %1″ per px").arg(*c.arcsecPerPixel, 0, 'f', 2);
        calibration_->setText(text);
    }
    else
    {
        calibration_->setText("Not calibrated");
    }

    QString guiding = guideModeName(guide.mode);
    guiding[0] = guiding[0].toUpper();
    if (guide.mode == GuideMode::Guiding)
        guiding = QString("<b>Guiding</b>: RA error %1 px, Dec error %2 px")
                      .arg(signedPixels(guide.raErrorPixels), signedPixels(guide.decErrorPixels));
    if (guide.pulses > 0 && guide.lastPulse)
    {
        guiding += QString("; %1 pulses, last %2 %3 ms at %4 UTC")
                       .arg(guide.pulses)
                       .arg(guideDirectionName(guide.lastPulse->direction))
                       .arg(guide.lastPulse->milliseconds)
                       .arg(QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(
                                guide.lastPulseTime.time_since_epoch()).count(), QTimeZone::UTC).toString("HH:mm:ss"));
    }
    guiding_->setText(guiding);
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
        return;
    }
    const double pixels = tolerancePercent() / 100.0 * frameShortSide_;
    QString text = QString("%1 px").arg(pixels, 0, 'f', 1);
    if (arcsecPerPixel_) text += QString(", %1″").arg(pixels * *arcsecPerPixel_, 0, 'f', 1);
    toleranceReadout_->setText(text);
}

void GuidingPanel::setAvailable(bool running, bool pixelSelected)
{
    lockButton_->setEnabled(running && pixelSelected);
}

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
