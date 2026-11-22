#pragma once

#include <algorithm>

#include "ui/cameras/camera_family_panel.h"
#include "cameras/zwo_source.h"

#include <optional>

class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;

// ZWO ASI: requested settings and separate actual readbacks. Native control
// edits are queued during Preview; layout edits restart it. Monitoring fixes settings.
class CameraPanel final : public CameraFamilyPanel
{
    Q_OBJECT
public:
    explicit CameraPanel(std::shared_ptr<ZwoApi> api,QWidget* parent=nullptr);
    [[nodiscard]] QString sourceName() const override { return "ZWO ASI camera"; }
    [[nodiscard]] QString sourceKey() const override { return "zwo"; }
    void refresh() override;
    void setRunning(bool running,bool monitoring=false) override;
    [[nodiscard]] bool hasCamera() const override;
    void validateSettings() const override;
    [[nodiscard]] std::unique_ptr<FrameSource> makeSource() override;
    [[nodiscard]] bool madeSource() const override { return source_ != nullptr; }
    void forgetSource() override { source_ = nullptr; }
    void showSourceReadback(bool live) override;
    bool readBack(std::string& error) override;
    void requestLiveControls() override;
    [[nodiscard]] CameraSettingsEditor* controls() const override { return editor_; }
    [[nodiscard]] double pixelSizeUm() const override
    {
        return profile_ ? profile_->camera.pixelSizeUm * std::max(1, profile_->current.bin) : 0;
    }
    [[nodiscard]] CameraSupport checkSupport() override;
    [[nodiscard]] QString installHint() const override;
    [[nodiscard]] RuntimeChooser* runtimeChooser() const override { return nullptr; }

    [[nodiscard]] ZwoProfile profile() const;
    [[nodiscard]] ZwoSettings settings() const;
    void showActual(const ZwoProfile& profile);
    void showLiveActual(const ZwoProfile& profile);
private:
    void inspectSelected();
    void showProfile();
    void updateReadback();
    std::shared_ptr<ZwoApi> api_;
    std::optional<ZwoProfile> profile_;
    ZwoSource* source_{nullptr};
    QComboBox* cameras_;
    QPushButton *refresh_,*apply_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false},monitoring_{false};
};
