#include "qhy_camera_panel.h"
#include "camera_settings_editor.h"
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
QhyCameraPanel::QhyCameraPanel(std::shared_ptr<QhyApi> api,QWidget* parent):QWidget(parent),api_(std::move(api))
{
    setObjectName("qhyCameraPanel");auto* layout=new QVBoxLayout(this);
    cameras_=new QComboBox(this);cameras_->setObjectName("qhyCameras");
    refresh_=new QPushButton("Refresh cameras",this);refresh_->setObjectName("refreshQhyCameras");
    layout->addWidget(cameras_);layout->addWidget(refresh_);
    editor_=new CameraSettingsEditor(1,1,this);layout->addWidget(editor_);
    apply_=new QPushButton("Apply settings",this);apply_->setObjectName("applyQhyCamera");layout->addWidget(apply_);
    readback_=new QLabel("Select QHY source and refresh to discover devices.",this);readback_->setWordWrap(true);
    readback_->setObjectName("qhyReadback");layout->addWidget(readback_);
    auto* hint=new QLabel("Applying while running finishes the current clip and starts a fresh session. Bin 1 only. RAW16 preserves every word bit. Hardware qualification is pending; loss counts and exposure timing are unknown.",this);
    hint->setWordWrap(true);layout->addWidget(hint);layout->addStretch();
    connect(refresh_,&QPushButton::clicked,this,&QhyCameraPanel::refresh);
    connect(cameras_,&QComboBox::currentIndexChanged,this,&QhyCameraPanel::inspectSelected);
    connect(apply_,&QPushButton::clicked,this,&QhyCameraPanel::applyRequested);setRunning(false);
}
void QhyCameraPanel::refresh()
{
    if(running_) return;profile_.reset();
    try
    {
        { const QSignalBlocker blocked(cameras_);cameras_->clear();
          for(const auto& d:api_->devices()) cameras_->addItem(QString::fromStdString(d.name),QString::fromStdString(d.id)); }
        if(cameras_->count()) inspectSelected();
        else { readback_->setText("No QHY camera detected. Connect one and refresh.");setRunning(false); }
    }
    catch(const std::exception& ex) { readback_->setText(QString::fromUtf8(ex.what()));setRunning(false); }
}
void QhyCameraPanel::inspectSelected()
{
    if(running_ || cameras_->currentIndex()<0) return;
    try { profile_=QhySource::inspect(api_,cameras_->currentData().toString().toStdString());showProfile(); }
    catch(const std::exception& ex) { profile_.reset();readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
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
void QhyCameraPanel::showActual(const QhyProfile& p) { profile_=p;showProfile();setRunning(running_); }
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
    readback_->setText(QString("Read back: %1 × %2, %3-bit transfer, origin (%4, %5).\nBin: 1 commanded; delivered geometry is checked.\nSensor ADC: %6 bits. SDK %7.\nDevice ID: %8 (separate serial unavailable).")
        .arg(s.width).arg(s.height).arg(s.format==PixelFormat::Mono16?16:8).arg(s.x).arg(s.y).arg(p.camera.sensorBits)
        .arg(QString::fromStdString(p.sdkVersion),QString::fromStdString(p.camera.device.id)));
}
void QhyCameraPanel::setRunning(bool running)
{
    running_=running;refresh_->setEnabled(!running);cameras_->setEnabled(!running);
    editor_->setEnabled(hasCamera());apply_->setEnabled(hasCamera());apply_->setText(running?"Apply and restart":"Apply settings");
}
