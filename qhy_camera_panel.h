#pragma once
#include "camera_family_panel.h"
#include "cameras/qhy_source.h"
#include <optional>
class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;
class RuntimeChooser;
// QHY discovery and runtime selection. The form retains requested values
// while actual camera settings are shown separately during Preview.
class QhyCameraPanel final : public CameraFamilyPanel
{
    Q_OBJECT
public:
    QhyCameraPanel(std::shared_ptr<QhyApi> api,QWidget* parent);
    [[nodiscard]] QString sourceName() const override { return "QHY camera"; }
    [[nodiscard]] QString sourceKey() const override { return "qhy"; }
    void refresh() override;
    void setRunning(bool running, bool monitoring=false) override;
    bool hasCamera() const override;
    void validateSettings() const override;
    [[nodiscard]] std::unique_ptr<FrameSource> makeSource() override;
    [[nodiscard]] bool madeSource() const override { return source_ != nullptr; }
    void forgetSource() override { source_ = nullptr; }
    void showSourceReadback(bool live) override;
    bool readBack(std::string& error) override;
    void requestLiveControls() override;
    [[nodiscard]] CameraSettingsEditor* controls() const override { return editor_; }

    QhyProfile profile() const;
    QhySettings settings() const;
    void showActual(const QhyProfile& profile);
    void showLiveActual(const QhyProfile& profile);
private:
    void inspectSelected();
    void showProfile();
    void updateReadback();
    std::shared_ptr<QhyApi> api_;
    std::optional<QhyProfile> profile_;
    QhySource* source_{nullptr};
    QComboBox* cameras_;
    QPushButton *refresh_,*apply_;
    RuntimeChooser* runtime_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false}, monitoring_{false};
};
