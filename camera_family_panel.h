#pragma once

#include "sources/frame_source.h"

#include <QtWidgets/QWidget>

#include <memory>
#include <string>

class CameraSettingsEditor;

// One camera family's panel as the window sees it: discovery, the requested
// settings, and the source made from them. ZWO, QHY, Player One and ToupTek
// each implement it over their own SDK boundary, and the window keeps a list
// of them instead of naming each family.
//
// The panel remembers the source it made last (the session owns it) so that
// it can show its readbacks and pass live control changes to it. The window
// calls forgetSource() before making any new source and when a start fails.
class CameraFamilyPanel : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
    // The Source entry and the key stored with it ("zwo", "qhy", ...).
    [[nodiscard]] virtual QString sourceName() const = 0;
    [[nodiscard]] virtual QString sourceKey() const = 0;
    // Discovery and inspection, while stopped.
    virtual void refresh() = 0;
    virtual void setRunning(bool running, bool monitoring) = 0;
    [[nodiscard]] virtual bool hasCamera() const = 0;
    // Throws std::invalid_argument when the requested settings cannot work.
    virtual void validateSettings() const = 0;
    // A source with the requested settings; hasCamera() must be true.
    [[nodiscard]] virtual std::unique_ptr<FrameSource> makeSource() = 0;
    [[nodiscard]] virtual bool madeSource() const = 0;
    virtual void forgetSource() = 0;
    // Shows what the source read back: in full after a start, or keeping the
    // observer's edits after a live change in Preview.
    virtual void showSourceReadback(bool live) = 0;
    // While stopped: opens the camera once and shows what it reads back.
    virtual bool readBack(std::string& error) = 0;
    // Preview: hands the edited controls to the running source. Throws when
    // they are unavailable or out of range.
    virtual void requestLiveControls() = 0;
    // The requested controls' form (Feature-15: exposure that follows the sky).
    [[nodiscard]] virtual CameraSettingsEditor* controls() const = 0;

signals:
    void applyRequested();
    void liveControlsRequested();
    void availabilityChanged();
};
