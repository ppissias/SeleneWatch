#include "session_end_dialog.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QVBoxLayout>

namespace {

QLabel* reading(QWidget* parent, const char* name)
{
    auto* label = new QLabel(parent);
    label->setObjectName(name);
    label->setStyleSheet("color: palette(mid);");
    return label;
}

} // namespace

SessionEndDialog::SessionEndDialog(const SessionEndSettings& settings, MountAtEnd mount, Context context, QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
    , initialMount_(mount)
{
    setObjectName("sessionEndDialog");
    setWindowTitle("When the session ends");

    auto* conditions = new QGroupBox("End at whichever comes first", this);
    auto* grid = new QGridLayout(conditions);

    atTime_ = new QCheckBox("At a local time", conditions);
    atTime_->setObjectName("sessionEndAtTime");
    atTime_->setToolTip("The session ends the next time this computer's clock shows this time.");
    time_ = new QTimeEdit(settings.localTime, conditions);
    time_->setObjectName("sessionEndTime");
    time_->setDisplayFormat("HH:mm");
    time_->setToolTip("Local time, on this computer's clock. The UTC time is shown beside it.");
    timeReading_ = reading(conditions, "sessionEndTimeReading");

    after_ = new QCheckBox("After", conditions);
    after_->setObjectName("sessionEndAfter");
    after_->setToolTip("The session ends this long after it started.");
    duration_ = new QTimeEdit(QTime(settings.durationMinutes / 60, settings.durationMinutes % 60), conditions);
    duration_->setObjectName("sessionEndDuration");
    duration_->setDisplayFormat("H 'h' mm 'min'");
    duration_->setMinimumTime(QTime(0, 1));
    duration_->setToolTip("How long the session lasts, from its start: up to 23 h 59 min.");
    durationReading_ = reading(conditions, "sessionEndDurationReading");

    atDawn_ = new QCheckBox("At dawn", conditions);
    atDawn_->setObjectName("sessionEndAtDawn");
    atDawn_->setToolTip("The session ends when the Sun rises above the altitude. Needs the site's position "
        "(Session → Site and instrument).");
    sunLimit_ = new QDoubleSpinBox(conditions);
    sunLimit_->setObjectName("sessionEndSunLimit");
    sunLimit_->setRange(-18, 0);
    sunLimit_->setDecimals(0);
    sunLimit_->setPrefix("Sun above ");
    sunLimit_->setSuffix("°");
    sunLimit_->setValue(settings.sunLimitDeg);
    sunLimit_->setToolTip("Dawn is when the Sun's altitude rises above this. -6 degrees is civil twilight, -12 nautical, "
        "-18 astronomical.");
    dawnReading_ = reading(conditions, "sessionEndDawnReading");

    moonBelow_ = new QCheckBox("When the Moon is below", conditions);
    moonBelow_->setObjectName("sessionEndMoonBelow");
    moonBelow_->setToolTip("The session ends when the Moon sinks below the altitude: the natural end of an evening "
        "crescent. Needs the site's position (Session → Site and instrument).");
    moonLimit_ = new QDoubleSpinBox(conditions);
    moonLimit_->setObjectName("sessionEndMoonLimit");
    moonLimit_->setRange(0, 60);
    moonLimit_->setDecimals(0);
    moonLimit_->setSuffix("°");
    moonLimit_->setValue(settings.moonLimitDeg);
    moonLimit_->setToolTip("The Moon's altitude above the horizon, as seen from the site, without refraction.");
    moonReading_ = reading(conditions, "sessionEndMoonReading");

    const auto row = [&](int at, QCheckBox* box, QWidget* editor, QLabel* label) {
        grid->addWidget(box, at, 0);
        grid->addWidget(editor, at, 1);
        grid->addWidget(label, at, 2);
    };
    row(0, atTime_, time_, timeReading_);
    row(1, after_, duration_, durationReading_);
    row(2, atDawn_, sunLimit_, dawnReading_);
    row(3, moonBelow_, moonLimit_, moonReading_);
    grid->setColumnStretch(2, 1);
    for (QWidget* widget : std::initializer_list<QWidget*>{moonBelow_, moonLimit_, moonReading_})
        widget->setVisible(context_.moon);
    atTime_->setChecked(settings.atLocalTime);
    after_->setChecked(settings.afterDuration);
    atDawn_->setChecked(settings.atDawn);
    moonBelow_->setChecked(settings.moonBelow);

    result_ = new QLabel(this);
    result_->setObjectName("sessionEndResult");
    result_->setWordWrap(true);

    mount_ = new QGroupBox("Then the mount", this);
    mount_->setObjectName("sessionEndMount");
    stopTracking_ = new QRadioButton("Stops tracking", mount_);
    stopTracking_->setObjectName("mountStopsTracking");
    stopTracking_->setToolTip("When the session ends by itself, SeleneWatch stops the mount's tracking. It never slews or "
        "parks it.");
    keepTracking_ = new QRadioButton("Keeps tracking", mount_);
    keepTracking_->setObjectName("mountKeepsTracking");
    keepTracking_->setToolTip("The mount is left tracking when the session ends.");
    keepWarning_ = new QLabel("⚠ On a German equatorial mount it can reach the pier. Set the meridian limits in the "
        "mount's driver.", mount_);
    keepWarning_->setObjectName("mountKeepWarning");
    keepWarning_->setWordWrap(true);
    keepWarning_->setStyleSheet("color: #b37900;");
    auto* applies = new QLabel("This also applies when the session ends by itself early: guiding lost, the camera lost, "
        "the disk full. Your own Stop leaves the mount as it is.", mount_);
    applies->setWordWrap(true);
    auto* mountLayout = new QVBoxLayout(mount_);
    mountLayout->addWidget(stopTracking_);
    mountLayout->addWidget(keepTracking_);
    mountLayout->addWidget(keepWarning_);
    mountLayout->addWidget(applies);
    stopTracking_->setChecked(mount == MountAtEnd::StopTracking);
    keepTracking_->setChecked(mount == MountAtEnd::KeepTracking);
    if (!context_.canSetTracking)
    {
        stopTracking_->setEnabled(false);
        stopTracking_->setText("Stops tracking (this mount does not let its tracking be set)");
        if (mount == MountAtEnd::StopTracking) keepTracking_->setChecked(true);
    }
    mount_->setVisible(context_.mountConnected);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(conditions);
    layout->addWidget(result_);
    layout->addWidget(mount_);
    layout->addWidget(buttons);

    for (QCheckBox* box : {atTime_, after_, atDawn_, moonBelow_})
        connect(box, &QCheckBox::toggled, this, [this] { update(); });
    for (QTimeEdit* edit : {time_, duration_})
        connect(edit, &QTimeEdit::timeChanged, this, [this] { update(); });
    for (QDoubleSpinBox* spin : {sunLimit_, moonLimit_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { update(); });
    for (QRadioButton* radio : {stopTracking_, keepTracking_})
        connect(radio, &QRadioButton::toggled, this, [this] { update(); });
    update();
}

SessionEndSettings SessionEndDialog::settings() const
{
    SessionEndSettings settings;
    settings.atLocalTime = atTime_->isChecked();
    settings.localTime = time_->time();
    settings.afterDuration = after_->isChecked();
    settings.durationMinutes = duration_->time().hour() * 60 + duration_->time().minute();
    settings.atDawn = atDawn_->isChecked();
    settings.sunLimitDeg = sunLimit_->value();
    settings.moonBelow = moonBelow_->isChecked();
    settings.moonLimitDeg = moonLimit_->value();
    return settings;
}

MountAtEnd SessionEndDialog::mountAtEnd() const
{
    if (stopTracking_->isChecked()) return MountAtEnd::StopTracking;
    if (keepTracking_->isChecked()) return MountAtEnd::KeepTracking;
    return initialMount_ == MountAtEnd::StopTracking && !context_.canSetTracking ? MountAtEnd::KeepTracking : initialMount_;
}

void SessionEndDialog::update()
{
    const SessionEndSettings current = settings();
    // Each condition's own time, ticked or not, so the observer sees what it would do.
    const auto when = [&](EndCause cause) {
        SessionEndSettings one = current;
        one.atLocalTime = cause == EndCause::LocalTime;
        one.afterDuration = cause == EndCause::Duration;
        one.atDawn = cause == EndCause::Dawn;
        one.moonBelow = cause == EndCause::MoonBelow;
        return endTime(one, cause, context_.start, context_.site, context_.moon, context_.zone);
    };
    const auto local = [&](std::chrono::system_clock::time_point time) {
        return localAndUtc(time, context_.zone);
    };
    if (const auto at = when(EndCause::LocalTime)) timeReading_->setText("= " + local(*at).section('(', 1).chopped(1));
    if (const auto at = when(EndCause::Duration))
        durationReading_->setText((context_.running ? "until " : "from now until ") + local(*at));
    const QString noPosition = "needs the site's position";
    const auto skyReading = [&](EndCause cause, const char* what) {
        if (!context_.site.hasPosition()) return noPosition;
        const auto at = when(cause);
        return at ? QString("%1: %2").arg(what, local(*at)) : QString("not within two days");
    };
    dawnReading_->setText(skyReading(EndCause::Dawn, "dawn"));
    moonReading_->setText(skyReading(EndCause::MoonBelow, "the Moon there"));

    const auto end = plannedEnd(current, context_.start, context_.site, context_.moon, context_.zone);
    if (end)
        result_->setText(QString("→ Ends at %1: %2.").arg(local(end->time), endCauseText(current, end->cause)));
    else if (current.any(context_.moon))
        result_->setText("→ None of the ticked conditions can be met (they need the site's position, or are not met "
                         "within two days): it runs until you stop it.");
    else
        result_->setText("→ Runs until you stop it.");
    keepWarning_->setVisible(keepTracking_->isChecked());
}
