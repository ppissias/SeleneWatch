#include "camera_settings_editor.h"
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QSpinBox>
#include <algorithm>
#include <cmath>
CameraSettingsEditor::CameraSettingsEditor(int widthStep,int heightStep,QWidget* parent)
    : QWidget(parent),widthStep_(widthStep),heightStep_(heightStep)
{
    liveTimer_=new QTimer(this); liveTimer_->setSingleShot(true); liveTimer_->setInterval(180);
    connect(liveTimer_,&QTimer::timeout,this,[this] { if (liveEditing_) emit liveControlsRequested(); });
    auto* form=new QFormLayout(this);
    format_=new QComboBox(this);format_->setObjectName("cameraFormat");
    bin_=new QComboBox(this);bin_->setObjectName("cameraBin");
    width_=new QSpinBox(this);height_=new QSpinBox(this);x_=new QSpinBox(this);y_=new QSpinBox(this);
    width_->setObjectName("cameraWidth");height_->setObjectName("cameraHeight");x_->setObjectName("cameraX");y_->setObjectName("cameraY");
    width_->setSingleStep(widthStep);height_->setSingleStep(heightStep);
    form->addRow("Raw format",format_);form->addRow("Binning",bin_);
    form->addRow("ROI width",width_);form->addRow("ROI height",height_);form->addRow("Origin x",x_);form->addRow("Origin y",y_);
    auto* controls=new QWidget(this);controlForm_=new QFormLayout(controls);form->addRow(controls);
    connect(bin_,&QComboBox::currentIndexChanged,this,&CameraSettingsEditor::updateBounds);
    connect(width_,&QSpinBox::valueChanged,this,&CameraSettingsEditor::updateBounds);
    connect(height_,&QSpinBox::valueChanged,this,&CameraSettingsEditor::updateBounds);
}
void CameraSettingsEditor::showGeometry(int sensorWidth,int sensorHeight,const std::vector<PixelFormat>& formats,
    const std::vector<int>& bins,const CameraGeometry& g)
{
    updating_=true;sensorWidth_=sensorWidth;sensorHeight_=sensorHeight;
    format_->clear();bin_->clear();
    for(auto f:formats) format_->addItem(f==PixelFormat::Mono16?"RAW16":"RAW8",static_cast<int>(f));
    format_->setCurrentIndex(std::max(0,format_->findData(static_cast<int>(g.format))));
    for(auto bin:bins) bin_->addItem(QString::number(bin),bin);
    bin_->setCurrentIndex(std::max(0,bin_->findData(g.bin)));
    width_->setRange(widthStep_,sensorWidth);height_->setRange(heightStep_,sensorHeight);
    width_->setValue(g.width);height_->setValue(g.height);
    x_->setRange(0,sensorWidth);y_->setRange(0,sensorHeight);x_->setValue(g.x);y_->setValue(g.y);
    updating_=false;updateBounds();
}
CameraGeometry CameraSettingsEditor::requestedGeometry() const
{
    return {width_->value(),height_->value(),x_->value(),y_->value(),bin_->currentData().toInt(),
        static_cast<PixelFormat>(format_->currentData().toInt())};
}
void CameraSettingsEditor::clearControls()
{
    liveTimer_->stop();
    while(controlForm_->rowCount()) controlForm_->removeRow(0);
    controls_.clear();
}
void CameraSettingsEditor::addControl(int id,const QString& name,double minimum,double maximum,double step,double value,
    double scale,const QString& suffix,bool writable)
{
    auto* input=new QDoubleSpinBox(this);input->setObjectName(QString("cameraControl%1").arg(id));
    int decimals=0;
    for (double number : {1.0/scale,step/scale,minimum/scale,maximum/scale,value/scale})
    {
        int required=0;
        while(required<6 && std::abs(number-std::round(number))>1e-8) { ++required;number*=10; }
        decimals=std::max(decimals,required);
    }
    input->setDecimals(decimals);input->setRange(minimum/scale,maximum/scale);input->setSingleStep(step/scale);
    input->setValue(value/scale);input->setSuffix(suffix);input->setEnabled(writable);
    input->setKeyboardTracking(false);
    connect(input,&QDoubleSpinBox::valueChanged,this,[this] { if (liveEditing_) liveTimer_->start(); });
    controlForm_->addRow(name,input);controls_[id]={input,scale};
}
void CameraSettingsEditor::setLiveEditing(bool enabled)
{
    liveEditing_=enabled;
    if (!enabled) liveTimer_->stop();
}
std::map<int,double> CameraSettingsEditor::controlValues() const
{
    std::map<int,double> result;
    for(const auto& [id,control]:controls_) result[id]=control.widget->value()*control.scale;
    return result;
}
void CameraSettingsEditor::updateBounds()
{
    if(updating_ || sensorWidth_<=0 || sensorHeight_<=0) return;
    updating_=true;const int bin=std::max(1,bin_->currentData().toInt());
    width_->setRange(widthStep_,sensorWidth_/bin);height_->setRange(heightStep_,sensorHeight_/bin);
    x_->setRange(0,std::max(0,sensorWidth_/bin-width_->value()));y_->setRange(0,std::max(0,sensorHeight_/bin-height_->value()));
    updating_=false;
}
