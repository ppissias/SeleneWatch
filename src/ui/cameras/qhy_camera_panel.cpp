#include "ui/cameras/qhy_camera_panel.h"
#include "ui/cameras/camera_settings_editor.h"
#include "ui/cameras/runtime_chooser.h"
#include "ui/widgets/theme.h"
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

QhyCameraPanel::QhyCameraPanel(std::shared_ptr<QhyApi> api,QWidget* parent):CameraFamilyPanel(parent),api_(std::move(api))
{
    setObjectName("qhyCameraPanel");auto* layout=new QVBoxLayout(this);
    layout->setContentsMargins(6,6,6,6);
    layout->setSpacing(5);
    runtime_=new RuntimeChooser("QHY","qhyccd.dll","qhy/runtimePath","Qhy",this);
    layout->addWidget(runtime_);
    auto* topRow=new QHBoxLayout;
    topRow->setContentsMargins(0,0,0,0);
    topRow->setSpacing(4);
    cameras_=new QComboBox(this);cameras_->setObjectName("qhyCameras");
    cameras_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    cameras_->setMinimumContentsLength(14);
    cameras_->setToolTip("The connected QHY camera to use.");
    refresh_=new QPushButton("Refresh",this);refresh_->setObjectName("refreshQhyCameras");
    refresh_->setToolTip("Searches again for connected QHY cameras.");
    topRow->addWidget(cameras_,1);topRow->addWidget(refresh_);
    layout->addLayout(topRow);
    editor_=new CameraSettingsEditor(1,1,this);layout->addWidget(editor_);
    apply_=new QPushButton("Apply settings",this);apply_->setObjectName("applyQhyCamera");layout->addWidget(apply_);
    apply_->setToolTip("Sends format and typed ROI edits to the camera (restarts Preview while running). Live controls, Draw ROI, Center, Full and presets apply automatically. Bin 1 only.");
    readback_=new QLabel("Refresh to discover QHY cameras.",this);
    readback_->setWordWrap(true);
    readback_->setTextFormat(Qt::PlainText);
    readback_->setObjectName("qhyReadback");layout->addWidget(readback_);
    layout->addStretch();
    connect(runtime_,&RuntimeChooser::changed,this,&QhyCameraPanel::refresh);
    connect(refresh_,&QPushButton::clicked,this,&QhyCameraPanel::refresh);
    connect(cameras_,&QComboBox::currentIndexChanged,this,&QhyCameraPanel::inspectSelected);
    connect(apply_,&QPushButton::clicked,this,&QhyCameraPanel::applyRequested);
    connect(editor_,&CameraSettingsEditor::liveControlsRequested,this,&QhyCameraPanel::liveControlsRequested);
    connect(editor_,&CameraSettingsEditor::geometryDirtyChanged,this,[this](bool dirty) {
        apply_->setStyleSheet(dirty && apply_->isEnabled()?dirtyApplyStyle():QString());
        apply_->setText(running_?(dirty?"Apply ROI / format*":"Apply ROI / format"):(dirty?"Apply settings*":"Apply settings"));
    });
    setRunning(false);
}
// Resolve the runtime and scan only while the panel can use the disconnected SDK.
void QhyCameraPanel::refresh()
{
    if (running_) return;
    profile_.reset();
    try
    {
        {
            const QSignalBlocker blocked(cameras_);
            cameras_->clear();
            api_->setRuntimeOverride(runtime_->path());
            for (const auto& d:api_->devices())
                cameras_->addItem(QString("%1 (%2)").arg(QString::fromStdString(d.name),QString::fromStdString(d.id)),
                    QString::fromStdString(d.id));
        }
        readback_->setToolTip(api_->loadedRuntimePath());
        if (cameras_->count())
        {
            inspectSelected();
            return;
        }
        readback_->setText(QString("QHY SDK %1 loaded. Connect a QHY camera and refresh. If Windows cannot see the camera, check the QHY driver installation.")
            .arg(QString::fromStdString(api_->version())));
    }
    catch (const std::exception& ex)
    {
        readback_->setToolTip(api_->loadedRuntimePath());
        readback_->setText(QString::fromUtf8(ex.what()));
    }
    setRunning(false);
    emit availabilityChanged();
}
void QhyCameraPanel::inspectSelected()
{
    if(running_ || cameras_->currentIndex()<0) return;
    try
    {
        profile_=QhySource::inspect(api_,cameras_->currentData().toString().toStdString());
        showProfile();
        // Start with the fastest readout defaults (max transfer speed, minimum USB traffic / H-blank)
        // instead of the SDK's conservative boot defaults; observer edits and live readbacks still rule afterwards.
        for (const auto& c : profile_->controls)
        {
            if (c.kind == QhyControl::TransferSpeed)
                editor_->setControlValue(static_cast<int>(c.kind), c.maximum);
            else if (c.kind == QhyControl::UsbTraffic)
                editor_->setControlValue(static_cast<int>(c.kind), c.minimum);
        }
    }
    catch(const std::exception& ex) { profile_.reset();readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
    emit availabilityChanged();
}
bool QhyCameraPanel::hasCamera() const { return profile_.has_value(); }
void QhyCameraPanel::validateSettings() const { QhySource::validateSettings(profile().camera,settings()); }
std::unique_ptr<FrameSource> QhyCameraPanel::makeSource()
{
    auto source=std::make_unique<QhySource>(api_,profile(),settings());
    source_=source.get();
    return source;
}
void QhyCameraPanel::showSourceReadback(bool live)
{
    if (!source_) return;
    if (live) showLiveActual(source_->actualProfile());
    else showActual(source_->actualProfile());
}
bool QhyCameraPanel::readBack(std::string& error)
{
    QhySource source(api_,profile(),settings());
    if (!source.open(error)) return false;
    showActual(source.actualProfile());
    return true;
}
void QhyCameraPanel::requestLiveControls()
{
    if (source_) source_->requestLiveControls(settings().controls);
}
CameraSupport QhyCameraPanel::checkSupport()
{
    CameraSupport support;
    try
    {
        api_->setRuntimeOverride(runtime_->path());
        support.sdkVersion=QString::fromStdString(api_->version());
        for (const auto& d:api_->devices()) support.cameras << QString::fromStdString(d.name);
    }
    catch (const std::exception& ex) { support.problem=QString::fromUtf8(ex.what()); }
    support.library=api_->loadedRuntimePath();
    return support;
}
QString QhyCameraPanel::installHint() const
{
    return "Install QHY's Windows All-In-One package from qhyccd.com/download. It holds the camera driver and qhyccd.dll, "
        "which SeleneWatch finds where the package installs it. Or choose another x64 qhyccd.dll.";
}
QhyProfile QhyCameraPanel::profile() const { return profile_.value(); }
QhySettings QhyCameraPanel::settings() const
{
    auto s=profile_.value().current;const auto g=editor_->requestedGeometry();
    s.width=g.width;s.height=g.height;s.x=g.x;s.y=g.y;s.bin=g.bin;s.format=g.format;
    for(const auto& [id,value]:editor_->controlValues()) s.controls[static_cast<QhyControl>(id)]=value;
    return s;
}
void QhyCameraPanel::showActual(const QhyProfile& p) { profile_=p;showProfile();setRunning(running_,monitoring_); }
// A live readback updates the status while preserving the values being edited.
void QhyCameraPanel::showLiveActual(const QhyProfile& p) { profile_=p;updateReadback(); }
void QhyCameraPanel::showProfile()
{
    const auto& p=*profile_;const auto& s=p.current;
    editor_->showGeometry(p.camera.width,p.camera.height,p.camera.formats,{1},{s.width,s.height,s.x,s.y,1,s.format});
    editor_->clearControls();
    for(const auto& c:p.controls)
    {
        const bool exposure=c.kind==QhyControl::Exposure;
        editor_->addControl(static_cast<int>(c.kind),QString::fromStdString(c.name),c.minimum,c.maximum,c.step,c.value,exposure?1000:1,exposure?" ms":"");
    }
    updateReadback();
}
void QhyCameraPanel::updateReadback()
{
    const auto& p=*profile_;
    const auto& s=p.current;
    QStringList values;
    for (const auto& c:p.controls)
    {
        const bool exposure=c.kind==QhyControl::Exposure;
        values << QString("%1: %2%3").arg(QString::fromStdString(c.name))
            .arg(exposure?c.value/1000:c.value,0,'g',10).arg(exposure?" ms":"");
    }
    readback_->setText(QString("Actual: %1 × %2 · RAW%3 · bin 1 · (%4, %5) · %6-bit\n%7")
        .arg(s.width).arg(s.height).arg(s.format==PixelFormat::Mono16?16:8).arg(s.x).arg(s.y).arg(p.camera.sensorBits)
        .arg(values.join(", ")));
    readback_->setToolTip(QString("QHY SDK %1 · %2\nBin 1 only; hardware validation pending.")
        .arg(QString::fromStdString(p.sdkVersion),api_->loadedRuntimePath()));
}
void QhyCameraPanel::setRunning(bool running,bool monitoring)
{
    running_=running;
    monitoring_=monitoring;
    refresh_->setEnabled(!running);
    cameras_->setEnabled(!running);
    runtime_->setRunning(running);
    editor_->setEnabled(hasCamera() && !monitoring);
    editor_->setLiveEditing(running && !monitoring);
    apply_->setEnabled(hasCamera() && !monitoring);
    const bool dirty=editor_->geometryDirty();
    apply_->setStyleSheet(dirty && apply_->isEnabled()?dirtyApplyStyle():QString());
    apply_->setText(running?(dirty?"Apply ROI / format*":"Apply ROI / format"):(dirty?"Apply settings*":"Apply settings"));
}
