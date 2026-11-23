#include "ui/cameras/touptek_camera_panel.h"
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

ToupTekCameraPanel::ToupTekCameraPanel(std::shared_ptr<ToupTekApi> api, QWidget* parent)
    : CameraFamilyPanel(parent), api_(std::move(api))
{
    setObjectName("toupTekCameraPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(5);
    runtime_ = new RuntimeChooser("ToupTek", "toupcam.dll", "touptek/runtimePath", "ToupTek", this);
    layout->addWidget(runtime_);
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(4);
    cameras_ = new QComboBox(this);
    cameras_->setObjectName("toupTekCameras");
    cameras_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    cameras_->setMinimumContentsLength(14);
    cameras_->setToolTip("The connected ToupTek camera to use.");
    refresh_ = new QPushButton("Refresh", this);
    refresh_->setObjectName("refreshToupTekCameras");
    refresh_->setToolTip("Searches again for connected ToupTek cameras.");
    topRow->addWidget(cameras_, 1);
    topRow->addWidget(refresh_);
    layout->addLayout(topRow);
    editor_ = new CameraSettingsEditor(RoiConstraints{2, 2, 2, 2, 8, 8, 1}, this);
    layout->addWidget(editor_);
    apply_ = new QPushButton("Apply settings", this);
    apply_->setObjectName("applyToupTekCamera");
    apply_->setToolTip("Sends format and typed ROI edits to the camera (restarts Preview while running). Live controls, Draw ROI, Center, Full and presets apply automatically. Bin 1 only.");
    layout->addWidget(apply_);
    readback_ = new QLabel("Refresh to discover ToupTek cameras.", this);
    readback_->setWordWrap(true);
    readback_->setTextFormat(Qt::PlainText);
    readback_->setObjectName("toupTekReadback");
    layout->addWidget(readback_);
    layout->addStretch();
    connect(runtime_, &RuntimeChooser::changed, this, &ToupTekCameraPanel::refresh);
    connect(refresh_, &QPushButton::clicked, this, &ToupTekCameraPanel::refresh);
    connect(cameras_, &QComboBox::currentIndexChanged, this, &ToupTekCameraPanel::inspectSelected);
    connect(apply_, &QPushButton::clicked, this, &ToupTekCameraPanel::applyRequested);
    connect(editor_, &CameraSettingsEditor::liveControlsRequested, this, &ToupTekCameraPanel::liveControlsRequested);
    connect(editor_, &CameraSettingsEditor::geometryDirtyChanged, this, [this](bool dirty) {
        apply_->setStyleSheet(dirty && apply_->isEnabled() ? dirtyApplyStyle() : QString());
        apply_->setText(running_ ? (dirty ? "Apply ROI / format*" : "Apply ROI / format")
                                 : (dirty ? "Apply settings*" : "Apply settings"));
    });
    setRunning(false);
}

void ToupTekCameraPanel::refresh()
{
    if (running_) return;
    profile_.reset();
    try
    {
        {
            const QSignalBlocker blocked(cameras_);
            cameras_->clear();
            api_->setRuntimeOverride(runtime_->path());
            for (const auto& d : api_->devices())
                cameras_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
        }
        readback_->setToolTip(api_->loadedRuntimePath());
        if (cameras_->count())
        {
            inspectSelected();
            return;
        }
        readback_->setText(QString("ToupTek SDK %1 loaded. Connect a camera and refresh. If Windows cannot see it, check the "
            "camera's driver.").arg(QString::fromStdString(api_->version())));
    }
    catch (const std::exception& ex)
    {
        readback_->setToolTip(api_->loadedRuntimePath());
        readback_->setText(QString::fromUtf8(ex.what()));
    }
    setRunning(false);
    emit availabilityChanged();
}

void ToupTekCameraPanel::inspectSelected()
{
    if (running_ || cameras_->currentIndex() < 0) return;
    try { profile_ = ToupTekSource::inspect(api_, cameras_->currentData().toString().toStdString()); showProfile(); }
    catch (const std::exception& ex) { profile_.reset(); readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
    emit availabilityChanged();
}

void ToupTekCameraPanel::validateSettings() const { ToupTekSource::validateSettings(profile().camera, settings()); }

std::unique_ptr<FrameSource> ToupTekCameraPanel::makeSource()
{
    auto source = std::make_unique<ToupTekSource>(api_, profile(), settings());
    source_ = source.get();
    return source;
}

void ToupTekCameraPanel::showSourceReadback(bool live)
{
    if (!source_) return;
    profile_ = source_->actualProfile();
    if (live) updateReadback();
    else { showProfile(); setRunning(running_, monitoring_); }
}

bool ToupTekCameraPanel::readBack(std::string& error)
{
    ToupTekSource source(api_, profile(), settings());
    if (!source.open(error)) return false;
    profile_ = source.actualProfile();
    showProfile();
    setRunning(running_, monitoring_);
    return true;
}

void ToupTekCameraPanel::requestLiveControls()
{
    if (source_) source_->requestLiveControls(settings().controls);
}

CameraSupport ToupTekCameraPanel::checkSupport()
{
    CameraSupport support;
    try
    {
        api_->setRuntimeOverride(runtime_->path());
        support.sdkVersion = QString::fromStdString(api_->version());
        for (const auto& d : api_->devices()) support.cameras << QString::fromStdString(d.name);
    }
    catch (const std::exception& ex) { support.problem = QString::fromUtf8(ex.what()); }
    support.library = api_->loadedRuntimePath();
    return support;
}

QString ToupTekCameraPanel::installHint() const
{
    return "Install the camera's driver, from ToupTek or the brand that sold it. SeleneWatch finds the toupcam.dll that "
        "N.I.N.A., 64-bit SharpCap or ToupTek's ASCOM driver installs, the newest first. Otherwise download ToupTek's camera "
        "SDK (ToupCamSDK) from touptekphotonics.com/download/?category=SDK, and put its win\\x64\\toupcam.dll next to "
        "SeleneWatch.exe or choose it. A DLL next to SeleneWatch.exe is always used first.";
}

ToupTekSettings ToupTekCameraPanel::settings() const
{
    auto s = profile_.value().current;
    const auto g = editor_->requestedGeometry();
    s.width = g.width; s.height = g.height; s.x = g.x; s.y = g.y; s.format = g.format;
    for (const auto& [id, value] : editor_->controlValues())
        s.controls[static_cast<ToupTekControl>(id)] = static_cast<int64_t>(std::llround(value));
    return s;
}

void ToupTekCameraPanel::showProfile()
{
    const auto& p = *profile_;
    const auto& s = p.current;
    editor_->showGeometry(p.camera.width, p.camera.height, p.camera.formats, {1}, {s.width, s.height, s.x, s.y, 1, s.format});
    editor_->clearControls();
    for (const auto& c : p.controls)
    {
        const bool exposure = c.kind == ToupTekControl::Exposure;
        editor_->addControl(static_cast<int>(c.kind), QString::fromStdString(c.name), static_cast<double>(c.minimum),
            static_cast<double>(c.maximum), exposure ? 100 : 1, static_cast<double>(c.value), exposure ? 1000 : 1, exposure ? " ms" : "");
    }
    updateReadback();
}

void ToupTekCameraPanel::updateReadback()
{
    const auto& p = *profile_;
    const auto& s = p.current;
    QStringList values;
    for (const auto& c : p.controls)
    {
        const bool exposure = c.kind == ToupTekControl::Exposure;
        values << QString("%1: %2%3").arg(QString::fromStdString(c.name))
            .arg(exposure ? static_cast<double>(c.value) / 1000 : static_cast<double>(c.value), 0, 'g', 10).arg(exposure ? " ms" : "");
    }
    const QString serial = p.serial.empty() ? QString("unknown") : QString::fromStdString(p.serial);
    readback_->setText(QString("Actual: %1 × %2 · RAW%3 · bin 1 · (%4, %5) · SN %6\n%7")
        .arg(s.width).arg(s.height).arg(s.format == PixelFormat::Mono16 ? 16 : 8).arg(s.x).arg(s.y).arg(serial, values.join(", ")));
    readback_->setToolTip(QString("Deepest raw: %1-bit · SDK %2 · %3\nBin 1 only; hardware validation pending.")
        .arg(p.camera.sensorBits).arg(QString::fromStdString(p.sdkVersion), api_->loadedRuntimePath()));
}

void ToupTekCameraPanel::setRunning(bool running, bool monitoring)
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
