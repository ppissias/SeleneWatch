#include "camera_settings_editor.h"
#include "control_help.h"
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <algorithm>
#include <cmath>
namespace {
// Slider positions when the box's own steps are too many to give each one.
constexpr int kSliderPositions=1000;
// A logarithmic slider starts at the smallest positive step, so its first
// position is the control's minimum and the rest spread evenly in ratio.
double sliderValue(const QDoubleSpinBox& input,bool logarithmic,int position,int positions)
{
    const double low=input.minimum(),high=input.maximum();
    if (position<=0 || positions<=0) return low;
    if (position>=positions) return high;
    const double fraction=static_cast<double>(position)/positions;
    if (!logarithmic) return low+fraction*(high-low);
    const double start=std::max(low,input.singleStep());
    return start*std::pow(high/start,fraction);
}
int sliderPosition(const QDoubleSpinBox& input,bool logarithmic,double value,int positions)
{
    const double low=input.minimum(),high=input.maximum();
    if (high<=low || value<=low) return 0;
    if (value>=high) return positions;
    double fraction=(value-low)/(high-low);
    if (logarithmic)
    {
        const double start=std::max(low,input.singleStep());
        fraction=value<=start?0:std::log(value/start)/std::log(high/start);
    }
    return std::clamp(static_cast<int>(std::lround(fraction*positions)),0,positions);
}
// Feature-24: what a camera control does, by the name its SDK gives it.
// The camera families name the same setting differently.
QString cameraControlHelp(const QString& name)
{
    const QString key=name.toLower();
    if (key.contains("exposure"))
        return "Exposure time of each frame. Longer shows fainter detail and a higher background, "
               "but limits the frame rate and smears fast events across fewer frames.";
    if (key.contains("gain"))
        return "Sensor amplification. Higher gain shows fainter flashes at a given exposure, with more noise "
               "and less headroom before bright features saturate.";
    if (key.contains("offset") || key.contains("black") || key=="brightness")
        return "Black level added to every pixel, so the darkest pixels are not clipped at zero. "
               "Raise it until the histogram's left edge is clear of zero.";
    if (key.contains("bandwidth") || key.contains("traffic") || key.contains("transfer") || key.contains("speed"))
        return "USB transfer rate. Lower it if frames are dropped or the camera stalls; "
               "higher gives the highest frame rate the cable and computer can sustain.";
    if (key.contains("gamma"))
        return "Camera-side gamma. Leave at the neutral value for measurements: it changes pixel values non-linearly.";
    if (key.contains("wb"))
        return "Colour balance of a colour camera's red or blue channel, applied by the camera.";
    if (key.contains("temp"))
        return "Sensor temperature setting or reading.";
    return QString("%1: a setting reported by the camera's SDK. See the camera maker's documentation.").arg(name);
}
}
CameraSettingsEditor::CameraSettingsEditor(int widthStep,int heightStep,QWidget* parent)
    : QWidget(parent),widthStep_(widthStep),heightStep_(heightStep)
{
    // Coalesce a burst of edits into one live SDK request.
    liveTimer_=new QTimer(this); liveTimer_->setSingleShot(true); liveTimer_->setInterval(180);
    connect(liveTimer_,&QTimer::timeout,this,[this] { if (liveEditing_) emit liveControlsRequested(); });
    auto* form=new QFormLayout(this);
    format_=new QComboBox(this);format_->setObjectName("cameraFormat");
    bin_=new QComboBox(this);bin_->setObjectName("cameraBin");
    width_=new QSpinBox(this);height_=new QSpinBox(this);x_=new QSpinBox(this);y_=new QSpinBox(this);
    width_->setObjectName("cameraWidth");height_->setObjectName("cameraHeight");x_->setObjectName("cameraX");y_->setObjectName("cameraY");
    width_->setSingleStep(widthStep);height_->setSingleStep(heightStep);
    format_->setToolTip("RAW8 or RAW16 pixels from the sensor. RAW16 keeps every bit the camera delivers; RAW8 is faster and smaller.");
    bin_->setToolTip("Combines blocks of pixels into one: brighter and faster, with less resolution.");
    width_->setToolTip("Width of the region of interest, in pixels after binning. A smaller region gives a higher frame rate.");
    height_->setToolTip("Height of the region of interest, in pixels after binning. A smaller region gives a higher frame rate.");
    x_->setToolTip("Left edge of the region of interest on the sensor, in pixels after binning.");
    y_->setToolTip("Top edge of the region of interest on the sensor, in pixels after binning.");
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
// A queued edit belongs to the controls being replaced.
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
    // Preserve the precision of the native step and actual readback after scaling.
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
    input->setToolTip(cameraControlHelp(name));
    connect(input,&QDoubleSpinBox::valueChanged,this,[this] { if (liveEditing_) liveTimer_->start(); });
    // Feature-25: a slider for a control with at least ten steps; on/off and
    // few-valued controls keep only the box.
    const double low=input->minimum(),high=input->maximum(),stepSize=input->singleStep();
    if (writable && stepSize>0 && (high-low)/stepSize>=10)
    {
        const bool logarithmic=low>=0 && high/std::max(low,stepSize)>1000;
        auto* row=new QWidget(this);
        auto* line=new QHBoxLayout(row);line->setContentsMargins(0,0,0,0);
        QSlider* slider=addSlider(input,logarithmic);
        slider->setObjectName(QString("cameraControlSlider%1").arg(id));
        slider->setToolTip(input->toolTip()+(logarithmic?" The slider is logarithmic; the box takes an exact value."
            :" The box takes an exact value."));
        line->addWidget(slider,1);line->addWidget(input);
        controlForm_->addRow(name,row);controls_[id]={input,scale,slider};
    }
    else
    {
        controlForm_->addRow(name,input);controls_[id]={input,scale};
    }
    installControlHelp(this);
}
// The slider sets the box; the box moves the slider. A change reaches the
// camera through the box, after the same pause as a typed value.
QSlider* CameraSettingsEditor::addSlider(QDoubleSpinBox* input,bool logarithmic)
{
    auto* slider=new QSlider(Qt::Horizontal,this);
    const double steps=(input->maximum()-input->minimum())/input->singleStep();
    slider->setRange(0,logarithmic || steps>kSliderPositions?kSliderPositions:static_cast<int>(std::lround(steps)));
    slider->setValue(sliderPosition(*input,logarithmic,input->value(),slider->maximum()));
    connect(slider,&QSlider::valueChanged,input,[input,slider,logarithmic](int position) {
        input->setValue(sliderValue(*input,logarithmic,position,slider->maximum()));
    });
    connect(input,&QDoubleSpinBox::valueChanged,slider,[input,slider,logarithmic](double value) {
        const QSignalBlocker blocked(slider);
        slider->setValue(sliderPosition(*input,logarithmic,value,slider->maximum()));
    });
    return slider;
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
std::optional<double> CameraSettingsEditor::controlValue(int id) const
{
    const auto found=controls_.find(id);
    if(found==controls_.end()) return std::nullopt;
    return found->second.widget->value()*found->second.scale;
}
std::optional<std::pair<double,double>> CameraSettingsEditor::controlRange(int id) const
{
    const auto found=controls_.find(id);
    if(found==controls_.end()) return std::nullopt;
    return std::pair{found->second.widget->minimum()*found->second.scale,found->second.widget->maximum()*found->second.scale};
}
bool CameraSettingsEditor::setControlValue(int id,double nativeValue)
{
    const auto found=controls_.find(id);
    if(found==controls_.end()) return false;
    found->second.widget->setValue(nativeValue/found->second.scale);
    return true;
}
// Constrain the ROI origin and extent to the sensor dimensions at the selected bin.
void CameraSettingsEditor::updateBounds()
{
    if(updating_ || sensorWidth_<=0 || sensorHeight_<=0) return;
    updating_=true;const int bin=std::max(1,bin_->currentData().toInt());
    width_->setRange(widthStep_,sensorWidth_/bin);height_->setRange(heightStep_,sensorHeight_/bin);
    x_->setRange(0,std::max(0,sensorWidth_/bin-width_->value()));y_->setRange(0,std::max(0,sensorHeight_/bin-height_->value()));
    updating_=false;
}
