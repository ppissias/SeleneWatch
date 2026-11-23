#include "ui/cameras/playerone_camera_panel.h"
#include "ui/cameras/camera_settings_editor.h"
#include "ui/cameras/runtime_chooser.h"
#include "ui/widgets/theme.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

PlayerOneCameraPanel::PlayerOneCameraPanel(std::shared_ptr<PlayerOneApi> api, QWidget* parent)
    : CameraFamilyPanel(parent), api_(std::move(api))
{
    setObjectName("playerOneCameraPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(5);
    runtime_ = new RuntimeChooser("Player One", "PlayerOneCamera.dll", "playerone/runtimePath", "PlayerOne", this);
    layout->addWidget(runtime_);
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(4);
    cameras_ = new QComboBox(this);
    cameras_->setObjectName("playerOneCameras");
    cameras_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    cameras_->setMinimumContentsLength(14);
    cameras_->setToolTip("The connected Player One camera to use.");
    refresh_ = new QPushButton("Refresh", this);
    refresh_->setObjectName("refreshPlayerOneCameras");
    refresh_->setToolTip("Searches again for connected Player One cameras.");
    topRow->addWidget(cameras_, 1);
    topRow->addWidget(refresh_);
    layout->addLayout(topRow);
    editor_ = new CameraSettingsEditor(4, 2, this);
    layout->addWidget(editor_);
    apply_ = new QPushButton("Apply settings", this);
    apply_->setObjectName("applyPlayerOneCamera");
    apply_->setToolTip("Sends format, binning and typed ROI edits to the camera (restarts Preview while running). Live controls, Draw ROI, Center, Full and presets apply automatically.");
    layout->addWidget(apply_);
    readback_ = new QLabel("Refresh to discover Player One cameras.", this);
    readback_->setWordWrap(true);
    readback_->setTextFormat(Qt::PlainText);
    readback_->setObjectName("playerOneReadback");
    layout->addWidget(readback_);
    layout->addStretch();
    connect(runtime_, &RuntimeChooser::changed, this, &PlayerOneCameraPanel::refresh);
    connect(refresh_, &QPushButton::clicked, this, &PlayerOneCameraPanel::refresh);
    connect(cameras_, &QComboBox::currentIndexChanged, this, &PlayerOneCameraPanel::inspectSelected);
    connect(apply_, &QPushButton::clicked, this, &PlayerOneCameraPanel::applyRequested);
    connect(editor_, &CameraSettingsEditor::liveControlsRequested, this, &PlayerOneCameraPanel::liveControlsRequested);
    connect(editor_, &CameraSettingsEditor::geometryDirtyChanged, this, [this](bool dirty) {
        apply_->setStyleSheet(dirty && apply_->isEnabled() ? dirtyApplyStyle() : QString());
        apply_->setText(running_ ? (dirty ? "Apply ROI / format*" : "Apply ROI / format")
                                 : (dirty ? "Apply settings*" : "Apply settings"));
    });
    setRunning(false);
}

void PlayerOneCameraPanel::refresh()
{
    if (running_) return;
    profile_.reset();
    try
    {
        {
            const QSignalBlocker blocked(cameras_);
            cameras_->clear();
            api_->setRuntimeOverride(runtime_->path());
            for (const auto& c : api_->cameras())
                cameras_->addItem(QString("%1 (%2)").arg(QString::fromStdString(c.name), QString::fromStdString(c.serial)), c.id);
        }
        readback_->setToolTip(api_->loadedRuntimePath());
        if (cameras_->count())
        {
            inspectSelected();
            return;
        }
        readback_->setText(QString("Player One SDK %1 loaded. Connect a Player One camera and refresh. If Windows cannot see it, "
            "check Player One's camera driver.").arg(QString::fromStdString(api_->version())));
    }
    catch (const std::exception& ex)
    {
        readback_->setToolTip(api_->loadedRuntimePath());
        readback_->setText(QString::fromUtf8(ex.what()));
    }
    setRunning(false);
    emit availabilityChanged();
}

void PlayerOneCameraPanel::inspectSelected()
{
    if (running_ || cameras_->currentIndex() < 0) return;
    try
    {
        profile_ = PlayerOneSource::inspect(api_, cameras_->currentData().toInt());
        showProfile();
        // Start with maximum USB bandwidth (when writable) for max FPS on connect.
        for (const auto& c : profile_->controls)
        {
            if (c.writable && c.kind == PlayerOneControl::UsbBandwidth)
                editor_->setControlValue(static_cast<int>(c.kind), static_cast<double>(c.maximum));
        }
    }
    catch (const std::exception& ex) { profile_.reset(); readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
    emit availabilityChanged();
}

void PlayerOneCameraPanel::validateSettings() const { PlayerOneSource::validateSettings(profile().camera, settings()); }

std::unique_ptr<FrameSource> PlayerOneCameraPanel::makeSource()
{
    auto source = std::make_unique<PlayerOneSource>(api_, profile(), settings());
    source_ = source.get();
    return source;
}

void PlayerOneCameraPanel::showSourceReadback(bool live)
{
    if (!source_) return;
    profile_ = source_->actualProfile();
    if (live) updateReadback();
    else { showProfile(); setRunning(running_, monitoring_); }
}

bool PlayerOneCameraPanel::readBack(std::string& error)
{
    PlayerOneSource source(api_, profile(), settings());
    if (!source.open(error)) return false;
    profile_ = source.actualProfile();
    showProfile();
    setRunning(running_, monitoring_);
    return true;
}

void PlayerOneCameraPanel::requestLiveControls()
{
    if (source_) source_->requestLiveControls(settings().controls);
}

CameraSupport PlayerOneCameraPanel::checkSupport()
{
    CameraSupport support;
    try
    {
        api_->setRuntimeOverride(runtime_->path());
        support.sdkVersion = QString::fromStdString(api_->version());
        for (const auto& c : api_->cameras()) support.cameras << QString::fromStdString(c.name);
    }
    catch (const std::exception& ex) { support.problem = QString::fromUtf8(ex.what()); }
    support.library = api_->loadedRuntimePath();
    return support;
}

QString PlayerOneCameraPanel::installHint() const
{
    return "Install Player One's camera driver, from player-one-astronomy.com/service/software. SeleneWatch finds the "
        "PlayerOneCamera.dll that N.I.N.A. or 64-bit SharpCap installs, the newest first. Otherwise download Player One's "
        "Windows camera SDK from the same page, and put its lib\\x64\\PlayerOneCamera.dll next to SeleneWatch.exe or choose it. "
        "A DLL next to SeleneWatch.exe is always used first.";
}

PlayerOneSettings PlayerOneCameraPanel::settings() const
{
    auto s = profile_.value().current;
    const auto g = editor_->requestedGeometry();
    s.width = g.width; s.height = g.height; s.x = g.x; s.y = g.y; s.bin = g.bin; s.format = g.format;
    for (const auto& [id, value] : editor_->controlValues())
        s.controls[static_cast<PlayerOneControl>(id)] = static_cast<int64_t>(std::llround(value));
    return s;
}

void PlayerOneCameraPanel::showProfile()
{
    const auto& p = *profile_;
    const auto& s = p.current;
    std::vector<int> bins;
    for (const auto bin : p.camera.bins)
        if (p.camera.bayer == BayerPattern::None || bin == 1) bins.push_back(bin);
    editor_->showGeometry(p.camera.width, p.camera.height, p.camera.formats, bins, {s.width, s.height, s.x, s.y, s.bin, s.format});
    editor_->clearControls();
    for (const auto& c : p.controls)
    {
        const bool exposure = c.kind == PlayerOneControl::Exposure;
        editor_->addControl(static_cast<int>(c.kind), QString::fromStdString(c.name), static_cast<double>(c.minimum),
            static_cast<double>(c.maximum), exposure ? 100 : 1, static_cast<double>(c.value), exposure ? 1000 : 1,
            exposure ? " ms" : "", c.writable);
    }
    updateReadback();
}

void PlayerOneCameraPanel::updateReadback()
{
    const auto& p = *profile_;
    const auto& s = p.current;
    QStringList values;
    for (const auto& c : p.controls)
    {
        const bool exposure = c.kind == PlayerOneControl::Exposure;
        values << QString("%1: %2%3").arg(QString::fromStdString(c.name))
            .arg(exposure ? static_cast<double>(c.value) / 1000 : static_cast<double>(c.value), 0, 'g', 10).arg(exposure ? " ms" : "");
    }
    const QString serial = p.camera.serial.empty() ? QString("unknown") : QString::fromStdString(p.camera.serial);
    const QString fmt = s.format == PixelFormat::Mono16 ? "RAW16" : s.format == PixelFormat::Mono8 ? "RAW8" : "processed";
    readback_->setText(QString("Actual: %1 × %2 · %3 · bin %4 · (%5, %6) · SN %7\n%8")
        .arg(s.width).arg(s.height).arg(fmt).arg(s.bin).arg(s.x).arg(s.y).arg(serial, values.join(", ")));
    readback_->setToolTip(QString("Sensor %1, ADC %2-bit · SDK %3 · %4\nHardware validation pending.")
        .arg(p.camera.sensor.empty() ? QString("unknown") : QString::fromStdString(p.camera.sensor))
        .arg(p.camera.sensorBits).arg(QString::fromStdString(p.sdkVersion), api_->loadedRuntimePath()));
}

void PlayerOneCameraPanel::setRunning(bool running, bool monitoring)
{
    running_ = running;
    monitoring_ = monitoring;
    refresh_->setEnabled(!running);
    cameras_->setEnabled(!running);
    runtime_->setRunning(running);
    editor_->setEnabled(hasCamera() && !monitoring);
    editor_->setLiveEditing(running && !monitoring);
    apply_->setEnabled(hasCamera() && !monitoring);
    const bool dirty = editor_->geometryDirty();
    apply_->setStyleSheet(dirty && apply_->isEnabled() ? dirtyApplyStyle() : QString());
    apply_->setText(running ? (dirty ? "Apply ROI / format*" : "Apply ROI / format")
                            : (dirty ? "Apply settings*" : "Apply settings"));
}
