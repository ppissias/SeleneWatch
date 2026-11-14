#include "camera_panel.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
#include <cmath>
#include <algorithm>

CameraPanel::CameraPanel(std::shared_ptr<ZwoApi> api,QWidget* parent):QWidget(parent),api_(std::move(api))
{
    setObjectName("cameraPanel");
    auto* layout=new QVBoxLayout(this);
    cameras_=new QComboBox(this); cameras_->setObjectName("asiCameras");
    refresh_=new QPushButton("Refresh cameras",this); refresh_->setObjectName("refreshCameras");
    layout->addWidget(cameras_); layout->addWidget(refresh_);
    controls_=new QWidget(this); auto* form=new QFormLayout(controls_);
    format_=new QComboBox(this); format_->setObjectName("cameraFormat");
    bin_=new QComboBox(this); bin_->setObjectName("cameraBin");
    width_=new QSpinBox(this); height_=new QSpinBox(this); x_=new QSpinBox(this); y_=new QSpinBox(this);
    width_->setObjectName("cameraWidth"); height_->setObjectName("cameraHeight"); x_->setObjectName("cameraX"); y_->setObjectName("cameraY");
    width_->setSingleStep(8); height_->setSingleStep(2);
    form->addRow("Raw format",format_); form->addRow("Binning",bin_);
    form->addRow("ROI width",width_); form->addRow("ROI height",height_); form->addRow("Origin x",x_); form->addRow("Origin y",y_);
    auto* controlBox=new QWidget(this); controlForm_=new QFormLayout(controlBox); form->addRow(controlBox);
    layout->addWidget(controls_);
    apply_=new QPushButton("Apply settings",this); apply_->setObjectName("applyCamera"); layout->addWidget(apply_);
    readback_=new QLabel("Select the camera source and refresh to discover ASI devices.",this);
    readback_->setWordWrap(true); readback_->setObjectName("cameraReadback"); layout->addWidget(readback_);
    auto* hint=new QLabel("Settings are read back on Apply or Start. Applying while running restarts acquisition and completes the current clip. RAW16 keeps every delivered word; ADC packing is recorded separately.",this);
    hint->setWordWrap(true); layout->addWidget(hint); layout->addStretch();
    connect(refresh_,&QPushButton::clicked,this,&CameraPanel::refresh);
    connect(cameras_,&QComboBox::currentIndexChanged,this,&CameraPanel::inspectSelected);
    connect(apply_,&QPushButton::clicked,this,&CameraPanel::applyRequested);
    connect(bin_,&QComboBox::currentIndexChanged,this,&CameraPanel::updateBounds);
    connect(width_,&QSpinBox::valueChanged,this,&CameraPanel::updateBounds);
    connect(height_,&QSpinBox::valueChanged,this,&CameraPanel::updateBounds);
    setRunning(false);
}
void CameraPanel::refresh()
{
    if (running_) return;
    profile_.reset();
    try
    {
        { const QSignalBlocker blocked(cameras_); cameras_->clear();
          for (const auto& c:api_->cameras()) cameras_->addItem(QString::fromStdString(c.name)+QString(" (ID %1)").arg(c.id),c.id); }
        if (cameras_->count()) inspectSelected();
        else { readback_->setText("No ASI camera detected. Connect one and refresh."); setRunning(false); }
    }
    catch (const std::exception& ex) { readback_->setText(QString::fromUtf8(ex.what())); setRunning(false); }
}
void CameraPanel::inspectSelected()
{
    if (running_ || cameras_->currentIndex()<0) return;
    try { profile_=ZwoSource::inspect(api_,cameras_->currentData().toInt()); showProfile(); }
    catch (const std::exception& ex) { profile_.reset(); readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
}
bool CameraPanel::hasCamera() const { return profile_.has_value(); }
ZwoProfile CameraPanel::profile() const { return profile_.value(); }
ZwoSettings CameraPanel::settings() const
{
    auto s=profile_.value().current;
    s.width=width_->value(); s.height=height_->value(); s.x=x_->value(); s.y=y_->value();
    s.bin=bin_->currentData().toInt(); s.format=static_cast<PixelFormat>(format_->currentData().toInt());
    for (const auto& [kind,widget]:values_) s.controls[kind]=static_cast<int64_t>(std::llround(widget->value()*(kind==ZwoControl::Exposure?1000:1)));
    return s;
}
void CameraPanel::showActual(const ZwoProfile& p) { profile_=p; showProfile(); setRunning(running_); }
void CameraPanel::showProfile()
{
    updating_=true;
    const auto& p=*profile_; const auto& s=p.current;
    format_->clear(); bin_->clear();
    for (auto f:p.camera.formats) format_->addItem(f==PixelFormat::Mono16?"RAW16":"RAW8",static_cast<int>(f));
    format_->setCurrentIndex(std::max(0,format_->findData(static_cast<int>(s.format))));
    for (auto b:p.camera.bins) if (p.camera.bayer==BayerPattern::None || b==1) bin_->addItem(QString::number(b),b);
    bin_->setCurrentIndex(std::max(0,bin_->findData(s.bin)));
    width_->setRange(8,p.camera.width); height_->setRange(2,p.camera.height);
    width_->setValue(s.width); height_->setValue(s.height);
    x_->setRange(0,p.camera.width); y_->setRange(0,p.camera.height); x_->setValue(s.x); y_->setValue(s.y);
    while (controlForm_->rowCount()) controlForm_->removeRow(0);
    values_.clear();
    for (const auto& c:p.controls)
    {
        if (c.kind==ZwoControl::Flip) continue;
        auto* widget=new QDoubleSpinBox(this);
        const bool exposure=c.kind==ZwoControl::Exposure;
        const double divisor=exposure?1000:1;
        widget->setObjectName(QString("cameraControl%1").arg(static_cast<int>(c.kind)));
        widget->setDecimals(exposure?3:0); widget->setRange(c.minimum/divisor,c.maximum/divisor);
        widget->setValue(c.value/divisor); widget->setEnabled(c.writable);
        widget->setSuffix(exposure?" ms":""); widget->setSingleStep(exposure?.1:1);
        controlForm_->addRow(QString::fromStdString(c.name),widget); values_[c.kind]=widget;
    }
    readback_->setText(QString("Read back: %1 × %2, %3, bin %4, origin (%5, %6).\nSensor ADC: %7 bits. SDK %8. Serial: %9.\n%10")
        .arg(s.width).arg(s.height).arg(s.format==PixelFormat::Mono16?"RAW16":s.format==PixelFormat::Mono8?"RAW8":"processed SDK mode; choose RAW").arg(s.bin).arg(s.x).arg(s.y)
        .arg(p.camera.sensorBits).arg(QString::fromStdString(p.sdkVersion),p.serial.empty()?"unknown":QString::fromStdString(p.serial),
            !p.camera.usb3 && p.camera.name.find("ASI120")!=std::string::npos?"USB2 ASI120: ROI pixel count must be a multiple of 1024.":"ROI dimensions are output pixels after binning."));
    updating_=false; updateBounds();
}
void CameraPanel::updateBounds()
{
    if (updating_ || !profile_) return;
    updating_=true;
    const int bin=std::max(1,bin_->currentData().toInt());
    width_->setRange(8,profile_->camera.width/bin); height_->setRange(2,profile_->camera.height/bin);
    x_->setRange(0,std::max(0,profile_->camera.width/bin-width_->value())); y_->setRange(0,std::max(0,profile_->camera.height/bin-height_->value()));
    updating_=false;
}
void CameraPanel::setRunning(bool running)
{
    running_=running; refresh_->setEnabled(!running); cameras_->setEnabled(!running);
    controls_->setEnabled(hasCamera()); apply_->setEnabled(hasCamera());
    apply_->setText(running?"Apply and restart":"Apply settings");
}
