#pragma once
#include "core/frame.h"
#include <QtCore/QRect>
#include <QtWidgets/QWidget>
#include <map>
#include <optional>
#include <utility>
#include <vector>
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QFormLayout;
class QToolButton;
class QSlider;
class QTimer;
// Requested raw format and ROI in output pixels after binning.
struct CameraGeometry
{
    int width{0},height{0},x{0},y{0},bin{1};
    PixelFormat format{PixelFormat::Mono8};
    bool operator==(const CameraGeometry&) const = default;
};
// Hardware ROI alignment constraints for a camera family / model.
struct RoiConstraints
{
    int widthStep{1};
    int heightStep{1};
    int xStep{1};
    int yStep{1};
    int minWidth{1};
    int minHeight{1};
    // Total ROI pixel count (width * height) must be a multiple of this value
    // (e.g. 1024 on USB2 ZWO ASI120 cameras).
    int pixelCountMultiple{1};
};
struct AlignedRoi
{
    int x{0},y{0},width{0},height{0};
    bool adjusted{false};
};
// Aligns a requested ROI inside [0, maxWidth] x [0, maxHeight] to camera
// step, minimum, and pixel-count rules. When preserveCenter is true (drawn
// boxes and presets), adjusts origin to keep the requested box centered.
[[nodiscard]] AlignedRoi alignRoi(int maxWidth,int maxHeight,const RoiConstraints& constraints,
    int x,int y,int width,int height,bool preserveCenter=true);
// Preset range for the camera Exposure slider and spin-box arrow step (Feature-39).
// The numeric spin box always keeps the camera's full hardware exposure range.
enum class ExposureRange
{
    Planetary, // Sunlit Moon & planets (<= 20 ms)
    Fast,      // Earthshine flashes & fast sky (5 - 100 ms)
    Sky,       // Slow & faint sky (50 ms - 5 s)
};
// Shared camera form. Owns requested widget values; vendor panels retain
// their separate actual SDK readbacks. Exposure scale is native units per UI unit.
class CameraSettingsEditor final : public QWidget
{
    Q_OBJECT
public:
    CameraSettingsEditor(int widthStep,int heightStep,QWidget* parent);
    CameraSettingsEditor(RoiConstraints constraints,QWidget* parent);
    void setRoiConstraints(const RoiConstraints& constraints);
    [[nodiscard]] RoiConstraints roiConstraints() const { return constraints_; }
    void showGeometry(int sensorWidth,int sensorHeight,const std::vector<PixelFormat>& formats,
        const std::vector<int>& bins,const CameraGeometry& geometry);
    CameraGeometry requestedGeometry() const;
    [[nodiscard]] CameraGeometry actualGeometry() const { return actualGeometry_; }
    [[nodiscard]] bool geometryDirty() const;
    void refreshTheme();
    // ROI helpers shared across all camera families.
    [[nodiscard]] bool drawingRoi() const;
    void setDrawingRoi(bool drawing);
    AlignedRoi setDrawnRoi(const QRect& previewRegion);
    AlignedRoi alignRequestedRoi();
    [[nodiscard]] QString alignmentHint() const;
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
    // Selects the Exposure slider's preset range without changing the live value.
    void setExposureRange(ExposureRange range);
    [[nodiscard]] ExposureRange exposureRange() const { return exposureRange_; }
    void setLiveEditing(bool enabled);
signals:
    void liveControlsRequested();
    void roiDrawingChanged(bool drawing);
    void roiApplyRequested(const QString& note);
    void roiAdjusted(const QString& note);
    void geometryDirtyChanged(bool dirty);
private:
    void notifyGeometryDirty();
    void updateBounds();
    void rebuildRoiPresets();
    void applyAlignedToBoxes(const AlignedRoi& aligned);
    void alignSpinBoxes(bool notify);
    void updateExposureRange();
    [[nodiscard]] int binnedWidth() const;
    [[nodiscard]] int binnedHeight() const;
    int sensorWidth_{0},sensorHeight_{0};
    RoiConstraints constraints_;
    CameraGeometry actualGeometry_;
    bool updating_{false};
    QComboBox *format_,*bin_,*roiPreset_;
    QToolButton *drawRoi_,*centerRoi_,*fullRoi_;
    QSpinBox *width_,*height_,*x_,*y_;
    QFormLayout* form_{nullptr};
    QFormLayout* controlForm_;
    // The spin box uses display units. Scale converts its value back to SDK units.
    // Feature-25/39: ranged writable controls have a slider beside the box;
    // Exposure uses fixed presets in the chosen ExposureRange.
    struct Control { QDoubleSpinBox* widget; double scale; QSlider* slider{nullptr}; };
    QSlider* addSlider(QDoubleSpinBox* input,bool logarithmic);
    std::map<int,Control> controls_;
    ExposureRange exposureRange_{ExposureRange::Fast};
    QComboBox* exposureRangeBox_{nullptr};
    std::optional<int> exposureId_;
    double exposureNativeStep_{0};
    std::vector<double> exposurePresets_;
    QTimer* liveTimer_;
    bool liveEditing_{false};
};
