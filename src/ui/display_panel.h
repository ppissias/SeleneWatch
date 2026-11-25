#pragma once

#include "display/display_settings.h"

#include <QtWidgets/QWidget>

class QComboBox;
class QDoubleSpinBox;
class QSlider;
class QCheckBox;
class QLabel;

// Live screen-only display stretch controls (Black/White levels, Auto-stretch,
// Invert, Bayer colour). Raw SER evidence and detection are never altered.
class DisplayPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DisplayPanel(QWidget* parent = nullptr);
    [[nodiscard]] DisplaySettings displaySettings() const;
    void setDisplaySettings(DisplaySettings settings);
    void setNativeMaximum(int maximum);
    void showLevels(DisplayLevels levels);
signals:
    void displayChanged();
private:
    void levelsChanged(bool whiteChanged);
    int maximum_{255};
    bool updating_{false};
    DisplaySettings display_;
    QComboBox* mode_{nullptr};
    QDoubleSpinBox* black_{nullptr};
    QDoubleSpinBox* white_{nullptr};
    QSlider* blackSlider_{nullptr};
    QSlider* whiteSlider_{nullptr};
    QCheckBox* invert_{nullptr};
    QCheckBox* debayer_{nullptr};
    QLabel* levels_{nullptr};
};
