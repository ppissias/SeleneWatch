// Feature-47: the status page for a phone on the local network. The window
// keeps the server and fills the page's snapshot from what it already shows;
// the page and the server are in remote/.

#include "ui/mainwindow.h"

#include "selenewatch_version.h"
#include "recording/clip_overview.h"
#include "remote/status_server.h"
#include "ui/detection_panel.h"
#include "ui_mainwindow.h"
#include "ui/widgets/elided_label.h"
#include "ui/preview_widget.h"

#include <QtCore/QBuffer>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStorageInfo>
#include <QtCore/QTimeZone>

namespace {

constexpr quint16 kDefaultPort = 8080;
// The page's pictures are made for a phone's width.
constexpr int kImageWidth = 900;
constexpr int kPreviewWidth = 720;
constexpr auto kPreviewReuse = std::chrono::seconds(5);

QByteArray encoded(const QImage& image, const char* format, int quality = -1)
{
    if (image.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format, quality) ? bytes : QByteArray();
}

QImage narrowed(const QImage& image, int width)
{
    return image.width() > width ? image.scaledToWidth(width, Qt::SmoothTransformation) : image;
}

QDateTime utc(std::chrono::system_clock::time_point time)
{
    return QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count(),
        QTimeZone::UTC);
}

QString seconds(double value)
{
    const int s = static_cast<int>(value + 0.5);
    return s >= 60 ? QString("%1 min %2 s").arg(s / 60).arg(s % 60) : QString("%1 s").arg(s);
}

} // namespace

void MainWindow::setupRemoteStatus()
{
    RemoteSources sources;
    sources.status = [this] { return remoteStatus(); };
    sources.lastClipImage = [this] { return remoteLastClipImage(); };
    sources.lastStack = [this] { return remoteLastStack(); };
    sources.preview = [this] { return remotePreview(); };
    remoteServer_ = new StatusServer(std::move(sources), this);
    QSettings settings;
    if (settings.value("remote/key").toString().isEmpty()) settings.setValue("remote/key", newAccessKey());
    if (settings.value("remote/enabled", false).toBool())
    {
        const QString error = applyRemoteStatus({true, static_cast<quint16>(settings.value("remote/port", kDefaultPort).toUInt()),
            settings.value("remote/key").toString()});
        if (!error.isEmpty()) showMessage("The status page for your phone did not start: " + error);
    }
}

void MainWindow::openRemoteStatus()
{
    if (!remoteDialog_)
    {
        const QSettings settings;
        remoteDialog_ = new RemoteStatusDialog({settings.value("remote/enabled", false).toBool(),
            static_cast<quint16>(settings.value("remote/port", kDefaultPort).toUInt()), settings.value("remote/key").toString()},
            [this](const RemoteStatusDialog::Settings& chosen) { return applyRemoteStatus(chosen); }, this);
    }
    remoteDialog_->show();
    remoteDialog_->raise();
    remoteDialog_->activateWindow();
}

QString MainWindow::applyRemoteStatus(const RemoteStatusDialog::Settings& chosen)
{
    QSettings settings;
    settings.setValue("remote/enabled", chosen.enabled);
    settings.setValue("remote/port", chosen.port);
    settings.setValue("remote/key", chosen.key);
    if (!chosen.enabled)
    {
        remoteServer_->stop();
        return {};
    }
    if (remoteServer_->listening() && remoteServer_->port() == chosen.port)
    {
        remoteServer_->setKey(chosen.key);
        return {};
    }
    QString error;
    if (!remoteServer_->start(chosen.port, chosen.key, error)) return QString("port %1: %2").arg(chosen.port).arg(error);
    return {};
}

RemoteStatus MainWindow::remoteStatus() const
{
    RemoteStatus s;
    s.software = QString("SeleneWatch %1").arg(SELENEWATCH_VERSION);
    s.updated = QDateTime::currentDateTimeUtc();
    s.state = statusLabel_->text().remove("Status: ");
    s.detail = modeLabel_->text();
    s.running = session_->isRunning();
    s.problem = sourceFailed_ || recordFailed_;
    s.watch = detectionPanel_->watchTitle();
    const StreamInfo stream = session_->streamInfo();
    s.source = !stream.camera.empty() ? QString::fromStdString(stream.camera)
                                      : QFileInfo(QString::fromStdString(stream.source)).fileName();
    if (sessionRun_) s.since = utc(sessionRun_->start);
    if (sessionEndSummary_) s.ends = sessionEndSummary_->text();
    if (s.running)
    {
        s.fps = measuredFps_;
        s.frames = session_->acquiredFrames();
        if (activeCamera()) s.dropped = session_->droppedFrames();
    }

    s.candidates = remoteCounts_.candidates;
    s.clipsSaved = remoteCounts_.saved;
    s.clipsNotSaved = remoteCounts_.notSaved;
    s.groups = remoteCounts_.groups;
    s.lastClip = remoteCounts_.lastClip;
    s.hasLastClipImage = !remoteCounts_.lastClipFolder.isEmpty()
        && QFileInfo::exists(remoteCounts_.lastClipFolder + "/00_overview.png");

    const GuiderStatus guider = session_->guider().status();
    s.lock = lockStateName(guider.lock.state);
    s.locked = guider.lock.state == LockState::Locked;
    if (s.locked && guider.lock.estimateValid) s.drift = QPointF(guider.lock.estimateDx, guider.lock.estimateDy);
    s.guiding = guider.guide.mode == GuideMode::Idle ? QString() : QString(guideModeName(guider.guide.mode));
    if (guider.guide.mountConnected) s.mount = QString::fromStdString(guider.guide.mountName);
    if (guider.guide.lastPulse) s.lastCorrection = utc(guider.guide.lastPulseTime);
    s.guideMessage = QString::fromStdString(guider.guide.message);

    if (session_->mode() == Session::Mode::TimeLapse)
    {
        const TimelapseStatus lapse = session_->timeLapseStatus();
        s.stacks = lapse.stacksCompleted;
        s.timelapse = QString("%1 %2").arg(lapse.stacksCompleted).arg(lapse.stacksCompleted == 1 ? "stack" : "stacks");
        if (lapse.periodsSkipped) s.timelapse += QString(", %1 skipped").arg(lapse.periodsSkipped);
        if (lapse.active && !lapse.finalizing) s.timelapse += ", next burst in " + seconds(lapse.nextBurstInSeconds);
        if (!lapse.warningText.isEmpty()) s.timelapse += " · " + lapse.warningText;
        s.hasLastStack = !lapse.latestThumbnail.isNull();
    }

    const QStorageInfo drive(session_->outputRoot());
    if (drive.isValid() && drive.isReady()) s.freeBytes = drive.bytesAvailable();
    s.message = lastMessage_;
    if (!lastMessage_.isEmpty()) s.messageTime = lastMessageTime_;
    s.hasPreview = s.running && !ui_->previewWidget->image().isNull();
    return s;
}

// The last clip's row of crops (before, peak, after, change) from its
// overview, cut where overviewLayout puts it; the whole overview when the
// row is not there. Made once per clip, when the page first asks.
QByteArray MainWindow::remoteLastClipImage()
{
    const QString folder = remoteCounts_.lastClipFolder;
    if (folder.isEmpty()) return {};
    if (folder == remoteClipImageFolder_) return remoteClipImage_;
    QImage overview(folder + "/00_overview.png");
    if (overview.isNull()) return {};
    const QSize frame = remoteCounts_.lastClipFrame;
    if (frame.width() > 0 && frame.height() > 0)
    {
        const OverviewLayout layout = overviewLayout(frame.width(), frame.height());
        if (layout.crops.height() > 0 && overview.rect().contains(layout.crops)) overview = overview.copy(layout.crops);
    }
    remoteClipImageFolder_ = folder;
    remoteClipImage_ = encoded(narrowed(overview, kImageWidth), "PNG");
    return remoteClipImage_;
}

QByteArray MainWindow::remoteLastStack() const
{
    if (session_->mode() != Session::Mode::TimeLapse) return {};
    return encoded(narrowed(session_->timeLapseStatus().latestThumbnail, kImageWidth), "PNG");
}

// The preview as the screen shows it, at most every 5 s however often the
// page asks.
QByteArray MainWindow::remotePreview()
{
    if (!session_->isRunning()) return {};
    const auto now = std::chrono::steady_clock::now();
    if (remotePreview_.isEmpty() || now - remotePreviewTime_ >= kPreviewReuse)
    {
        remotePreview_ = encoded(narrowed(ui_->previewWidget->image(), kPreviewWidth), "JPG", 75);
        remotePreviewTime_ = now;
    }
    return remotePreview_;
}
