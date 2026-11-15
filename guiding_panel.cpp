#include "guiding_panel.h"

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

namespace {

QString signedPixels(double value)
{
    return (value >= 0 ? "+" : "−") + QString::number(std::abs(value), 'f', 2);
}

} // namespace

GuidingPanel::GuidingPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* hint = new QLabel("Drag a rectangle on the image around a crater or other detail to lock on it, "
        "or click a pixel and press <b>Lock at selection</b>. The lock measures how far the field drifts; "
        "it does not move the mount.", this);
    hint->setWordWrap(true);

    state_ = new QLabel(this);
    state_->setObjectName("lockStateLabel");
    drift_ = new QLabel(this);
    drift_->setObjectName("lockDriftLabel");
    drift_->setToolTip("Median of the valid offsets of the last few seconds of source time, "
        "relative to where the feature was locked. Positive is right and down in the frame.");
    match_ = new QLabel(this);
    match_->setObjectName("lockMatchLabel");
    match_->setWordWrap(true);
    region_ = new QLabel(this);
    region_->setWordWrap(true);
    message_ = new QLabel(this);
    message_->setObjectName("lockMessageLabel");
    message_->setWordWrap(true);
    message_->setStyleSheet("color: #e0a030;");
    logProblem_ = new QLabel("guiding.txt could not be written. The lock continues; its record is incomplete.", this);
    logProblem_->setWordWrap(true);
    logProblem_->setStyleSheet("color: #ff6060;");
    logProblem_->hide();

    auto* form = new QFormLayout;
    form->addRow("State", state_);
    form->addRow("Drift", drift_);
    form->addRow("Last match", match_);
    form->addRow("Region", region_);

    lockButton_ = new QPushButton("Lock at selection", this);
    lockButton_->setObjectName("lockAtSelectionButton");
    lockButton_->setToolTip(QString("Lock on a %1 x %1 pixel box centred on the selected pixel.").arg(kDefaultLockSide));
    unlockButton_ = new QPushButton("Unlock", this);
    unlockButton_->setObjectName("unlockButton");
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(lockButton_);
    buttons->addWidget(unlockButton_);
    buttons->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addLayout(form);
    layout->addWidget(message_);
    layout->addWidget(logProblem_);
    layout->addLayout(buttons);
    layout->addStretch();

    connect(lockButton_, &QPushButton::clicked, this, &GuidingPanel::lockAtSelectionRequested);
    connect(unlockButton_, &QPushButton::clicked, this, &GuidingPanel::unlockRequested);
    setStatus({});
    setAvailable(false, false);
}

void GuidingPanel::setStatus(const LockStatus& status)
{
    locked_ = status.state != LockState::Off;
    unlockButton_->setEnabled(locked_);
    QString state = lockStateName(status.state);
    state[0] = state[0].toUpper();
    state_->setText("<b>" + state + "</b>");

    if (status.estimateValid)
    {
        drift_->setText(QString("dx %1 px, dy %2 px (median of %3)")
            .arg(signedPixels(status.estimateDx), signedPixels(status.estimateDy)).arg(status.estimateSamples));
    }
    else if (locked_)
    {
        drift_->setText(status.state == LockState::Locked ? "Collecting measurements" : "No estimate while the lock is lost");
    }
    else
    {
        drift_->setText("—");
    }

    if (status.lastMatch)
    {
        const LockMatch& match = *status.lastMatch;
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
            .arg(status.region.x).arg(status.region.y).arg(status.region.width).arg(status.region.height)
            .arg(status.binning).arg(status.quality.contrast, 0, 'f', 0).arg(status.quality.structureRatio, 0, 'f', 2));
    }
    else
    {
        region_->setText("—");
    }
    message_->setText(QString::fromStdString(status.message));
    message_->setVisible(!status.message.empty());
}

void GuidingPanel::setAvailable(bool running, bool pixelSelected)
{
    lockButton_->setEnabled(running && pixelSelected);
}

void GuidingPanel::setLogFailed(bool failed)
{
    logProblem_->setVisible(failed);
}
