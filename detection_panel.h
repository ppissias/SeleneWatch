#pragma once

#include "capture_settings.h"
#include "detection_result.h"

#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QToolButton;
class MeterSlider;
class PeakHistory;

// Feature-15: the Sky group's unattended-night controls. The window acts on
// them; the panel only holds and shows them.
struct SkyExtras
{
    // Watch from dusk to dawn: below this Sun altitude, in degrees.
    bool night{false};
    double sunLimitDeg{-10};
    // Follow the sky's brightness within these limits.
    bool follow{false};
    double minExposureMs{1}, maxExposureMs{2000};
    double maxGain{0}, gainStep{0};
    bool operator==(const SkyExtras&) const = default;
};

// The observer's trigger controls and a read-only view of source-thread
// measurements. It never calls the detector or accesses its live buffers.
//
// Feature-11: the target, Moon or Sky, is chosen at the top. In Sky mode the
// panel also shows the large-brightening share, the release limit and the
// mask. Its settings carry those only in Sky mode; on the Moon they are off.
// The window owns which settings belong to which target. Feature-13 adds the
// darkening controls at the bottom, in both modes.
class DetectionPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DetectionPanel(QWidget* parent = nullptr);
    [[nodiscard]] DetectionSettings settings() const;
    void setSettings(const DetectionSettings& settings);
    [[nodiscard]] DetectionOverlay overlay() const;
    void setView(const DetectionView& view);
    // The current capture state can change while no new exposure is ready.
    void setMonitoring(bool monitoring);
    [[nodiscard]] QImage overlayImage() const;

    // The target whose settings the panel holds. The choice box only asks
    // for another one; the window saves these settings, then sets it.
    [[nodiscard]] Target target() const;
    // Set before the target's settings. Does not announce it.
    void setTarget(Target target);
    // The target changes only while the stream is stopped.
    void setTargetEditable(bool editable);
    // Adds a shape, in frame pixels, to the mask. False when the mask is
    // full, the shape has fewer than three or too many corners, or the
    // target is not the sky.
    bool addMaskShape(const MaskShape& shape);
    bool addMaskRect(QRect rect);
    [[nodiscard]] bool drawingMask() const;

    [[nodiscard]] SkyExtras skyExtras() const;
    void setSkyExtras(const SkyExtras& extras);
    // What the schedule and the exposure follower found; empty hides it.
    void showNight(const QString& text);
    void showFollow(const QString& text);

    // Named detection profiles (saved per target; Sky horizon mask is optional).
    void reloadProfiles();
    bool saveCurrentProfile(const QString& name, bool includeMask = false);
    bool applyProfile(const QString& name);
    bool deleteProfile(const QString& name);

signals:
    void skyExtrasChanged();
    void settingsChanged();
    void displayChanged();
    // The observer chose another target in the choice box.
    void targetRequested(Target target);
    void maskDrawingChanged(bool drawing);

private:
    void controlsChanged(bool maximumChanged = false);
    void updateDecision();
    void showMask();
    void onSaveProfileClicked();
    void onDeleteProfileClicked();
    void onProfileComboChanged(int index);

    MeterSlider* sigmaSlider_;
    MeterSlider* minSlider_;
    MeterSlider* maxSlider_;
    MeterSlider* compactnessSlider_;
    MeterSlider* referenceSlider_;
    MeterSlider* noiseSlider_;
    QDoubleSpinBox* sigma_;
    QSpinBox* minimum_;
    QSpinBox* maximum_;
    QDoubleSpinBox* compactness_;
    QDoubleSpinBox* referenceTime_;
    QDoubleSpinBox* minimumNoise_;
    QCheckBox* oneFrame_;
    QComboBox* overlay_;
    QComboBox* target_;
    QComboBox* profileCombo_;
    QToolButton* saveProfile_;
    QToolButton* deleteProfile_;
    Target current_{Target::Moon};
    QWidget* sky_;
    QDoubleSpinBox* brightening_;
    QDoubleSpinBox* release_;
    QLabel* maskLabel_;
    QToolButton* drawMask_;
    QToolButton* clearMask_;
    DetectionMask mask_;
    // Feature-15: unattended nights.
    QCheckBox* night_;
    QDoubleSpinBox* sunLimit_;
    QLabel* nightReading_;
    QCheckBox* follow_;
    QDoubleSpinBox* minExposure_;
    QDoubleSpinBox* maxExposure_;
    QDoubleSpinBox* maxGain_;
    QDoubleSpinBox* gainStep_;
    QLabel* followReading_;
    // Feature-13: darkening, in both modes.
    QCheckBox* darkening_;
    // The darkening limits and readout, shown only for the Moon (Feature-22).
    QWidget* darkControls_;
    QSpinBox* maxDark_;
    QDoubleSpinBox* darkBox_;
    QDoubleSpinBox* darkShare_;
    QLabel* darkReading_;
    QLabel* sigmaReading_;
    QLabel* sizeReading_;
    QLabel* state_;
    QLabel* mapScale_;
    PeakHistory* history_;
    QImage overlayImage_;
    DetectionSummary latest_;
    bool monitoring_{false}, settling_{false};
    bool updating_{false};
};
