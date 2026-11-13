#pragma once

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <optional>

// Displays a frame fitted to the widget, with a selected pixel marked on
// the screen only. The same image rectangle controls painting and input,
// so a resize or a letterbox margin cannot move the selected frame pixel.
class PreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(QImage image);
    void clear();
    void setDetectionOverlay(QImage image, QRect frameBox);
    [[nodiscard]] std::optional<QPoint> selectedPosition() const;
    [[nodiscard]] std::optional<QPoint> framePosition(QPointF widgetPosition) const;
    [[nodiscard]] QRect imageRect() const;

signals:
    void positionSelected(QPoint position);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QImage image_;
    QImage overlay_;
    QRect detectionBox_;
    std::optional<QPoint> selected_;
};
