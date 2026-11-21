#pragma once
#include "frame.h"
#include <QtWidgets/QWidget>
#include <map>
#include <optional>
#include <utility>
#include <vector>
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QFormLayout;
class QSlider;
class QTimer;
// Requested raw format and ROI in output pixels after binning.
struct CameraGeometry
{
    int width{0},height{0},x{0},y{0},bin{1};
    PixelFormat format{PixelFormat::Mono8};
};
// Shared ASI/QHY form. Owns requested widget values; vendor panels retain
// their separate actual SDK readbacks. Exposure scale is native units per UI unit.
class CameraSettingsEditor final : public QWidget
{
    Q_OBJECT
public:
    CameraSettingsEditor(int widthStep,int heightStep,QWidget* parent);
    void showGeometry(int sensorWidth,int sensorHeight,const std::vector<PixelFormat>& formats,
        const std::vector<int>& bins,const CameraGeometry& geometry);
    CameraGeometry requestedGeometry() const;
    void clearControls();
    void addControl(int id,const QString& name,double minimum,double maximum,double step,double value,
        double nativeUnitsPerDisplayUnit,const QString& suffix,bool writable=true);
    std::map<int,double> controlValues() const;
    // One control's requested value and range, in native units, and setting
    // it (Feature-15, the exposure that follows the sky). None or false when
    // the camera has no such control.
    std::optional<double> controlValue(int id) const;
    std::optional<std::pair<double,double>> controlRange(int id) const;
    bool setControlValue(int id,double nativeValue);
    void setLiveEditing(bool enabled);
signals:
    void liveControlsRequested();
private:
    void updateBounds();
    int sensorWidth_{0},sensorHeight_{0},widthStep_,heightStep_;
    bool updating_{false};
    QComboBox *format_,*bin_;
    QSpinBox *width_,*height_,*x_,*y_;
    QFormLayout* controlForm_;
    // The spin box uses display units. Scale converts its value back to SDK units.
    // Feature-25: a ranged, writable control also has a slider beside the box,
    // logarithmic when its range spans more than three decades (exposure).
    struct Control { QDoubleSpinBox* widget; double scale; QSlider* slider{nullptr}; };
    QSlider* addSlider(QDoubleSpinBox* input,bool logarithmic);
    std::map<int,Control> controls_;
    QTimer* liveTimer_;
    bool liveEditing_{false};
};
