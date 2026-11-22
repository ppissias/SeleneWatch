// Feature-32: the end of a session. The Ends row above the preview, the
// conditions that end Monitoring or a time-lapse by itself, and what the
// mount does then. The window keeps the running session's start; the
// conditions are plain functions (session_end.h).

#include "ui/mainwindow.h"

#include "ui/widgets/elided_label.h"
#include "mounts/simulated_mount.h"
#include "recording/metadata_text.h"
#include "ui/session_end_dialog.h"
#include "ui/timelapse_panel.h"

#include <QtCore/QDateTime>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

namespace {

constexpr int kMountUnset = 0, kMountKeep = 1, kMountStop = 2;

MountAtEnd mountFromSetting(int value)
{
    return value == kMountKeep ? MountAtEnd::KeepTracking : value == kMountStop ? MountAtEnd::StopTracking : MountAtEnd::Unset;
}

int mountToSetting(MountAtEnd mount)
{
    return mount == MountAtEnd::KeepTracking ? kMountKeep : mount == MountAtEnd::StopTracking ? kMountStop : kMountUnset;
}

} // namespace

QWidget* MainWindow::makeSessionEndRow(QWidget* parent)
{
    auto* row = new QWidget(parent);
    row->setObjectName("sessionEndRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    sessionEndSummary_ = new ElidedLabel({}, Qt::ElideRight, row);
    sessionEndSummary_->setObjectName("sessionEndSummary");
    sessionEndSummary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sessionEndWarning_ = new QLabel(row);
    sessionEndWarning_->setObjectName("sessionEndWarning");
    sessionEndWarning_->setStyleSheet("color: #b37900;");
    sessionEndWarning_->hide();
    sessionEndChange_ = new QPushButton("Change…", row);
    sessionEndChange_->setObjectName("sessionEndChange");
    sessionEndChange_->setToolTip("When Monitoring or a time-lapse ends by itself: at a local time, after a duration, at "
        "dawn, or when the Moon sinks below an altitude, whichever comes first. And, with a mount connected, whether it "
        "keeps tracking or stops tracking then, also when the session ends early (guiding lost, the camera lost, the "
        "disk full).");
    layout->addWidget(sessionEndSummary_, 1);
    layout->addWidget(sessionEndWarning_);
    layout->addWidget(sessionEndChange_);
    connect(sessionEndChange_, &QPushButton::clicked, this, &MainWindow::onChangeSessionEnd);
    return row;
}

void MainWindow::loadSessionEnd()
{
    QSettings saved;
    // Feature-15's Watch from dusk to dawn becomes the At dawn condition, once.
    if (!saved.contains("sessionEnd/atDawn") && saved.contains("sky/night/enabled"))
    {
        saved.setValue("sessionEnd/atDawn", saved.value("sky/night/enabled").toBool());
        saved.setValue("sessionEnd/sunLimit", saved.value("sky/night/sunLimit", -10.0).toDouble());
    }
    saved.remove("sky/night");
    SessionEndSettings settings;
    settings.atLocalTime = saved.value("sessionEnd/atLocalTime", settings.atLocalTime).toBool();
    const QTime time = QTime::fromString(saved.value("sessionEnd/localTime").toString(), "HH:mm");
    if (time.isValid()) settings.localTime = time;
    settings.afterDuration = saved.value("sessionEnd/afterDuration", settings.afterDuration).toBool();
    settings.durationMinutes = std::clamp(saved.value("sessionEnd/durationMinutes", settings.durationMinutes).toInt(), 1, 24 * 60 - 1);
    settings.atDawn = saved.value("sessionEnd/atDawn", settings.atDawn).toBool();
    settings.sunLimitDeg = std::clamp(saved.value("sessionEnd/sunLimit", settings.sunLimitDeg).toDouble(), -18.0, 0.0);
    settings.moonBelow = saved.value("sessionEnd/moonBelow", settings.moonBelow).toBool();
    settings.moonLimitDeg = std::clamp(saved.value("sessionEnd/moonLimit", settings.moonLimitDeg).toDouble(), 0.0, 60.0);
    sessionEnd_ = settings;
    mountAtEnd_ = mountFromSetting(saved.value("sessionEnd/mount", kMountUnset).toInt());

    sessionEndTimer_ = new QTimer(this);
    sessionEndTimer_->setInterval(1000);
    connect(sessionEndTimer_, &QTimer::timeout, this, [this] { checkSessionEnd(std::chrono::system_clock::now()); });
    sessionEndTimer_->start();
    checkSessionEnd(std::chrono::system_clock::now());
}

void MainWindow::applySessionEnd(const SessionEndSettings& settings, MountAtEnd mount)
{
    sessionEnd_ = settings;
    mountAtEnd_ = mount;
    QSettings saved;
    saved.setValue("sessionEnd/atLocalTime", settings.atLocalTime);
    saved.setValue("sessionEnd/localTime", settings.localTime.toString("HH:mm"));
    saved.setValue("sessionEnd/afterDuration", settings.afterDuration);
    saved.setValue("sessionEnd/durationMinutes", settings.durationMinutes);
    saved.setValue("sessionEnd/atDawn", settings.atDawn);
    saved.setValue("sessionEnd/sunLimit", settings.sunLimitDeg);
    saved.setValue("sessionEnd/moonBelow", settings.moonBelow);
    saved.setValue("sessionEnd/moonLimit", settings.moonLimitDeg);
    saved.setValue("sessionEnd/mount", mountToSetting(mount));
    checkSessionEnd(std::chrono::system_clock::now());
}

// The Moon's condition applies to the Moon and its time-lapse, not to a planet's.
bool MainWindow::moonEnds() const
{
    if (target_ != Target::Moon) return false;
    if (session_->mode() == Session::Mode::TimeLapse || (sessionRun_ && sessionRun_->timeLapse))
        return timelapsePanel_->settings().preset == TimelapsePreset::Moon;
    return true;
}

// The mount is the Moon's: the sky is watched with a fixed camera.
bool MainWindow::mountForEnd() const
{
    return target_ == Target::Moon && session_->guider().status().guide.mountConnected;
}

void MainWindow::onChangeSessionEnd()
{
    const GuideStatus guide = session_->guider().status().guide;
    SessionEndDialog::Context context;
    context.site = savedSite();
    context.moon = moonEnds();
    context.mountConnected = mountForEnd();
    context.canSetTracking = !guide.mountConnected || guide.mount.canSetTracking;
    context.running = sessionRun_.has_value();
    if (sessionRun_) context.start = sessionRun_->start;
    SessionEndDialog dialog(sessionEnd_, mountAtEnd_, context, this);
    if (dialog.exec() == QDialog::Accepted) applySessionEnd(dialog.settings(), dialog.mountAtEnd());
}

// The first start with a mount connected asks what it does at the end.
// False when the observer cancels the start.
bool MainWindow::askMountAtEnd()
{
    if (!mountForEnd() || mountAtEnd_ != MountAtEnd::Unset) return true;
    const bool canStop = session_->guider().status().guide.mount.canSetTracking;
    QMessageBox box(this);
    box.setObjectName("mountAtEndQuestion");
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("The mount after this session");
    box.setText("When this session ends, by its schedule or because something stopped it, what should the mount do?");
    box.setInformativeText(QString("You can change this later with Ends → Change…. Your own Stop leaves the mount as it is.")
        + (canStop ? QString() : "\n\nThis mount does not let SeleneWatch stop its tracking."));
    QPushButton* stop = box.addButton("Stop tracking", QMessageBox::AcceptRole);
    stop->setEnabled(canStop);
    QPushButton* keep = box.addButton("Keep tracking", QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == stop) applySessionEnd(sessionEnd_, MountAtEnd::StopTracking);
    else if (box.clickedButton() == keep) applySessionEnd(sessionEnd_, MountAtEnd::KeepTracking);
    else return false;
    return true;
}

void MainWindow::beginSessionRun(bool timeLapse)
{
    SessionRun run;
    run.timeLapse = timeLapse;
    run.start = std::chrono::system_clock::now();
    run.guideStops = session_->guider().status().guide.unplannedStops;
    if (timeLapse) run.folder = session_->timeLapseStatus().sessionDir;
    sessionRun_ = run;
    sessionRunGone_ = 0;
    endNote_.clear();
    checkSessionEnd(run.start);
}

void MainWindow::checkSessionEnd(std::chrono::system_clock::time_point now)
{
    if (sessionRun_)
    {
        // Ended without the window: a time-lapse's stack count, disk reserve
        // or failure, or a stream that stopped. A short gap is a restart.
        const auto mode = session_->mode();
        const bool still = session_->isRunning() && (sessionRun_->timeLapse
            ? mode == Session::Mode::TimeLapse && session_->timeLapseStatus().active
            : mode == Session::Mode::Monitoring || session_->modeChangePending());
        if (still)
        {
            sessionRunGone_ = 0;
            const auto end = plannedEnd(sessionEnd_, sessionRun_->start, savedSite(), moonEnds(), QTimeZone::systemTimeZone());
            if (end && now >= end->time) endSession(endCauseText(sessionEnd_, end->cause));
        }
        else if (++sessionRunGone_ >= 3)
        {
            QString why = "Monitoring stopped";
            if (sessionRun_->timeLapse)
            {
                const QString reason = session_->timeLapseStatus().stopReason;
                why = reason.isEmpty() ? QString("the time-lapse stopped") : "the time-lapse stopped: " + reason;
            }
            endSession(why);
        }
    }
    updateSessionEndRow(now);
}

void MainWindow::updateSessionEndRow(std::chrono::system_clock::time_point now)
{
    if (!sessionEndSummary_) return;
    const QTimeZone zone = QTimeZone::systemTimeZone();
    const ObservingSite site = savedSite();
    const bool moon = moonEnds();
    const auto start = sessionRun_ ? sessionRun_->start : now;
    const auto end = plannedEnd(sessionEnd_, start, site, moon, zone);
    QString text;
    if (end && sessionRun_)
        text = QString("Ends in %1: %2, %3").arg(durationText(std::chrono::duration_cast<std::chrono::seconds>(end->time - now)),
            localAndUtc(end->time, zone), endCauseText(sessionEnd_, end->cause));
    else if (end)
        text = QString("Ends: %1, %2").arg(localAndUtc(end->time, zone), endCauseText(sessionEnd_, end->cause));
    else if (sessionEnd_.any(moon))
        text = !site.hasPosition() ? "Ends: when you stop it (dawn and the Moon need the site's position)"
                                   : "Ends: when you stop it (not within two days)";
    else
        text = "Ends: when you stop it";
    QString warning;
    if (mountForEnd())
    {
        if (mountAtEnd_ == MountAtEnd::StopTracking) text += " · then the mount stops tracking";
        else if (mountAtEnd_ == MountAtEnd::KeepTracking) warning = "⚠ the mount keeps tracking";
        else warning = "⚠ choose what the mount does at the end";
    }
    sessionEndSummary_->setText(text);
    sessionEndSummary_->setToolTip(text + (warning.isEmpty() ? QString() : " · " + warning) + "\n\n"
        + "Monitoring or a time-lapse ends at the first condition met (Change…). Guiding lost, the camera lost or "
          "the disk full end it too. The mount's choice applies to every end that SeleneWatch makes by itself; your "
          "own Stop leaves the mount as it is.");
    sessionEndWarning_->setText(warning);
    sessionEndWarning_->setVisible(!warning.isEmpty());
}

// An end the window did not get from the observer: a condition met, a
// failure, the disk reserve, lost guiding. The stream stops (closing the
// camera, finishing the clip, the recording and the report, or building the
// time-lapse's video), then the mount is left as chosen.
void MainWindow::endSession(const QString& why)
{
    if (!sessionRun_) return;
    SessionRun run = *sessionRun_;
    sessionRun_.reset();
    if (run.timeLapse)
    {
        const QString folder = session_->timeLapseStatus().sessionDir;
        if (!folder.isEmpty()) run.folder = folder;
    }
    if (session_->isRunning()) finishStream(("session end: " + why).toStdString());
    if (!run.timeLapse) run.folder = session_->lastWatchFolder();

    const QString note = QString("%1: the session ended (%2).").arg(QDateTime::currentDateTime().toString("HH:mm"), why);
    endNote_ = note;
    if (mountForEnd())
    {
        if (mountAtEnd_ == MountAtEnd::StopTracking)
        {
            pendingMount_ = MountRecord{session_->guider().status().guide.trackingStops, run.timeLapse, run.folder, note};
            session_->guider().requestStopTracking();
            endNote_ += " Stopping the mount's tracking…";
        }
        else
        {
            const QString result = mountAtEnd_ == MountAtEnd::KeepTracking ? "kept tracking, as chosen"
                                                                           : "left as it was (no choice made)";
            recordMountAtEnd({0, run.timeLapse, run.folder, note}, result);
            endNote_ += " The mount " + result + ".";
        }
    }
    showMessage(endNote_);
    updateControls();
    updateStatusLabel();
}

// The guider's answer to a tracking stop. Called with each guider status.
void MainWindow::checkTrackingStop(const GuideStatus& guide)
{
    if (!pendingMount_ || guide.trackingStops <= pendingMount_->trackingStops) return;
    const MountRecord record = *pendingMount_;
    pendingMount_.reset();
    const QString result = QString::fromStdString(guide.trackingStopResult);
    recordMountAtEnd(record, result);
    endNote_ = record.note + " The mount: " + result + ".";
    showMessage(endNote_);
}

// What the mount did goes into the session's record: session.txt and its
// report for Monitoring, timelapse.txt once the time-lapse has written it.
void MainWindow::recordMountAtEnd(const MountRecord& record, const QString& result)
{
    if (record.folder.isEmpty()) return;
    if (!record.timeLapse)
    {
        const QString line = QString("%1 mount_at_end action=%2 result=\"%3\"\n")
            .arg(utcText(std::chrono::system_clock::now()), mountAtEndName(mountAtEnd_), result);
        session_->addToRecord(record.folder, "session.txt", line, true);
        return;
    }
    timeLapseMountLine_ = std::pair{record.folder, QString("mount_at_end: %1, %2\n").arg(mountAtEndName(mountAtEnd_), result)};
    if (!session_->timeLapseFinalizing() && session_->mode() != Session::Mode::TimeLapse) writeTimeLapseMountLine(record.folder);
}

void MainWindow::writeTimeLapseMountLine(const QString& folder)
{
    if (!timeLapseMountLine_ || timeLapseMountLine_->first != folder) return;
    session_->addToRecord(folder, "timelapse.txt", timeLapseMountLine_->second, false);
    timeLapseMountLine_.reset();
}
