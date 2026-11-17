#pragma once
#include "cameras/qhy_source.h"
#include <QtWidgets/QWidget>
#include <optional>
class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;
// QHY discovery and runtime selection. The form retains requested values
// while actual camera settings are shown separately during Preview.
class QhyCameraPanel final : public QWidget
{
    Q_OBJECT
public:
    QhyCameraPanel(std::shared_ptr<QhyApi> api,QWidget* parent);
    void refresh();
    void setRunning(bool running, bool monitoring=false);
    bool hasCamera() const;
    QhyProfile profile() const;
    QhySettings settings() const;
    void showActual(const QhyProfile& profile);
    void showLiveActual(const QhyProfile& profile);
    // The requested controls' form (Feature-15: exposure that follows the sky).
    [[nodiscard]] CameraSettingsEditor* controls() const { return editor_; }
signals:
    void applyRequested();
    void liveControlsRequested();
    void availabilityChanged();
private:
    void inspectSelected();
    void showProfile();
    void updateReadback();
    void chooseRuntime();
    void setRuntimeOverride(QString path);
    std::shared_ptr<QhyApi> api_;
    std::optional<QhyProfile> profile_;
    QComboBox* cameras_;
    QPushButton *refresh_,*apply_,*chooseRuntime_,*automaticRuntime_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    QString runtimeOverride_;
    bool running_{false}, monitoring_{false};
};
