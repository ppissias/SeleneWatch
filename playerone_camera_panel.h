#pragma once
#include "camera_family_panel.h"
#include "cameras/playerone_source.h"
#include <optional>
class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;
class RuntimeChooser;

// Feature-16. Player One discovery, runtime choice and requested settings,
// with the actual readbacks shown separately, as for ZWO.
class PlayerOneCameraPanel final : public CameraFamilyPanel
{
    Q_OBJECT
public:
    PlayerOneCameraPanel(std::shared_ptr<PlayerOneApi> api, QWidget* parent);
    [[nodiscard]] QString sourceName() const override { return "Player One camera"; }
    [[nodiscard]] QString sourceKey() const override { return "playerone"; }
    void refresh() override;
    void setRunning(bool running, bool monitoring = false) override;
    [[nodiscard]] bool hasCamera() const override { return profile_.has_value(); }
    void validateSettings() const override;
    [[nodiscard]] std::unique_ptr<FrameSource> makeSource() override;
    [[nodiscard]] bool madeSource() const override { return source_ != nullptr; }
    void forgetSource() override { source_ = nullptr; }
    void showSourceReadback(bool live) override;
    bool readBack(std::string& error) override;
    void requestLiveControls() override;
    [[nodiscard]] CameraSettingsEditor* controls() const override { return editor_; }
    [[nodiscard]] CameraSupport checkSupport() override;
    [[nodiscard]] QString installHint() const override;
    [[nodiscard]] RuntimeChooser* runtimeChooser() const override { return runtime_; }

    [[nodiscard]] PlayerOneProfile profile() const { return profile_.value(); }
    [[nodiscard]] PlayerOneSettings settings() const;

private:
    void inspectSelected();
    void showProfile();
    void updateReadback();
    std::shared_ptr<PlayerOneApi> api_;
    std::optional<PlayerOneProfile> profile_;
    PlayerOneSource* source_{nullptr};
    RuntimeChooser* runtime_;
    QComboBox* cameras_;
    QPushButton *refresh_, *apply_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false}, monitoring_{false};
};
