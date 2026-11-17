#pragma once
#include "camera_family_panel.h"
#include "cameras/touptek_source.h"
#include <optional>
class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;
class RuntimeChooser;

// Feature-16. ToupTek discovery, runtime choice and requested settings,
// with the actual readbacks shown separately, as for QHY.
class ToupTekCameraPanel final : public CameraFamilyPanel
{
    Q_OBJECT
public:
    ToupTekCameraPanel(std::shared_ptr<ToupTekApi> api, QWidget* parent);
    [[nodiscard]] QString sourceName() const override { return "ToupTek camera"; }
    [[nodiscard]] QString sourceKey() const override { return "touptek"; }
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

    [[nodiscard]] ToupTekProfile profile() const { return profile_.value(); }
    [[nodiscard]] ToupTekSettings settings() const;

private:
    void inspectSelected();
    void showProfile();
    void updateReadback();
    std::shared_ptr<ToupTekApi> api_;
    std::optional<ToupTekProfile> profile_;
    ToupTekSource* source_{nullptr};
    RuntimeChooser* runtime_;
    QComboBox* cameras_;
    QPushButton *refresh_, *apply_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false}, monitoring_{false};
};
