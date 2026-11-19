#include "preview_widget.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
    setToolTip("Click the image to select a pixel for a test flash. Drag a rectangle around a crater "
        "or other detail to lock on it. In Sky mode, with Draw mask on, a dragged rectangle is left out "
        "of detection. Use the mouse wheel to zoom and right- or middle-drag to pan while zoomed. "
        "Coordinates refer to the original frame.");
}

void PreviewWidget::setImage(QImage image)
{
    const bool sizeChanged = image.size() != image_.size();
    if (sizeChanged)
    {
        selected_.reset();
        pan_ = {0.0, 0.0};
    }
    image_ = std::move(image);
    if (sizeChanged)
    {
        emit zoomChanged(zoom(), fitMode_);
    }
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
    pan_ = {0.0, 0.0};
    panning_ = false;
    setCursor(Qt::CrossCursor);
    update();
}

void PreviewWidget::setFitToWindow()
{
    fitMode_ = true;
    pan_ = {0.0, 0.0};
    panning_ = false;
    setCursor(Qt::CrossCursor);
    update();
    emit zoomChanged(zoom(), true);
}

void PreviewWidget::setZoom(double factor)
{
    zoom_ = std::clamp(factor, kMinZoom, kMaxZoom);
    fitMode_ = false;
    if (!image_.isNull())
    {
        const QRect area = contentsRect().adjusted(1, 1, -1, -1);
        const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
        const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
        pan_ = clampedPanOffset(area, w, h, pan_);
    }
    else
    {
        pan_ = {0.0, 0.0};
    }
    update();
    emit zoomChanged(zoom_, false);
}

double PreviewWidget::zoom() const
{
    if (!fitMode_) return zoom_;
    if (image_.isNull() || image_.width() <= 0) return 1.0;
    const QRect r = imageRect();
    return r.isEmpty() ? 1.0 : r.width() / static_cast<double>(image_.width());
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
    const QRect area = contentsRect().adjusted(1, 1, -1, -1);
    const QRectF visible = QRectF(imageRect()).intersected(QRectF(area));
    if (visible.isEmpty()) return {};
    // The visible image's right and bottom edges belong to the next pixel, outside it.
    auto clamp = [&](QPointF point) {
        return QPointF(std::clamp(point.x(), visible.left(), visible.right() - 0.01),
            std::clamp(point.y(), visible.top(), visible.bottom() - 0.01));
    };
    const auto a = framePosition(clamp(from));
    const auto b = framePosition(clamp(to));
    if (!a || !b) return {};
    return QRect(QPoint(std::min(a->x(), b->x()), std::min(a->y(), b->y())),
        QPoint(std::max(a->x(), b->x()), std::max(a->y(), b->y())));
}

QPointF PreviewWidget::clampedPanOffset(const QRect& area, int scaledW, int scaledH, QPointF desired) const
{
    const double maxX = scaledW > area.width() ? (scaledW - area.width()) * 0.5 : 0.0;
    const double maxY = scaledH > area.height() ? (scaledH - area.height()) * 0.5 : 0.0;
    return QPointF(std::clamp(desired.x(), -maxX, maxX), std::clamp(desired.y(), -maxY, maxY));
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
    if (fitMode_)
    {
        const QSize size = image_.size().scaled(area.size(), Qt::KeepAspectRatio);
        return QRect(area.x() + (area.width() - size.width()) / 2,
            area.y() + (area.height() - size.height()) / 2, size.width(), size.height());
    }
    const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
    const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
    const QPointF pan = clampedPanOffset(area, w, h, pan_);
    const int x = area.x() + static_cast<int>(std::lround((area.width() - w) * 0.5 + pan.x()));
    const int y = area.y() + static_cast<int>(std::lround((area.height() - h) * 0.5 + pan.y()));
    return QRect(x, y, w, h);
}

std::optional<QPoint> PreviewWidget::framePosition(QPointF position) const
{
    const QRect area = contentsRect().adjusted(1, 1, -1, -1);
    const QRect image = imageRect();
    if (image.isEmpty()
        || position.x() < area.x() || position.x() >= area.x() + area.width()
        || position.y() < area.y() || position.y() >= area.y() + area.height()
        || position.x() < image.x() || position.x() >= image.x() + image.width()
        || position.y() < image.y() || position.y() >= image.y() + image.height())
    {
        return std::nullopt;
    }
    const int fx = std::clamp(static_cast<int>((position.x() - image.x()) * image_.width() / image.width()),
        0, image_.width() - 1);
    const int fy = std::clamp(static_cast<int>((position.y() - image.y()) * image_.height() / image.height()),
        0, image_.height() - 1);
    return QPoint(fx, fy);
}

void PreviewWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#121212"));
    if (image_.isNull())
    {
        painter.setPen(QColor("#333333"));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        painter.setPen(QColor("#dddddd"));
        painter.drawText(rect(), Qt::AlignCenter, "Choose a source and press Start");
        return;
    }

    const QRect viewport = contentsRect().adjusted(1, 1, -1, -1);
    const QRect image = imageRect();
    const QRect visibleImage = viewport.intersected(image);

    painter.save();
    painter.setClipRect(viewport);
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
        painter.setClipRect(visibleImage);
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
        painter.setClipRect(visibleImage);
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
        painter.save();
        painter.setClipRect(visibleImage);
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
        painter.restore();
    }
    painter.restore();

    painter.setPen(QColor("#333333"));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void PreviewWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!image_.isNull())
    {
        if (!fitMode_)
        {
            const QRect area = contentsRect().adjusted(1, 1, -1, -1);
            const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
            const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
            pan_ = clampedPanOffset(area, w, h, pan_);
        }
        else
        {
            emit zoomChanged(zoom(), true);
        }
    }
}

void PreviewWidget::wheelEvent(QWheelEvent* event)
{
    if (image_.isNull())
    {
        event->ignore();
        return;
    }
    double delta = event->angleDelta().y();
    if (delta == 0.0) delta = event->pixelDelta().y();
    if (delta == 0.0)
    {
        event->ignore();
        return;
    }

    const QRect area = contentsRect().adjusted(1, 1, -1, -1);
    const QRect oldRect = imageRect();
    const double oldScale = zoom();
    const double steps = delta / 120.0;
    const double newZoom = std::clamp(oldScale * std::pow(1.25, steps), kMinZoom, kMaxZoom);

    // Keep the frame point under the cursor fixed on screen as zoom changes.
    const QPointF cursor = event->position();
    const double fx = (cursor.x() - oldRect.x()) * image_.width() / static_cast<double>(std::max(1, oldRect.width()));
    const double fy = (cursor.y() - oldRect.y()) * image_.height() / static_cast<double>(std::max(1, oldRect.height()));
    const double newW = image_.width() * newZoom;
    const double newH = image_.height() * newZoom;
    const double newX = cursor.x() - fx * newZoom;
    const double newY = cursor.y() - fy * newZoom;
    const QPointF desiredPan(
        newX - area.x() - (area.width() - newW) * 0.5,
        newY - area.y() - (area.height() - newH) * 0.5);

    fitMode_ = false;
    zoom_ = newZoom;
    pan_ = clampedPanOffset(area, static_cast<int>(std::lround(newW)), static_cast<int>(std::lround(newH)), desiredPan);
    update();
    emit zoomChanged(zoom_, false);
    event->accept();
}

// A press on the image waits for the release: a short one selects the
// pressed pixel, a longer movement selects a region. Right- or middle-drag
// pans when zoomed.
void PreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if ((event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) && !image_.isNull() && !fitMode_)
    {
        panning_ = true;
        panLastPos_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
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
    if (panning_ && (event->buttons() & (Qt::MiddleButton | Qt::RightButton)))
    {
        const QPointF delta = event->position() - panLastPos_;
        panLastPos_ = event->position();
        if (!image_.isNull())
        {
            const QRect area = contentsRect().adjusted(1, 1, -1, -1);
            const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
            const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
            pan_ = clampedPanOffset(area, w, h, pan_ + delta);
            update();
        }
        event->accept();
        return;
    }
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
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton))
    {
        panning_ = false;
        setCursor(Qt::CrossCursor);
        event->accept();
        return;
    }
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
