#pragma once

#include "detection_result.h"

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class MeterSlider;
class PeakHistory;

// The observer's trigger controls and a read-only view of source-thread
// measurements. It never calls the detector or accesses its live buffers.
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

signals:
    void settingsChanged();
    void displayChanged();

private:
    void controlsChanged(bool maximumChanged = false);
    void updateDecision();

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
