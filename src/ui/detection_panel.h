#pragma once

#include "core/capture_settings.h"
#include "detection/detection_result.h"
#include "settings/watch_settings.h"

#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;
class MeterSlider;
class PeakHistory;

// The observer's trigger controls and a read-only view of source-thread
// measurements. It never calls the detector or accesses its live buffers.
//
// Feature-11: the target, Moon or Sky, is chosen at the top. In Sky mode the
// panel also shows the large-brightening share, the release limit and the
// mask. Its settings carry those only in Sky mode; on the Moon they are off.
// The window owns which settings belong to which target. Feature-31: the
// watch is the Moon's flashes, the Moon's shadows or the sky; the darkening
// limits are shown for the shadows, the brightness ceiling for the flashes.
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
    // Feature-31: what is watched, of which the target is the Moon or the sky.
    [[nodiscard]] Watch watch() const;
    // Set before the watch's settings. Does not announce it.
    void setWatch(Watch watch);
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
    // What the exposure follower found; empty hides it.
    void showFollow(const QString& text);
    // Feature-36: shown for the slow sky watch; changed only outside Monitoring.
    [[nodiscard]] WatchTimelapse watchTimelapse() const;
    void setWatchTimelapse(const WatchTimelapse& timelapse);
    void setWatchTimelapseEditable(bool editable);

    // Named detection profiles (saved per target; Sky horizon mask is optional).
    void reloadProfiles();
    bool saveCurrentProfile(const QString& name, bool includeMask = false);
    bool applyProfile(const QString& name);
    bool deleteProfile(const QString& name);

signals:
    void skyExtrasChanged();
    void watchTimelapseChanged();
    void settingsChanged();
    void displayChanged();
    // The observer chose another watch in the choice box.
    void watchRequested(Watch watch);
    void maskDrawingChanged(bool drawing);
    // Feature-26: the prominent Auto-tune button was pressed.
    void autoTuneRequested();

private:
    void controlsChanged(bool maximumChanged = false);
    void updateDecision();
    void showMask();
    void onSaveProfileClicked();
    void onDeleteProfileClicked();
    void onProfileComboChanged(int index);

    QPushButton* autoTune_;
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
    Watch watch_{Watch::MoonFlashes};
    QWidget* sky_;
    QDoubleSpinBox* brightening_;
    QDoubleSpinBox* release_;
    QLabel* maskLabel_;
    QToolButton* drawMask_;
    QToolButton* clearMask_;
    DetectionMask mask_;
    // Feature-15: the exposure follower.
    QCheckBox* follow_;
    QDoubleSpinBox* minExposure_;
    QDoubleSpinBox* maxExposure_;
    QDoubleSpinBox* maxGain_;
    QDoubleSpinBox* gainStep_;
    QLabel* followReading_;
    // Feature-36.
    QWidget* timelapseRow_;
    QCheckBox* timelapse_;
    QSpinBox* timelapseInterval_;
    // Feature-31: the flashes' ceiling, shown only for the flashes.
    QWidget* flashControls_;
    QDoubleSpinBox* ignoreAbove_;
    // The darkening limits and readout, shown only for the shadows (Feature-31).
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
