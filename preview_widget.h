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
// A click selects a pixel. A drag selects a frame region for a feature lock;
// the widget only reports it. The lock and found boxes are drawn as given.
class PreviewWidget : public QWidget
{
    Q_OBJECT

public:
    // A press that moves farther than this, in screen pixels, is a drag.
    static constexpr int kDragThreshold = 5;

    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(QImage image);
    void clear();
    void setDetectionOverlay(QImage image, QRect frameBox);
    // Frame coordinates. found is drawn green when valid, amber otherwise.
    void setLockBoxes(std::optional<QRect> locked, std::optional<QRectF> found, bool foundValid);
    // Frame coordinates. Optional lunar constant anchor region (Feature-18).
    void setAnchorBox(std::optional<QRect> anchor);
    // Frame coordinates. The detection mask (Feature-11), drawn hatched, and
    // the corners of a shape being drawn (Feature-15), joined by a line.
    void setMask(std::vector<QPolygon> mask);
    void setPendingShape(std::vector<QPoint> corners);
    [[nodiscard]] std::optional<QPoint> selectedPosition() const;
    [[nodiscard]] std::optional<QPoint> framePosition(QPointF widgetPosition) const;
    [[nodiscard]] QRect imageRect() const;

signals:
    void positionSelected(QPoint position);
    void regionSelected(QRect frameRegion);
    // A double-click on the image, which closes a mask shape being drawn.
    void shapeClosed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF toWidget(const QRectF& frameRect) const;
    // The frame pixels a drag between two widget points covers, clamped to the image.
    [[nodiscard]] QRect dragRegion(QPointF from, QPointF to) const;

    QImage image_;
    QImage overlay_;
    QRect detectionBox_;
    std::optional<QPoint> selected_;
    std::optional<QRect> lockBox_;
    std::optional<QRectF> foundBox_;
    std::optional<QRect> anchorBox_;
    bool foundValid_{false};
    std::vector<QPolygon> mask_;
    std::vector<QPoint> pending_;
    // A left press on the image, and whether it has become a drag.
    std::optional<QPointF> pressPosition_;
    QPointF dragPosition_;
    bool dragging_{false};
};
