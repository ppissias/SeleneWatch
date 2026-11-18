#include "preview_widget.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <utility>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
    setToolTip("Click the image to select a pixel for a test flash. Drag a rectangle around a crater "
        "or other detail to lock on it. In Sky mode, with Draw mask on, a dragged rectangle is left out "
        "of detection. Coordinates refer to the original frame.");
}

void PreviewWidget::setImage(QImage image)
{
    if (image.size() != image_.size())
    {
        selected_.reset();
    }
    image_ = std::move(image);
    update();
}

void PreviewWidget::clear()
{
    image_ = {};
    overlay_ = {};
    detectionBox_ = {};
    selected_.reset();
    lockBox_.reset();
    foundBox_.reset();
    pressPosition_.reset();
    dragging_ = false;
    update();
}

void PreviewWidget::setLockBoxes(std::optional<QRect> locked, std::optional<QRectF> found, bool foundValid)
{
    lockBox_ = locked;
    foundBox_ = found;
    foundValid_ = foundValid;
    update();
}

void PreviewWidget::setAnchorBox(std::optional<QRect> anchor)
{
    anchorBox_ = anchor;
    update();
}

void PreviewWidget::setMask(std::vector<QPolygon> mask)
{
    mask_ = std::move(mask);
    update();
}

void PreviewWidget::setPendingShape(std::vector<QPoint> corners)
{
    pending_ = std::move(corners);
    update();
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && framePosition(event->position())) emit shapeClosed();
}

QRectF PreviewWidget::toWidget(const QRectF& frameRect) const
{
    const QRect image = imageRect();
    const double sx = image.width() / static_cast<double>(image_.width());
    const double sy = image.height() / static_cast<double>(image_.height());
    return QRectF(image.x() + frameRect.x() * sx, image.y() + frameRect.y() * sy,
        frameRect.width() * sx, frameRect.height() * sy);
}

QRect PreviewWidget::dragRegion(QPointF from, QPointF to) const
{
    const QRectF image = imageRect();
    // The image's right and bottom edges belong to the next pixel, outside it.
    auto clamp = [&](QPointF point) {
        return QPointF(std::clamp(point.x(), image.left(), image.right() - 0.01),
            std::clamp(point.y(), image.top(), image.bottom() - 0.01));
    };
    const auto a = framePosition(clamp(from));
    const auto b = framePosition(clamp(to));
    if (!a || !b) return {};
    return QRect(QPoint(std::min(a->x(), b->x()), std::min(a->y(), b->y())),
        QPoint(std::max(a->x(), b->x()), std::max(a->y(), b->y())));
}

void PreviewWidget::setDetectionOverlay(QImage image, QRect frameBox)
{
    overlay_ = std::move(image);
    detectionBox_ = frameBox;
    update();
}

std::optional<QPoint> PreviewWidget::selectedPosition() const
{
    return selected_;
}

QRect PreviewWidget::imageRect() const
{
    if (image_.isNull())
    {
        return {};
    }
    const QRect area = contentsRect().adjusted(1, 1, -1, -1);
    const QSize size = image_.size().scaled(area.size(), Qt::KeepAspectRatio);
    return QRect(area.x() + (area.width() - size.width()) / 2,
        area.y() + (area.height() - size.height()) / 2, size.width(), size.height());
}

std::optional<QPoint> PreviewWidget::framePosition(QPointF position) const
{
    const QRect image = imageRect();
    if (image.isEmpty() || position.x() < image.x() || position.x() >= image.x() + image.width()
        || position.y() < image.y() || position.y() >= image.y() + image.height())
    {
        return std::nullopt;
    }
    return QPoint(static_cast<int>((position.x() - image.x()) * image_.width() / image.width()),
        static_cast<int>((position.y() - image.y()) * image_.height() / image.height()));
}

void PreviewWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#121212"));
    painter.setPen(QColor("#333333"));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    if (image_.isNull())
    {
        painter.setPen(QColor("#dddddd"));
        painter.drawText(rect(), Qt::AlignCenter, "Choose a source and press Start");
        return;
    }

    const QRect image = imageRect();
    painter.drawImage(image, image_);
    if (!overlay_.isNull())
    {
        painter.drawImage(image, overlay_);
        if (!detectionBox_.isEmpty())
        {
            painter.setPen(QPen(QColor("#ffdd66"), 1));
            painter.drawRect(QRectF(image.x() + detectionBox_.x() * image.width() / static_cast<double>(image_.width()),
                image.y() + detectionBox_.y() * image.height() / static_cast<double>(image_.height()),
                detectionBox_.width() * image.width() / static_cast<double>(image_.width()),
                detectionBox_.height() * image.height() / static_cast<double>(image_.height())));
        }
    }
    // A frame corner (x, y) is the top-left of pixel (x, y) on the screen.
    const auto toScreen = [&](QPoint corner) {
        return QPointF(image.x() + corner.x() * image.width() / static_cast<double>(image_.width()),
            image.y() + corner.y() * image.height() / static_cast<double>(image_.height()));
    };
    if (!mask_.empty() || !pending_.empty())
    {
        painter.save();
        painter.setClipRect(image);
        for (const QPolygon& shape : mask_)
        {
            QPolygonF area;
            for (const QPoint& corner : shape) area << toScreen(corner);
            QPainterPath path;
            path.addPolygon(area);
            path.closeSubpath();
            painter.fillPath(path, QColor(0, 0, 0, 90));
            painter.fillPath(path, QBrush(QColor(220, 70, 70, 160), Qt::BDiagPattern));
            painter.setPen(QPen(QColor(220, 70, 70), 1));
            painter.drawPath(path);
        }
        if (!pending_.empty())
        {
            QPolygonF line;
            for (const QPoint& corner : pending_) line << toScreen(corner);
            painter.setPen(QPen(QColor(255, 120, 120), 1, Qt::DashLine));
            painter.drawPolyline(line);
            for (const QPointF& corner : line) painter.drawEllipse(corner, 3, 3);
        }
        painter.restore();
    }
    if (lockBox_ || foundBox_ || anchorBox_ || dragging_)
    {
        painter.save();
        painter.setClipRect(image);
        if (anchorBox_ && anchorBox_->isValid())
        {
            painter.setPen(QPen(QColor("#ff99dd"), 1.5, Qt::DashDotLine));
            painter.drawRect(toWidget(*anchorBox_));
        }
        if (lockBox_)
        {
            painter.setPen(QPen(QColor("#65e5ff"), 1, Qt::DashLine));
            painter.drawRect(toWidget(*lockBox_));
        }
        if (foundBox_)
        {
            painter.setPen(QPen(foundValid_ ? QColor("#66ff88") : QColor("#ffaa33"), 2));
            painter.drawRect(toWidget(*foundBox_));
        }
        if (dragging_ && pressPosition_)
        {
            painter.setPen(QPen(QColor("#65e5ff"), 1));
            painter.drawRect(QRectF(*pressPosition_, dragPosition_).normalized());
        }
        painter.restore();
    }
    if (selected_)
    {
        const QPointF centre(image.x() + (selected_->x() + 0.5) * image.width() / image_.width(),
            image.y() + (selected_->y() + 0.5) * image.height() / image_.height());
        painter.setClipRect(image);
        // A dark outline keeps the cross visible on bright lunar terrain.
        for (int pass = 0; pass < 2; ++pass)
        {
            painter.setPen(QPen(pass == 0 ? Qt::black : QColor("#65e5ff"), pass == 0 ? 3 : 1));
            painter.drawEllipse(centre, 5, 5);
            // Leave the centre clear so the marker does not hide a small blip.
            painter.drawLine(centre + QPointF(-10, 0), centre + QPointF(-6, 0));
            painter.drawLine(centre + QPointF(6, 0), centre + QPointF(10, 0));
            painter.drawLine(centre + QPointF(0, -10), centre + QPointF(0, -6));
            painter.drawLine(centre + QPointF(0, 6), centre + QPointF(0, 10));
        }
    }
}

// A press on the image waits for the release: a short one selects the
// pressed pixel, a longer movement selects a region.
void PreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && framePosition(event->position()))
    {
        pressPosition_ = event->position();
        dragPosition_ = event->position();
        dragging_ = false;
        return;
    }
    QWidget::mousePressEvent(event);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (pressPosition_ && (event->buttons() & Qt::LeftButton))
    {
        dragPosition_ = event->position();
        if ((dragPosition_ - *pressPosition_).manhattanLength() > kDragThreshold) dragging_ = true;
        if (dragging_) update();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !pressPosition_)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QPointF press = *pressPosition_;
    pressPosition_.reset();
    if ((event->position() - press).manhattanLength() > kDragThreshold) dragging_ = true;
    if (dragging_)
    {
        dragging_ = false;
        update();
        const QRect region = dragRegion(press, event->position());
        if (!region.isEmpty()) emit regionSelected(region);
        return;
    }
    const auto position = framePosition(press);
    if (position)
    {
        selected_ = position;
        update();
        emit positionSelected(*position);
    }
}
