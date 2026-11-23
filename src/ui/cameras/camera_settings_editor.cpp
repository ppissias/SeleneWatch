#include "ui/cameras/camera_settings_editor.h"
#include "ui/widgets/control_help.h"
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
namespace {
// Slider positions when the box's own steps are too many to give each one.
constexpr int kSliderPositions=1000;
// Feature-39: fixed preset stops (ms) for each Exposure range. Small overlaps
// avoid back-and-forth range switching near 15 ms or 60 ms.
constexpr double kPlanetaryPresets[]={0.05,0.1,0.2,0.5,1,1.5,2,3,4,5,6,8,10,12,15,20};
constexpr double kFastPresets[]={5,8,10,12,15,16,20,25,30,33,40,50,60,75,100};
constexpr double kSkyPresets[]={50,75,100,150,200,250,350,500,750,1000,1500,2000,3000,4000,5000};
std::span<const double> exposurePresetTable(ExposureRange range)
{
    switch (range)
    {
    case ExposureRange::Planetary: return kPlanetaryPresets;
    case ExposureRange::Fast: return kFastPresets;
    case ExposureRange::Sky: return kSkyPresets;
    }
    return kFastPresets;
}
double exposureRangeStepMs(ExposureRange range)
{
    switch (range)
    {
    case ExposureRange::Planetary: return 0.1;
    case ExposureRange::Fast: return 1.0;
    case ExposureRange::Sky: return 50.0;
    }
    return 1.0;
}
std::vector<double> filteredExposurePresets(ExposureRange range,double low,double high)
{
    const auto table=exposurePresetTable(range);
    std::vector<double> stops;
    stops.reserve(table.size());
    for (double ms : table)
        if (ms>=low-1e-9 && ms<=high+1e-9) stops.push_back(std::clamp(ms,low,high));
    if (stops.size()<2)
    {
        stops.clear();
        const double first=std::clamp(table.front(),low,high);
        const double last=std::clamp(table.back(),low,high);
        stops.push_back(first);
        if (std::abs(last-first)>1e-9) stops.push_back(last);
    }
    return stops;
}
int nearestPresetIndex(const std::vector<double>& stops,double value)
{
    if (stops.empty()) return 0;
    int best=0;
    double bestDist=std::abs(stops[0]-value);
    for (size_t i=1;i<stops.size();++i)
    {
        const double dist=std::abs(stops[i]-value);
        if (dist<bestDist) { bestDist=dist; best=static_cast<int>(i); }
    }
    return best;
}
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
AlignedRoi alignRoi(int maxWidth,int maxHeight,const RoiConstraints& constraints,
    int reqX,int reqY,int reqW,int reqH,bool preserveCenter)
{
    if (maxWidth<=0 || maxHeight<=0)
        return {0,0,std::max(0,reqW),std::max(0,reqH),false};

    const int wStep=std::max(1,constraints.widthStep);
    const int hStep=std::max(1,constraints.heightStep);
    const int xStep=std::max(1,constraints.xStep);
    const int yStep=std::max(1,constraints.yStep);
    const int pixelMult=std::max(1,constraints.pixelCountMultiple);

    const int maxValidW=(maxWidth/wStep)*wStep;
    const int maxValidH=(maxHeight/hStep)*hStep;
    if (maxValidW<=0 || maxValidH<=0)
        return {0,0,maxWidth,maxHeight,true};

    const int minValidW=std::min(maxValidW,((std::max(1,constraints.minWidth)+wStep-1)/wStep)*wStep);
    const int minValidH=std::min(maxValidH,((std::max(1,constraints.minHeight)+hStep-1)/hStep)*hStep);

    auto roundStep=[](int value,int step,int lo,int hi) {
        const int clamped=std::clamp(value,lo,hi);
        const int snapped=((clamped+step/2)/step)*step;
        return std::clamp(snapped,lo,hi);
    };

    int w=roundStep(reqW,wStep,minValidW,maxValidW);
    int h=roundStep(reqH,hStep,minValidH,maxValidH);

    if (pixelMult>1 && (static_cast<int64_t>(w)*h)%pixelMult!=0)
    {
        auto findNearest=[&](int searchRadius) -> std::optional<std::pair<int,int>> {
            const int rawLoW=std::max(minValidW,w-searchRadius);
            const int loW=(rawLoW/wStep)*wStep;
            const int hiW=std::min(maxValidW,((w+searchRadius+wStep-1)/wStep)*wStep);
            const int rawLoH=std::max(minValidH,h-searchRadius);
            const int loH=(rawLoH/hStep)*hStep;
            const int hiH=std::min(maxValidH,((h+searchRadius+hStep-1)/hStep)*hStep);
            bool found=false;
            int bestW=w,bestH=h;
            int bestDist=std::numeric_limits<int>::max();
            int bestMax=std::numeric_limits<int>::max();
            int64_t bestArea=-1;
            for (int cw=loW;cw<=hiW;cw+=wStep)
            {
                if (cw<minValidW || cw>maxValidW) continue;
                for (int ch=loH;ch<=hiH;ch+=hStep)
                {
                    if (ch<minValidH || ch>maxValidH) continue;
                    if ((static_cast<int64_t>(cw)*ch)%pixelMult!=0) continue;
                    const int dw=std::abs(cw-reqW);
                    const int dh=std::abs(ch-reqH);
                    const int dist=dw+dh;
                    const int maxDiff=std::max(dw,dh);
                    const int64_t area=static_cast<int64_t>(cw)*ch;
                    if (!found || dist<bestDist
                        || (dist==bestDist && maxDiff<bestMax)
                        || (dist==bestDist && maxDiff==bestMax && area>bestArea))
                    {
                        found=true;
                        bestW=cw;
                        bestH=ch;
                        bestDist=dist;
                        bestMax=maxDiff;
                        bestArea=area;
                    }
                }
            }
            if (!found) return std::nullopt;
            return std::pair{bestW,bestH};
        };

        auto best=findNearest(64);
        if (!best) best=findNearest(std::max(maxValidW,maxValidH));
        if (best) { w=best->first; h=best->second; }
    }

    const int maxX=(std::max(0,maxWidth-w)/xStep)*xStep;
    const int maxY=(std::max(0,maxHeight-h)/yStep)*yStep;
    const int targetX=preserveCenter?reqX+(reqW-w)/2:reqX;
    const int targetY=preserveCenter?reqY+(reqH-h)/2:reqY;
    const int x=roundStep(targetX,xStep,0,maxX);
    const int y=roundStep(targetY,yStep,0,maxY);
    const bool adjusted=(x!=reqX || y!=reqY || w!=reqW || h!=reqH);
    return {x,y,w,h,adjusted};
}

CameraSettingsEditor::CameraSettingsEditor(int widthStep,int heightStep,QWidget* parent)
    : CameraSettingsEditor(RoiConstraints{widthStep,heightStep,1,1,widthStep,heightStep,1},parent)
{
}

CameraSettingsEditor::CameraSettingsEditor(RoiConstraints constraints,QWidget* parent)
    : QWidget(parent),constraints_(constraints)
{
    // Coalesce a burst of edits into one live SDK request.
    liveTimer_=new QTimer(this); liveTimer_->setSingleShot(true); liveTimer_->setInterval(180);
    connect(liveTimer_,&QTimer::timeout,this,[this] { if (liveEditing_) emit liveControlsRequested(); });
    auto* form=new QFormLayout(this);
    form->setContentsMargins(0,0,0,0);
    form->setVerticalSpacing(4);
    format_=new QComboBox(this);format_->setObjectName("cameraFormat");
    bin_=new QComboBox(this);bin_->setObjectName("cameraBin");
    auto* roiButtons=new QWidget(this);
    auto* roiRow=new QHBoxLayout(roiButtons);
    roiRow->setContentsMargins(0,0,0,0);
    roiRow->setSpacing(4);
    drawRoi_=new QToolButton(this);
    drawRoi_->setText("Draw ROI");
    drawRoi_->setObjectName("cameraDrawRoi");
    drawRoi_->setCheckable(true);
    drawRoi_->setEnabled(false);
    drawRoi_->setToolTip("Draw a new camera ROI box directly on the preview. While Preview is running, releasing the drag rounds to the camera's alignment rules, applies the ROI, and restarts Preview.");
    centerRoi_=new QToolButton(this);
    centerRoi_->setText("Center");
    centerRoi_->setObjectName("cameraRoiCenter");
    centerRoi_->setToolTip("Center the current ROI dimensions on the sensor. While Preview is running, applies immediately and restarts Preview.");
    fullRoi_=new QToolButton(this);
    fullRoi_->setText("Full");
    fullRoi_->setObjectName("cameraRoiFull");
    fullRoi_->setToolTip("Reset the camera ROI to the full sensor at the current binning. While Preview is running, applies immediately and restarts Preview.");
    roiRow->addWidget(drawRoi_);
    roiRow->addWidget(centerRoi_);
    roiRow->addWidget(fullRoi_);
    roiRow->addStretch(1);
    roiButtons->setToolTip("Draw an ROI box on the preview, center the current ROI size on the sensor, or reset to the full sensor.");
    roiPreset_=new QComboBox(this);
    roiPreset_->setObjectName("cameraRoiPreset");
    roiPreset_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    roiPreset_->setMinimumContentsLength(14);
    roiPreset_->addItem("Centered presets…");
    roiPreset_->setToolTip("Choose a centered ROI size already aligned to this camera's hardware rules. While Preview is running, applies immediately and restarts Preview.");
    width_=new QSpinBox(this);height_=new QSpinBox(this);x_=new QSpinBox(this);y_=new QSpinBox(this);
    width_->setObjectName("cameraWidth");height_->setObjectName("cameraHeight");x_->setObjectName("cameraX");y_->setObjectName("cameraY");
    width_->setSingleStep(std::max(1,constraints_.widthStep));
    height_->setSingleStep(std::max(1,constraints_.heightStep));
    x_->setSingleStep(std::max(1,constraints_.xStep));
    y_->setSingleStep(std::max(1,constraints_.yStep));
    format_->setToolTip("RAW8 or RAW16 pixels from the sensor. RAW16 keeps every bit the camera delivers; RAW8 is faster and smaller.");
    bin_->setToolTip("Combines blocks of pixels into one: brighter and faster, with less resolution.");
    width_->setToolTip("Width of the region of interest, in pixels after binning. A smaller region gives a higher frame rate. Automatically rounded to the camera's alignment rules.");
    height_->setToolTip("Height of the region of interest, in pixels after binning. A smaller region gives a higher frame rate. Automatically rounded to the camera's alignment rules.");
    x_->setToolTip("Left edge of the region of interest on the sensor, in pixels after binning. Automatically rounded to the camera's alignment rules.");
    y_->setToolTip("Top edge of the region of interest on the sensor, in pixels after binning. Automatically rounded to the camera's alignment rules.");
    auto* sizeRowWidget=new QWidget(this);
    auto* sizeRow=new QHBoxLayout(sizeRowWidget);
    sizeRow->setContentsMargins(0,0,0,0);
    sizeRow->setSpacing(4);
    sizeRow->addWidget(width_,1);
    sizeRow->addWidget(new QLabel("×",sizeRowWidget));
    sizeRow->addWidget(height_,1);
    sizeRowWidget->setToolTip("Width × height of the region of interest, in pixels after binning. Automatically rounded to the camera's alignment rules.");
    auto* originRowWidget=new QWidget(this);
    auto* originRow=new QHBoxLayout(originRowWidget);
    originRow->setContentsMargins(0,0,0,0);
    originRow->setSpacing(4);
    originRow->addWidget(x_,1);
    originRow->addWidget(new QLabel(",",originRowWidget));
    originRow->addWidget(y_,1);
    originRowWidget->setToolTip("Top-left origin (x, y) of the region of interest on the sensor, in pixels after binning. Automatically rounded to the camera's alignment rules.");
    form->addRow("Raw format",format_);form->addRow("Binning",bin_);
    form->addRow("ROI",roiButtons);form->addRow("ROI preset",roiPreset_);
    form->addRow("ROI size",sizeRowWidget);form->addRow("ROI origin",originRowWidget);
    auto* controls=new QWidget(this);controlForm_=new QFormLayout(controls);controlForm_->setContentsMargins(0,0,0,0);controlForm_->setVerticalSpacing(4);form->addRow(controls);
    connect(format_,&QComboBox::currentIndexChanged,this,&CameraSettingsEditor::notifyGeometryDirty);
    connect(bin_,&QComboBox::currentIndexChanged,this,[this] {
        updateBounds();
        rebuildRoiPresets();
        alignSpinBoxes(false);
        notifyGeometryDirty();
    });
    connect(width_,&QSpinBox::valueChanged,this,[this] { updateBounds(); notifyGeometryDirty(); });
    connect(height_,&QSpinBox::valueChanged,this,[this] { updateBounds(); notifyGeometryDirty(); });
    connect(x_,&QSpinBox::valueChanged,this,&CameraSettingsEditor::notifyGeometryDirty);
    connect(y_,&QSpinBox::valueChanged,this,&CameraSettingsEditor::notifyGeometryDirty);
    for (QSpinBox* box : {width_,height_,x_,y_})
        connect(box,&QSpinBox::editingFinished,this,[this] { alignSpinBoxes(true); });
    connect(drawRoi_,&QToolButton::toggled,this,&CameraSettingsEditor::roiDrawingChanged);
    connect(fullRoi_,&QToolButton::clicked,this,[this] {
        if (sensorWidth_<=0 || sensorHeight_<=0) return;
        const int bw=binnedWidth(),bh=binnedHeight();
        const AlignedRoi full=alignRoi(bw,bh,constraints_,0,0,bw,bh,false);
        applyAlignedToBoxes(full);
        emit roiApplyRequested(QString("Camera ROI reset to full sensor (%1 × %2).").arg(full.width).arg(full.height));
    });
    connect(centerRoi_,&QToolButton::clicked,this,[this] {
        if (sensorWidth_<=0 || sensorHeight_<=0) return;
        const int bw=binnedWidth(),bh=binnedHeight();
        const int reqW=width_->value(),reqH=height_->value();
        const AlignedRoi centered=alignRoi(bw,bh,constraints_,(bw-reqW)/2,(bh-reqH)/2,reqW,reqH,true);
        applyAlignedToBoxes(centered);
        emit roiApplyRequested(QString("Camera ROI centered at %1 × %2, origin (%3, %4).")
            .arg(centered.width).arg(centered.height).arg(centered.x).arg(centered.y));
    });
    connect(roiPreset_,&QComboBox::activated,this,[this](int index) {
        if (index<=0) return;
        const QVariant data=roiPreset_->itemData(index);
        { const QSignalBlocker blocked(roiPreset_); roiPreset_->setCurrentIndex(0); }
        if (!data.canConvert<QRect>()) return;
        const QRect r=data.toRect();
        const AlignedRoi aligned=alignRoi(binnedWidth(),binnedHeight(),constraints_,r.x(),r.y(),r.width(),r.height(),true);
        applyAlignedToBoxes(aligned);
        emit roiApplyRequested(QString("Camera ROI set to %1 × %2 centered at (%3, %4).")
            .arg(aligned.width).arg(aligned.height).arg(aligned.x).arg(aligned.y));
    });
}
void CameraSettingsEditor::setRoiConstraints(const RoiConstraints& constraints)
{
    constraints_=constraints;
    width_->setSingleStep(std::max(1,constraints_.widthStep));
    height_->setSingleStep(std::max(1,constraints_.heightStep));
    x_->setSingleStep(std::max(1,constraints_.xStep));
    y_->setSingleStep(std::max(1,constraints_.yStep));
    updateBounds();
    rebuildRoiPresets();
}
void CameraSettingsEditor::showGeometry(int sensorWidth,int sensorHeight,const std::vector<PixelFormat>& formats,
    const std::vector<int>& bins,const CameraGeometry& g)
{
    updating_=true;sensorWidth_=sensorWidth;sensorHeight_=sensorHeight;actualGeometry_=g;
    format_->clear();bin_->clear();
    for(auto f:formats) format_->addItem(f==PixelFormat::Mono16?"RAW16":"RAW8",static_cast<int>(f));
    format_->setCurrentIndex(std::max(0,format_->findData(static_cast<int>(g.format))));
    for(auto bin:bins) bin_->addItem(QString::number(bin),bin);
    bin_->setCurrentIndex(std::max(0,bin_->findData(g.bin)));
    const int minW=std::max(1,std::max(constraints_.widthStep,constraints_.minWidth));
    const int minH=std::max(1,std::max(constraints_.heightStep,constraints_.minHeight));
    width_->setRange(std::min(minW,std::max(1,sensorWidth)),std::max(1,sensorWidth));
    height_->setRange(std::min(minH,std::max(1,sensorHeight)),std::max(1,sensorHeight));
    width_->setValue(g.width);height_->setValue(g.height);
    x_->setRange(0,std::max(0,sensorWidth));y_->setRange(0,std::max(0,sensorHeight));x_->setValue(g.x);y_->setValue(g.y);
    updating_=false;updateBounds();rebuildRoiPresets();notifyGeometryDirty();
}
CameraGeometry CameraSettingsEditor::requestedGeometry() const
{
    return {width_->value(),height_->value(),x_->value(),y_->value(),bin_->currentData().toInt(),
        static_cast<PixelFormat>(format_->currentData().toInt())};
}
bool CameraSettingsEditor::geometryDirty() const
{
    return sensorWidth_>0 && sensorHeight_>0 && requestedGeometry()!=actualGeometry_;
}
void CameraSettingsEditor::refreshTheme()
{
    emit geometryDirtyChanged(geometryDirty());
}
void CameraSettingsEditor::notifyGeometryDirty()
{
    if (!updating_) emit geometryDirtyChanged(geometryDirty());
}
bool CameraSettingsEditor::drawingRoi() const { return drawRoi_->isChecked(); }
void CameraSettingsEditor::setDrawingRoi(bool drawing)
{
    const QSignalBlocker blocked(drawRoi_);
    drawRoi_->setChecked(drawing);
}
AlignedRoi CameraSettingsEditor::setDrawnRoi(const QRect& previewRegion)
{
    const int bw=binnedWidth(),bh=binnedHeight();
    if (bw<=0 || bh<=0) return {};
    const int actualBin=std::max(1,actualGeometry_.bin);
    const int reqBin=std::max(1,bin_->currentData().toInt());
    const int maxPreviewW=actualGeometry_.width>0?actualGeometry_.width:bw;
    const int maxPreviewH=actualGeometry_.height>0?actualGeometry_.height:bh;
    const QRect clamped=previewRegion.normalized().intersected(QRect(0,0,maxPreviewW,maxPreviewH));
    // Translate preview-frame pixels into sensor coordinates at the requested bin,
    // so dragging inside an already cropped ROI zooms deeper into the right spot.
    const int curX=actualGeometry_.x+clamped.x();
    const int curY=actualGeometry_.y+clamped.y();
    const int curW=std::max(1,clamped.width());
    const int curH=std::max(1,clamped.height());
    const int sensorX=reqBin==actualBin?curX:(curX*actualBin)/reqBin;
    const int sensorY=reqBin==actualBin?curY:(curY*actualBin)/reqBin;
    const int sensorW=reqBin==actualBin?curW:std::max(1,(curW*actualBin)/reqBin);
    const int sensorH=reqBin==actualBin?curH:std::max(1,(curH*actualBin)/reqBin);
    const AlignedRoi aligned=alignRoi(bw,bh,constraints_,sensorX,sensorY,sensorW,sensorH,true);
    applyAlignedToBoxes(aligned);
    return aligned;
}
AlignedRoi CameraSettingsEditor::alignRequestedRoi()
{
    const int bw=binnedWidth(),bh=binnedHeight();
    if (bw<=0 || bh<=0) return {};
    const AlignedRoi aligned=alignRoi(bw,bh,constraints_,x_->value(),y_->value(),width_->value(),height_->value(),false);
    if (aligned.adjusted) applyAlignedToBoxes(aligned);
    return aligned;
}
QString CameraSettingsEditor::alignmentHint() const
{
    if (constraints_.pixelCountMultiple>1)
        return QString("camera %1×%2 / %3-pixel alignment").arg(constraints_.widthStep).arg(constraints_.heightStep).arg(constraints_.pixelCountMultiple);
    if (constraints_.xStep>1 || constraints_.yStep>1)
        return QString("camera %1×%2 size & %3×%4 origin alignment")
            .arg(constraints_.widthStep).arg(constraints_.heightStep).arg(constraints_.xStep).arg(constraints_.yStep);
    if (constraints_.widthStep>1 || constraints_.heightStep>1)
        return QString("camera %1×%2 alignment").arg(constraints_.widthStep).arg(constraints_.heightStep);
    return "camera sensor bounds";
}
// A queued edit belongs to the controls being replaced.
void CameraSettingsEditor::clearControls()
{
    liveTimer_->stop();
    while(controlForm_->rowCount()) controlForm_->removeRow(0);
    controls_.clear();
    exposureRangeBox_=nullptr;
    exposureId_.reset();
    exposureNativeStep_=0;
    exposurePresets_.clear();
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
    // Feature-25/39: a slider for a control with at least ten steps; Exposure
    // uses fixed presets in the chosen ExposureRange while the box keeps the
    // full hardware range for fine-tuning and typed values.
    const double low=input->minimum(),high=input->maximum(),stepSize=input->singleStep();
    const bool isExposure=writable && !exposureId_
        && suffix.trimmed().compare("ms",Qt::CaseInsensitive)==0
        && name.contains("exposure",Qt::CaseInsensitive);
    if (writable && stepSize>0 && (high-low)/stepSize>=10)
    {
        if (isExposure)
        {
            exposureRangeBox_=new QComboBox(this);
            exposureRangeBox_->setObjectName("cameraExposureRange");
            exposureRangeBox_->addItem("Planetary (≤ 20 ms)",static_cast<int>(ExposureRange::Planetary));
            exposureRangeBox_->addItem("Fast (5–100 ms)",static_cast<int>(ExposureRange::Fast));
            exposureRangeBox_->addItem("Sky (50 ms–5 s)",static_cast<int>(ExposureRange::Sky));
            exposureRangeBox_->setToolTip("Preset range for the Exposure slider and its up/down arrow step. "
                "The box still accepts any exposure the camera supports.");
            controlForm_->addRow("Exposure range",exposureRangeBox_);
            connect(exposureRangeBox_,&QComboBox::currentIndexChanged,this,[this](int index) {
                if (index<0) return;
                setExposureRange(static_cast<ExposureRange>(exposureRangeBox_->itemData(index).toInt()));
            });
            auto* row=new QWidget(this);
            auto* line=new QHBoxLayout(row);line->setContentsMargins(0,0,0,0);
            auto* slider=new QSlider(Qt::Horizontal,this);
            slider->setObjectName(QString("cameraControlSlider%1").arg(id));
            slider->setTickPosition(QSlider::TicksBelow);
            slider->setTickInterval(1);
            slider->setPageStep(1);
            slider->setToolTip(input->toolTip()+" The slider steps through presets in the chosen range; "
                "use the arrows or box to fine-tune.");
            connect(slider,&QSlider::valueChanged,input,[this,input](int position) {
                if (position>=0 && static_cast<size_t>(position)<exposurePresets_.size())
                    input->setValue(exposurePresets_[static_cast<size_t>(position)]);
            });
            connect(input,&QDoubleSpinBox::valueChanged,slider,[this,slider](double current) {
                const QSignalBlocker blocked(slider);
                slider->setValue(nearestPresetIndex(exposurePresets_,current));
            });
            line->addWidget(slider,1);line->addWidget(input);
            controlForm_->addRow(name,row);controls_[id]={input,scale,slider};
            exposureId_=id;
            exposureNativeStep_=stepSize;
            updateExposureRange();
        }
        else
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
    }
    else
    {
        controlForm_->addRow(name,input);controls_[id]={input,scale};
    }
    installControlHelp(this);
}
void CameraSettingsEditor::setExposureRange(ExposureRange range)
{
    exposureRange_=range;
    updateExposureRange();
}
// Updates the exposure slider's preset stops and arrow step without mutating
// the box's current value (so switching ranges never sends an unintended live edit).
void CameraSettingsEditor::updateExposureRange()
{
    if (exposureRangeBox_)
    {
        const QSignalBlocker blocked(exposureRangeBox_);
        const int idx=exposureRangeBox_->findData(static_cast<int>(exposureRange_));
        if (idx>=0) exposureRangeBox_->setCurrentIndex(idx);
    }
    if (!exposureId_) return;
    const auto found=controls_.find(*exposureId_);
    if (found==controls_.end() || !found->second.slider) return;
    QDoubleSpinBox* input=found->second.widget;
    QSlider* slider=found->second.slider;
    exposurePresets_=filteredExposurePresets(exposureRange_,input->minimum(),input->maximum());
    input->setSingleStep(std::max(exposureNativeStep_,exposureRangeStepMs(exposureRange_)));
    const QSignalBlocker blocked(slider);
    slider->setRange(0,std::max(0,static_cast<int>(exposurePresets_.size())-1));
    slider->setValue(nearestPresetIndex(exposurePresets_,input->value()));
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
    if (drawRoi_)
    {
        if (!enabled && drawRoi_->isChecked()) drawRoi_->setChecked(false);
        drawRoi_->setEnabled(enabled);
    }
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
int CameraSettingsEditor::binnedWidth() const
{
    const int bin=std::max(1,bin_->currentData().toInt());
    return sensorWidth_>0?sensorWidth_/bin:0;
}
int CameraSettingsEditor::binnedHeight() const
{
    const int bin=std::max(1,bin_->currentData().toInt());
    return sensorHeight_>0?sensorHeight_/bin:0;
}
void CameraSettingsEditor::applyAlignedToBoxes(const AlignedRoi& aligned)
{
    const int bw=binnedWidth(),bh=binnedHeight();
    if (bw<=0 || bh<=0) return;
    updating_=true;
    const int wStep=std::max(1,constraints_.widthStep);
    const int hStep=std::max(1,constraints_.heightStep);
    const int xStep=std::max(1,constraints_.xStep);
    const int yStep=std::max(1,constraints_.yStep);
    const int maxW=(bw/wStep)*wStep;
    const int maxH=(bh/hStep)*hStep;
    const int minW=std::min(maxW,((std::max(1,constraints_.minWidth)+wStep-1)/wStep)*wStep);
    const int minH=std::min(maxH,((std::max(1,constraints_.minHeight)+hStep-1)/hStep)*hStep);
    if (maxW>0 && maxH>0)
    {
        width_->setRange(minW,maxW);
        height_->setRange(minH,maxH);
        width_->setValue(aligned.width);
        height_->setValue(aligned.height);
        x_->setRange(0,(std::max(0,bw-width_->value())/xStep)*xStep);
        y_->setRange(0,(std::max(0,bh-height_->value())/yStep)*yStep);
        x_->setValue(aligned.x);
        y_->setValue(aligned.y);
    }
    updating_=false;
}
void CameraSettingsEditor::alignSpinBoxes(bool notify)
{
    if (updating_ || sensorWidth_<=0 || sensorHeight_<=0) return;
    const AlignedRoi aligned=alignRequestedRoi();
    if (notify && aligned.adjusted)
    {
        emit roiAdjusted(QString("ROI rounded to %1 × %2 at (%3, %4) (%5).")
            .arg(aligned.width).arg(aligned.height).arg(aligned.x).arg(aligned.y)
            .arg(alignmentHint()));
    }
}
void CameraSettingsEditor::rebuildRoiPresets()
{
    if (!roiPreset_) return;
    const QSignalBlocker blocked(roiPreset_);
    roiPreset_->clear();
    roiPreset_->addItem("Centered presets…");
    const int bw=binnedWidth(),bh=binnedHeight();
    if (bw<=0 || bh<=0) return;
    const AlignedRoi full=alignRoi(bw,bh,constraints_,0,0,bw,bh,false);
    std::vector<std::pair<int,int>> seen;
    auto addPreset=[&](const QString& label,int reqW,int reqH) {
        const AlignedRoi a=alignRoi(bw,bh,constraints_,(bw-reqW)/2,(bh-reqH)/2,reqW,reqH,true);
        if (a.width<=0 || a.height<=0) return;
        if (a.width==full.width && a.height==full.height) return;
        for (const auto& [sw,sh] : seen) if (sw==a.width && sh==a.height) return;
        seen.emplace_back(a.width,a.height);
        roiPreset_->addItem(label.arg(a.width).arg(a.height),QVariant::fromValue(QRect(a.x,a.y,a.width,a.height)));
    };
    for (int pct : {75,50,25})
        addPreset(QString("%1% (%2 × %3)").arg(pct).arg("%1").arg("%2"),(bw*pct)/100,(bh*pct)/100);
    constexpr std::pair<int,int> kStandardSizes[]={
        {1280,1024},{1024,1024},{1024,768},{800,600},{640,480},{512,512},{320,240},{256,256}
    };
    for (const auto& [pw,ph] : kStandardSizes)
    {
        if (pw<=bw && ph<=bh && pw>=constraints_.minWidth && ph>=constraints_.minHeight)
            addPreset("%1 × %2",pw,ph);
    }
}
// Constrain the ROI origin and extent to the sensor dimensions at the selected bin.
void CameraSettingsEditor::updateBounds()
{
    if(updating_ || sensorWidth_<=0 || sensorHeight_<=0) return;
    updating_=true;
    const int bw=binnedWidth(),bh=binnedHeight();
    const int wStep=std::max(1,constraints_.widthStep);
    const int hStep=std::max(1,constraints_.heightStep);
    const int xStep=std::max(1,constraints_.xStep);
    const int yStep=std::max(1,constraints_.yStep);
    const int maxW=std::max(1,(bw/wStep)*wStep);
    const int maxH=std::max(1,(bh/hStep)*hStep);
    const int minW=std::min(maxW,((std::max(1,constraints_.minWidth)+wStep-1)/wStep)*wStep);
    const int minH=std::min(maxH,((std::max(1,constraints_.minHeight)+hStep-1)/hStep)*hStep);
    width_->setRange(minW,maxW);height_->setRange(minH,maxH);
    x_->setRange(0,(std::max(0,bw-width_->value())/xStep)*xStep);
    y_->setRange(0,(std::max(0,bh-height_->value())/yStep)*yStep);
    updating_=false;
}
