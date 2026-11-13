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
    [[nodiscard]] QImage overlayImage() const;

signals:
    void settingsChanged();
    void displayChanged();

private:
    void controlsChanged(bool maximumChanged = false);

    MeterSlider* sigmaSlider_;
    MeterSlider* minSlider_;
    MeterSlider* maxSlider_;
    QDoubleSpinBox* sigma_;
    QSpinBox* minimum_;
    QSpinBox* maximum_;
    QCheckBox* oneFrame_;
    QComboBox* overlay_;
    QLabel* sigmaReading_;
    QLabel* sizeReading_;
    QLabel* state_;
    QLabel* mapScale_;
    PeakHistory* history_;
    QImage overlayImage_;
    bool updating_{false};
};
