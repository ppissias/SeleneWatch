#pragma once

#include "cameras/zwo_source.h"
#include <QtWidgets/QWidget>
#include <map>
#include <optional>

class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;

// Stopped discovery/inspection and requested camera settings. Hardware readback
// remains distinct from edits until Apply/Start; all streaming lives in Session.
class CameraPanel : public QWidget
{
    Q_OBJECT
public:
    explicit CameraPanel(std::shared_ptr<ZwoApi> api,QWidget* parent=nullptr);
    void refresh();
    void setRunning(bool running);
    [[nodiscard]] bool hasCamera() const;
    [[nodiscard]] ZwoProfile profile() const;
    [[nodiscard]] ZwoSettings settings() const;
    void showActual(const ZwoProfile& profile);
signals:
    void applyRequested();
private:
    void inspectSelected();
    void showProfile();
    std::shared_ptr<ZwoApi> api_;
    std::optional<ZwoProfile> profile_;
    QComboBox* cameras_;
    QPushButton *refresh_,*apply_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false};
};
