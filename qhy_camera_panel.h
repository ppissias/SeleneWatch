#pragma once
#include "cameras/qhy_source.h"
#include <QtWidgets/QWidget>
#include <optional>
class QComboBox;
class QPushButton;
class QLabel;
class CameraSettingsEditor;
// QHY discovery and actual readbacks, distinct from edits in the shared form.
// The development adapter exposes bin 1 until hardware qualification is done.
class QhyCameraPanel final : public QWidget
{
    Q_OBJECT
public:
    QhyCameraPanel(std::shared_ptr<QhyApi> api,QWidget* parent);
    void refresh();
    void setRunning(bool running);
    bool hasCamera() const;
    QhyProfile profile() const;
    QhySettings settings() const;
    void showActual(const QhyProfile& profile);
signals:
    void applyRequested();
private:
    void inspectSelected();
    void showProfile();
    std::shared_ptr<QhyApi> api_;
    std::optional<QhyProfile> profile_;
    QComboBox* cameras_;
    QPushButton *refresh_,*apply_;
    QLabel* readback_;
    CameraSettingsEditor* editor_;
    bool running_{false};
};
