#include "qhy_camera_panel.h"
#include "camera_settings_editor.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace {
constexpr auto runtimeSetting="qhy/runtimePath";
}

QhyCameraPanel::QhyCameraPanel(std::shared_ptr<QhyApi> api,QWidget* parent):QWidget(parent),api_(std::move(api))
{
    runtimeOverride_=QSettings().value(runtimeSetting).toString();
    setObjectName("qhyCameraPanel");auto* layout=new QVBoxLayout(this);
    auto* runtimeButtons=new QHBoxLayout;
    chooseRuntime_=new QPushButton("Choose QHY DLL…",this);
    chooseRuntime_->setObjectName("chooseQhyRuntime");
    automaticRuntime_=new QPushButton("Use automatic discovery",this);
    automaticRuntime_->setObjectName("automaticQhyRuntime");
    runtimeButtons->addWidget(chooseRuntime_);
    runtimeButtons->addWidget(automaticRuntime_);
    layout->addLayout(runtimeButtons);
    cameras_=new QComboBox(this);cameras_->setObjectName("qhyCameras");
    refresh_=new QPushButton("Refresh cameras",this);refresh_->setObjectName("refreshQhyCameras");
    layout->addWidget(cameras_);layout->addWidget(refresh_);
    editor_=new CameraSettingsEditor(1,1,this);layout->addWidget(editor_);
    apply_=new QPushButton("Apply settings",this);apply_->setObjectName("applyQhyCamera");layout->addWidget(apply_);
    readback_=new QLabel("Refresh to discover QHY cameras.",this);
    readback_->setWordWrap(true);
    readback_->setTextFormat(Qt::PlainText);
    readback_->setObjectName("qhyReadback");layout->addWidget(readback_);
    auto* hint=new QLabel("Preview applies supported controls live. Format and ROI changes restart Preview. Bin 1 only. QHY hardware validation is pending.",this);
    hint->setWordWrap(true);layout->addWidget(hint);layout->addStretch();
    connect(chooseRuntime_,&QPushButton::clicked,this,&QhyCameraPanel::chooseRuntime);
    connect(automaticRuntime_,&QPushButton::clicked,this,[this] { setRuntimeOverride({}); });
    connect(refresh_,&QPushButton::clicked,this,&QhyCameraPanel::refresh);
    connect(cameras_,&QComboBox::currentIndexChanged,this,&QhyCameraPanel::inspectSelected);
    connect(apply_,&QPushButton::clicked,this,&QhyCameraPanel::applyRequested);
    connect(editor_,&CameraSettingsEditor::liveControlsRequested,this,&QhyCameraPanel::liveControlsRequested);
    setRunning(false);
}
void QhyCameraPanel::chooseRuntime()
{
    if (running_) return;
    const auto directory=runtimeOverride_.isEmpty()?QString{}:QFileInfo(runtimeOverride_).absolutePath();
    const auto path=QFileDialog::getOpenFileName(this,"Select the installed x64 QHY runtime",directory,
        "QHY runtime (qhyccd.dll)");
    if (!path.isEmpty()) setRuntimeOverride(path);
}
// Remember the observer's chosen DLL path for the next discovery.
void QhyCameraPanel::setRuntimeOverride(QString path)
{
    if (running_) return;
    runtimeOverride_=std::move(path);
    QSettings saved;
    if (runtimeOverride_.isEmpty()) saved.remove(runtimeSetting);
    else saved.setValue(runtimeSetting,runtimeOverride_);
    saved.sync();
    refresh();
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
            api_->setRuntimeOverride(runtimeOverride_);
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
    try { profile_=QhySource::inspect(api_,cameras_->currentData().toString().toStdString());showProfile(); }
    catch(const std::exception& ex) { profile_.reset();readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
    emit availabilityChanged();
}
bool QhyCameraPanel::hasCamera() const { return profile_.has_value(); }
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
    QString readback=QString("Read back: %1 × %2, RAW%3, origin (%4, %5).\nBin 1 commanded. Sensor ADC: %6 bits. SDK %7.")
        .arg(s.width).arg(s.height).arg(s.format==PixelFormat::Mono16?16:8).arg(s.x).arg(s.y).arg(p.camera.sensorBits)
        .arg(QString::fromStdString(p.sdkVersion));
    QStringList values;
    for (const auto& c:p.controls)
    {
        const bool exposure=c.kind==QhyControl::Exposure;
        values << QString("%1: %2%3").arg(QString::fromStdString(c.name))
            .arg(exposure?c.value/1000:c.value,0,'g',10).arg(exposure?" ms":"");
    }
    readback_->setText(readback+"\nActual controls: "+values.join(", "));
}
void QhyCameraPanel::setRunning(bool running,bool monitoring)
{
    running_=running;
    monitoring_=monitoring;
    refresh_->setEnabled(!running);
    cameras_->setEnabled(!running);
    chooseRuntime_->setEnabled(!running);
    chooseRuntime_->setToolTip(runtimeOverride_.isEmpty()?"Using automatic QHY runtime discovery":QDir::toNativeSeparators(runtimeOverride_));
    automaticRuntime_->setEnabled(!running && !runtimeOverride_.isEmpty());
    editor_->setEnabled(hasCamera() && !monitoring);
    editor_->setLiveEditing(running && !monitoring);
    apply_->setEnabled(hasCamera() && !monitoring);
    apply_->setText(running?"Apply format / ROI and restart Preview":"Apply settings");
}
