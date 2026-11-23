#pragma once

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtGui/QPolygon>
#include <QtWidgets/QWidget>

#include <optional>
#include <vector>

// Displays a frame fitted to the widget, with a selected pixel marked on
// the screen only. The same image rectangle controls painting and input,
// so a resize or a letterbox margin cannot move the selected frame pixel.
//
// In DragMode::Pan (default), a left-drag pans when zoomed in and draws no
// rectangle; right- and middle-drag also pan in every mode. In DragMode::Region
// (while an explicit draw tool is active), a left-drag selects a frame region
// and reports it via regionSelected. A short click selects a pixel when pixel
// selection is enabled. The lock and found boxes are drawn as given.
class PreviewWidget : public QWidget
{
    Q_OBJECT

public:
    enum class DragMode
    {
        Pan,
        Region
    };

    // A press that moves farther than this, in screen pixels, is a drag.
    static constexpr int kDragThreshold = 5;
    static constexpr double kMinZoom = 0.25;
    static constexpr double kMaxZoom = 8.0;

    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(QImage image);
    [[nodiscard]] QImage image() const { return image_; }
    void clear();
    void setEmptyText(QString text);
    void setDragMode(DragMode mode);
    [[nodiscard]] DragMode dragMode() const { return dragMode_; }
    void setPixelSelectionEnabled(bool enabled);
    [[nodiscard]] bool pixelSelectionEnabled() const { return pixelSelectionEnabled_; }
    void setFitToWindow();
    void setZoom(double factor);
    void setZoomAndPan(double factor, QPointF panOffset);
    [[nodiscard]] bool isFitToWindow() const { return fitMode_; }
    [[nodiscard]] double zoom() const;
    [[nodiscard]] QPointF panOffset() const { return pan_; }
    void setDetectionOverlay(QImage image, QRect frameBox);
    // Frame coordinates. found is drawn green when valid, amber otherwise.
    void setLockBoxes(std::optional<QRect> locked, std::optional<QRectF> found, bool foundValid);
    // Frame coordinates. Optional lunar constant anchor region (Feature-18).
    void setAnchorBox(std::optional<QRect> anchor);
    // Frame coordinates. The detection mask (Feature-11), drawn hatched, and
    // the corners of a shape being drawn (Feature-15), joined by a line.
    void setMask(std::vector<QPolygon> mask);
    void setPendingShape(std::vector<QPoint> corners);
    // Feature-28: the colours of the boxes the observer draws, and of the
    // rubber band while one is being drawn, so a guiding lock box is never
    // taken for an earthshine box.
    static constexpr const char* kLockColour = "#65e5ff";
    static constexpr const char* kEarthshineColour = "#c38cff";
    static constexpr const char* kCameraRoiColour = "#ffb84d";
    void setDragColour(QColor colour);
    // Feature-26: the earthshine area auto-tune measures, as one pixel per
    // square cell of cellSize frame pixels (transparent outside it), and the
    // observer's box it grew from. A null image and no box clear it.
    void setTuneArea(QImage cells, int cellSize, std::optional<QRect> box);
    [[nodiscard]] std::optional<QPoint> selectedPosition() const;
    [[nodiscard]] std::optional<QPoint> framePosition(QPointF widgetPosition) const;
    [[nodiscard]] QRect imageRect() const;

signals:
    void positionSelected(QPoint position);
    void regionSelected(QRect frameRegion);
    // A double-click on the image, which closes a mask shape being drawn.
    void shapeClosed();
    void zoomChanged(double effectiveZoom, bool fitToWindow);
    void viewChanged(bool fitToWindow, double zoom, QPointF panOffset);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF toWidget(const QRectF& frameRect) const;
    // The frame pixels a drag between two widget points covers, clamped to the image.
    [[nodiscard]] QRect dragRegion(QPointF from, QPointF to) const;
    [[nodiscard]] QPointF clampedPanOffset(const QRect& area, int scaledW, int scaledH, QPointF desired) const;
    [[nodiscard]] bool canPan() const;
    void updateIdleCursor();

    QImage image_;
    QString emptyText_{"Choose a source and press Start"};
    QImage overlay_;
    QRect detectionBox_;
    std::optional<QPoint> selected_;
    std::optional<QRect> lockBox_;
    std::optional<QRectF> foundBox_;
    std::optional<QRect> anchorBox_;
    bool foundValid_{false};
    std::vector<QPolygon> mask_;
    std::vector<QPoint> pending_;
    QImage tuneCells_;
    int tuneCellSize_{1};
    std::optional<QRect> tuneBox_;
    QColor dragColour_{kLockColour};
    DragMode dragMode_{DragMode::Pan};
    bool pixelSelectionEnabled_{true};
    // A left press on the image, and whether it has become a region drag or pan.
    std::optional<QPointF> pressPosition_;
    QPointF dragPosition_;
    bool dragging_{false};
    bool leftPanning_{false};
    // Zoom and pan state. Fit mode scales to the widget; manual zoom uses zoom_ and pan_.
    bool fitMode_{true};
    double zoom_{1.0};
    QPointF pan_{0.0, 0.0};
    bool panning_{false};
    QPointF panLastPos_;
};
