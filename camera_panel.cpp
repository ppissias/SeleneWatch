#include "camera_panel.h"
#include "camera_settings_editor.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <cmath>

CameraPanel::CameraPanel(std::shared_ptr<ZwoApi> api,QWidget* parent):CameraFamilyPanel(parent),api_(std::move(api))
{
    setObjectName("cameraPanel");
    auto* layout=new QVBoxLayout(this);
    cameras_=new QComboBox(this); cameras_->setObjectName("asiCameras");
    refresh_=new QPushButton("Refresh cameras",this); refresh_->setObjectName("refreshCameras");
    layout->addWidget(cameras_); layout->addWidget(refresh_);
    editor_=new CameraSettingsEditor(8,2,this);layout->addWidget(editor_);
    apply_=new QPushButton("Apply settings",this); apply_->setObjectName("applyCamera"); layout->addWidget(apply_);
    readback_=new QLabel("Select the camera source and refresh to discover ASI devices.",this);
    readback_->setWordWrap(true); readback_->setObjectName("cameraReadback"); layout->addWidget(readback_);
    auto* hint=new QLabel("In Preview, native controls apply live after an edit. Actual readbacks appear below. Format, binning and ROI use Apply and restart Preview. Stop monitoring to tune camera settings again. RAW16 keeps every delivered word.",this);
    hint->setWordWrap(true); layout->addWidget(hint); layout->addStretch();
    connect(refresh_,&QPushButton::clicked,this,&CameraPanel::refresh);
    connect(cameras_,&QComboBox::currentIndexChanged,this,&CameraPanel::inspectSelected);
    connect(apply_,&QPushButton::clicked,this,&CameraPanel::applyRequested);
    connect(editor_,&CameraSettingsEditor::liveControlsRequested,this,&CameraPanel::liveControlsRequested);
    setRunning(false);
}
// Discovery and inspection run while stopped. The source worker owns SDK access during capture.
void CameraPanel::refresh()
{
    if (running_) return;
    profile_.reset();
    try
    {
        { const QSignalBlocker blocked(cameras_); cameras_->clear();
          for (const auto& c:api_->cameras()) cameras_->addItem(QString::fromStdString(c.name)+QString(" (ID %1)").arg(c.id),c.id); }
        if (cameras_->count()) { inspectSelected(); return; }
        readback_->setText("No ASI camera detected. Connect one and refresh.");
    }
    catch (const std::exception& ex) { readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
    emit availabilityChanged();
}
void CameraPanel::inspectSelected()
{
    if (running_ || cameras_->currentIndex()<0) return;
    try { profile_=ZwoSource::inspect(api_,cameras_->currentData().toInt()); showProfile(); }
    catch (const std::exception& ex) { profile_.reset(); readback_->setText(QString::fromUtf8(ex.what())); }
    setRunning(false);
    emit availabilityChanged();
}
bool CameraPanel::hasCamera() const { return profile_.has_value(); }
void CameraPanel::validateSettings() const { ZwoSource::validateSettings(profile().camera,settings()); }
std::unique_ptr<FrameSource> CameraPanel::makeSource()
{
    auto source=std::make_unique<ZwoSource>(api_,profile(),settings());
    source_=source.get();
    return source;
}
void CameraPanel::showSourceReadback(bool live)
{
    if (!source_) return;
    if (live) showLiveActual(source_->actualProfile());
    else showActual(source_->actualProfile());
}
bool CameraPanel::readBack(std::string& error)
{
    ZwoSource source(api_,profile(),settings());
    if (!source.open(error)) return false;
    showActual(source.actualProfile());
    return true;
}
void CameraPanel::requestLiveControls()
{
    if (source_) source_->requestLiveControls(settings().controls);
}
ZwoProfile CameraPanel::profile() const { return profile_.value(); }
ZwoSettings CameraPanel::settings() const
{
    auto s=profile_.value().current;
    const auto g=editor_->requestedGeometry();
    s.width=g.width;s.height=g.height;s.x=g.x;s.y=g.y;s.bin=g.bin;s.format=g.format;
    for (const auto& [id,value]:editor_->controlValues()) s.controls[static_cast<ZwoControl>(id)]=static_cast<int64_t>(std::llround(value));
    return s;
}
void CameraPanel::showActual(const ZwoProfile& p) { profile_=p; showProfile(); setRunning(running_,monitoring_); }
// Update the measured values while leaving the observer's edits in the form.
void CameraPanel::showLiveActual(const ZwoProfile& p) { profile_=p; updateReadback(); }
void CameraPanel::showProfile()
{
    const auto& p=*profile_; const auto& s=p.current;
    std::vector<int> bins;
    for (auto bin:p.camera.bins) if(p.camera.bayer==BayerPattern::None || bin==1) bins.push_back(bin);
    editor_->showGeometry(p.camera.width,p.camera.height,p.camera.formats,bins,{s.width,s.height,s.x,s.y,s.bin,s.format});
    editor_->clearControls();
    for (const auto& c:p.controls)
    {
        if (c.kind==ZwoControl::Flip) continue;
        const bool exposure=c.kind==ZwoControl::Exposure;
        editor_->addControl(static_cast<int>(c.kind),QString::fromStdString(c.name),static_cast<double>(c.minimum),static_cast<double>(c.maximum),exposure?100:1,
            static_cast<double>(c.value),exposure?1000:1,exposure?" ms":"",c.writable);
    }
    updateReadback();
}
void CameraPanel::updateReadback()
{
    const auto& p=*profile_; const auto& s=p.current;
    readback_->setText(QString("Read back: %1 × %2, %3, bin %4, origin (%5, %6).\nSensor ADC: %7 bits. SDK %8. Serial: %9.\n%10")
        .arg(s.width).arg(s.height).arg(s.format==PixelFormat::Mono16?"RAW16":s.format==PixelFormat::Mono8?"RAW8":"processed SDK mode; choose RAW").arg(s.bin).arg(s.x).arg(s.y)
        .arg(p.camera.sensorBits).arg(QString::fromStdString(p.sdkVersion),p.serial.empty()?"unknown":QString::fromStdString(p.serial),
            !p.camera.usb3 && p.camera.name.find("ASI120")!=std::string::npos?"USB2 ASI120: ROI pixel count must be a multiple of 1024.":"ROI dimensions are output pixels after binning."));
    QStringList values;
    for (const auto& c:p.controls)
        if (c.kind!=ZwoControl::Flip)
            values << QString("%1: %2%3").arg(QString::fromStdString(c.name))
                .arg(c.kind==ZwoControl::Exposure?c.value/1000.0:static_cast<double>(c.value),0,'g',10)
                .arg(c.kind==ZwoControl::Exposure?" ms":"");
    readback_->setText(readback_->text()+"\nActual controls: "+values.join(", "));
}
void CameraPanel::setRunning(bool running,bool monitoring)
{
    running_=running; monitoring_=monitoring;
    refresh_->setEnabled(!running); cameras_->setEnabled(!running);
    editor_->setEnabled(hasCamera() && !monitoring); apply_->setEnabled(hasCamera() && !monitoring);
    editor_->setLiveEditing(running && !monitoring);
    apply_->setText(running?"Apply format / ROI and restart Preview":"Apply settings");
}
