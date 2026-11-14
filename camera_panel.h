#pragma once

#include "cameras/zwo_source.h"
#include <QtWidgets/QWidget>
#include <map>
#include <optional>

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QFormLayout;
class QTimer;

// Stopped discovery/inspection and requested camera settings. Hardware readback
// remains distinct from edits. Preview control edits are queued to the source;
// layout edits require Apply. Monitoring freezes acquisition settings.
class CameraPanel : public QWidget
{
    Q_OBJECT
public:
    explicit CameraPanel(std::shared_ptr<ZwoApi> api,QWidget* parent=nullptr);
    void refresh();
    void setRunning(bool running,bool monitoring=false);
    [[nodiscard]] bool hasCamera() const;
    [[nodiscard]] ZwoProfile profile() const;
    [[nodiscard]] ZwoSettings settings() const;
    void showActual(const ZwoProfile& profile);
    void showLiveActual(const ZwoProfile& profile);
signals:
    void applyRequested();
    void liveControlsRequested();
private:
    void inspectSelected();
    void showProfile();
    void updateBounds();
    void updateReadback();
    std::shared_ptr<ZwoApi> api_;
    std::optional<ZwoProfile> profile_;
    QComboBox* cameras_; QComboBox* format_; QComboBox* bin_;
    QSpinBox *width_,*height_,*x_,*y_;
    QPushButton *refresh_,*apply_;
    QLabel* readback_;
    QFormLayout* controlForm_;
    QWidget* controls_;
    std::map<ZwoControl,QDoubleSpinBox*> values_;
    QTimer* liveTimer_;
    bool running_{false}, monitoring_{false}, updating_{false};
};
