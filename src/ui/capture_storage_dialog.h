#pragma once

#include "capture/frame_ring_buffer.h"
#include "core/capture_settings.h"
#include "recording/event_writer.h"

#include <QtWidgets/QDialog>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QSpinBox;

// Stopped-session clip capture, RAM ceiling, and disk reserve settings, plus
// live buffer/writer usage. Opened via Session → Capture & storage… so the
// floating Display tool window stays purely live screen stretch controls.
class CaptureStorageDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CaptureStorageDialog(QWidget* parent = nullptr);

    [[nodiscard]] CaptureSettings captureSettings() const;
    [[nodiscard]] int syntheticFormat() const;
    void setCaptureSettings(CaptureSettings settings);
    void setRunning(bool running, bool recording);
    void setTestingTools(bool on);
    void showUsage(MemoryUsage memory, RingUsage history, WriterUsage writer);

signals:
    void settingsChanged();

private:
    bool updating_{false};
    Target target_{Target::Moon};
    QDoubleSpinBox* history_{nullptr};
    QDoubleSpinBox* post_{nullptr};
    QDoubleSpinBox* maxPost_{nullptr};
    QSpinBox* memory_{nullptr};
    QDoubleSpinBox* reserve_{nullptr};
    QFormLayout* storageForm_{nullptr};
    QComboBox* format_{nullptr};
    QLabel* usage_{nullptr};
    QPushButton* closeButton_{nullptr};
};
