#pragma once

#include "capture_settings.h"
#include "display_settings.h"
#include "frame_ring_buffer.h"
#include "recording/event_writer.h"

#include <QtWidgets/QWidget>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QSlider;
class QCheckBox;
class QLabel;

// GUI-owned display settings and stopped-session capture settings. The
// preview reads the former; Session copies the latter only at Start.
class DisplayPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DisplayPanel(QWidget* parent = nullptr);
    [[nodiscard]] DisplaySettings displaySettings() const;
    [[nodiscard]] CaptureSettings captureSettings() const;
    [[nodiscard]] int syntheticFormat() const;
    void setDisplaySettings(DisplaySettings settings);
    void setCaptureSettings(CaptureSettings settings);
    void setNativeMaximum(int maximum);
    void setRunning(bool running, bool recording);
    void showLevels(DisplayLevels levels);
    void showUsage(MemoryUsage memory, RingUsage history, WriterUsage writer);
signals:
    void displayChanged();
private:
    void levelsChanged(bool whiteChanged);
    int maximum_{255};
    bool updating_{false};
    DisplaySettings display_;
    QComboBox* mode_;
    QComboBox* format_;
    QDoubleSpinBox* black_;
    QDoubleSpinBox* white_;
    QSlider* blackSlider_;
    QSlider* whiteSlider_;
    QCheckBox* invert_;
    QCheckBox* debayer_;
    QDoubleSpinBox* history_;
    QDoubleSpinBox* post_;
    QDoubleSpinBox* maxPost_;
    QSpinBox* memory_;
    QLabel* levels_;
    QLabel* usage_;
};
