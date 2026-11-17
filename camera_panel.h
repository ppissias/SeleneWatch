#pragma once

#include "cameras/zwo_source.h"
#include <QtWidgets/QWidget>
#include <optional>

class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;

// Requested settings and separate actual readbacks. Native control edits are
// queued during Preview; layout edits restart it. Monitoring fixes settings.
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
    // The requested controls' form (Feature-15: exposure that follows the sky).
    [[nodiscard]] CameraSettingsEditor* controls() const { return editor_; }
signals:
    void applyRequested();
    void liveControlsRequested();
private:
    void inspectSelected();
    void showProfile();
    void updateReadback();
    std::shared_ptr<ZwoApi> api_;
    std::optional<ZwoProfile> profile_;
    QComboBox* cameras_;
    QPushButton *refresh_,*apply_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false},monitoring_{false};
};
