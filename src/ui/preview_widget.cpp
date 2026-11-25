#include "ui/preview_widget.h"

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
    setToolTip("Click the image to select a pixel for a test flash. Use the mouse wheel to zoom and drag to pan while "
        "zoomed. Choose a draw tool (Draw lock box, Draw earthshine box, Select lunar anchor, or Draw mask) to drag a "
        "box on the frame; right- or middle-drag still pans. Coordinates refer to the original frame.");
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
        updateIdleCursor();
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
    autoStabilizeBox_.reset();
    autoStabilizeLabel_.clear();
    pressPosition_.reset();
    dragging_ = false;
    leftPanning_ = false;
    pan_ = {0.0, 0.0};
    displayShift_ = {0.0, 0.0};
    panning_ = false;
    updateIdleCursor();
    update();
}

void PreviewWidget::setEmptyText(QString text)
{
    emptyText_ = std::move(text);
    if (image_.isNull()) update();
}

void PreviewWidget::setDragMode(DragMode mode)
{
    if (dragMode_ == mode) return;
    dragMode_ = mode;
    pressPosition_.reset();
    dragging_ = false;
    leftPanning_ = false;
    updateIdleCursor();
    update();
}

void PreviewWidget::setPixelSelectionEnabled(bool enabled)
{
    pixelSelectionEnabled_ = enabled;
    if (!enabled) selected_.reset();
    updateIdleCursor();
    update();
}

void PreviewWidget::setFitToWindow()
{
    fitMode_ = true;
    pan_ = {0.0, 0.0};
    panning_ = false;
    leftPanning_ = false;
    updateIdleCursor();
    update();
    const double z = zoom();
    emit zoomChanged(z, true);
    emit viewChanged(true, z, pan_);
}

void PreviewWidget::setZoom(double factor)
{
    setZoomAndPan(factor, pan_);
}

void PreviewWidget::setZoomAndPan(double factor, QPointF panOffset)
{
    zoom_ = std::clamp(factor, kMinZoom, kMaxZoom);
    fitMode_ = false;
    if (!image_.isNull())
    {
        const QRect area = contentsRect().adjusted(1, 1, -1, -1);
        const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
        const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
        pan_ = clampedPanOffset(area, w, h, panOffset);
    }
    else
    {
        pan_ = {0.0, 0.0};
    }
    updateIdleCursor();
    update();
    emit zoomChanged(zoom_, false);
    emit viewChanged(false, zoom_, pan_);
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

void PreviewWidget::setAutoStabilizeBox(std::optional<QRectF> box, QString label)
{
    if (autoStabilizeBox_ == box && autoStabilizeLabel_ == label) return;
    autoStabilizeBox_ = box;
    autoStabilizeLabel_ = std::move(label);
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

namespace {

// A small label at a box's top-left corner, so boxes of different jobs are told apart.
void drawBoxLabel(QPainter& painter, const QRectF& box, const QString& text, const QColor& colour)
{
    const QFontMetrics metrics(painter.font());
    const QRectF label(box.left(), box.top() - metrics.height() - 2, metrics.horizontalAdvance(text) + 8, metrics.height() + 2);
    painter.fillRect(label, QColor(0, 0, 0, 160));
    painter.setPen(colour);
    painter.drawText(label, Qt::AlignCenter, text);
}

} // namespace

void PreviewWidget::setDragColour(QColor colour)
{
    dragColour_ = colour;
}

void PreviewWidget::setTuneArea(QImage cells, int cellSize, std::optional<QRect> box)
{
    tuneCells_ = std::move(cells);
    tuneCellSize_ = std::max(1, cellSize);
    tuneBox_ = box;
    update();
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && framePosition(event->position())) emit shapeClosed();
}

QRectF PreviewWidget::displayImageRect() const
{
    const QRect base = imageRect();
    if (base.isEmpty() || image_.isNull() || displayShift_.isNull()) return QRectF(base);
    const double sx = base.width() / static_cast<double>(image_.width());
    const double sy = base.height() / static_cast<double>(image_.height());
    return QRectF(base.x() - displayShift_.x() * sx, base.y() - displayShift_.y() * sy,
        base.width(), base.height());
}

QRectF PreviewWidget::toWidget(const QRectF& frameRect) const
{
    const QRectF image = displayImageRect();
    const double sx = image.width() / static_cast<double>(image_.width());
    const double sy = image.height() / static_cast<double>(image_.height());
    return QRectF(image.x() + frameRect.x() * sx, image.y() + frameRect.y() * sy,
        frameRect.width() * sx, frameRect.height() * sy);
}

QRect PreviewWidget::dragRegion(QPointF from, QPointF to) const
{
    const QRect area = contentsRect().adjusted(1, 1, -1, -1);
    const QRectF visible = QRectF(imageRect()).intersected(QRectF(area)).intersected(displayImageRect());
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

bool PreviewWidget::canPan() const
{
    if (image_.isNull() || fitMode_) return false;
    const QRect area = contentsRect().adjusted(1, 1, -1, -1);
    const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
    const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
    return w > area.width() || h > area.height();
}

void PreviewWidget::updateIdleCursor()
{
    if (panning_ || leftPanning_) setCursor(Qt::ClosedHandCursor);
    else if (dragMode_ == DragMode::Region) setCursor(Qt::CrossCursor);
    else if (canPan()) setCursor(Qt::OpenHandCursor);
    else setCursor(pixelSelectionEnabled_ ? Qt::CrossCursor : Qt::ArrowCursor);
}

void PreviewWidget::setDetectionOverlay(QImage image, QRect frameBox)
{
    overlay_ = std::move(image);
    detectionBox_ = frameBox;
    update();
}

void PreviewWidget::setDisplayShift(QPointF frameShift)
{
    if (!std::isfinite(frameShift.x()) || !std::isfinite(frameShift.y())) frameShift = {0.0, 0.0};
    if (std::abs(frameShift.x() - displayShift_.x()) < 1e-6 && std::abs(frameShift.y() - displayShift_.y()) < 1e-6)
        return;
    displayShift_ = frameShift;
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
    const QRect base = imageRect();
    const QRectF display = displayImageRect();
    if (base.isEmpty()
        || position.x() < area.x() || position.x() >= area.x() + area.width()
        || position.y() < area.y() || position.y() >= area.y() + area.height()
        || position.x() < base.x() || position.x() >= base.x() + base.width()
        || position.y() < base.y() || position.y() >= base.y() + base.height()
        || position.x() < display.x() || position.x() >= display.x() + display.width()
        || position.y() < display.y() || position.y() >= display.y() + display.height())
    {
        return std::nullopt;
    }
    const int fx = std::clamp(static_cast<int>(std::floor((position.x() - display.x()) * image_.width() / display.width())),
        0, image_.width() - 1);
    const int fy = std::clamp(static_cast<int>(std::floor((position.y() - display.y()) * image_.height() / display.height())),
        0, image_.height() - 1);
    return QPoint(fx, fy);
}

void PreviewWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    const QColor base = palette().color(QPalette::Base);
    const bool darkPalette = base.lightness() < 40;
    painter.fillRect(rect(), darkPalette ? base : QColor("#121212"));
    if (image_.isNull())
    {
        const QColor border = darkPalette ? palette().color(QPalette::Mid) : QColor("#333333");
        const QColor text = darkPalette ? palette().color(QPalette::WindowText) : QColor("#dddddd");
        painter.setPen(border);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        painter.setPen(text);
        painter.drawText(rect(), Qt::AlignCenter, emptyText_);
        return;
    }

    const QRect viewport = contentsRect().adjusted(1, 1, -1, -1);
    const QRect image = imageRect();
    const QRectF display = displayImageRect();
    const QRect visibleImage = viewport.intersected(image);

    painter.save();
    painter.setClipRect(visibleImage);
    const bool downscaled = image.width() < image_.width() || image.height() < image_.height();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, downscaled || !displayShift_.isNull());
    painter.drawImage(display, image_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (!overlay_.isNull())
    {
        painter.drawImage(display, overlay_);
        if (!detectionBox_.isEmpty())
        {
            painter.setPen(QPen(QColor("#ffdd66"), 1));
            painter.drawRect(toWidget(detectionBox_));
        }
    }
    // A frame corner (x, y) is the top-left of pixel (x, y) on the screen.
    const auto toScreen = [&](QPoint corner) {
        return QPointF(display.x() + corner.x() * display.width() / static_cast<double>(image_.width()),
            display.y() + corner.y() * display.height() / static_cast<double>(image_.height()));
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
    // Feature-26: the earthshine area, cell by cell, and the box it grew from.
    if (!tuneCells_.isNull() || tuneBox_)
    {
        painter.save();
        painter.setClipRect(visibleImage);
        if (!tuneCells_.isNull())
            painter.drawImage(QRectF(toScreen({0, 0}), toScreen({tuneCells_.width() * tuneCellSize_,
                tuneCells_.height() * tuneCellSize_})), tuneCells_);
        if (tuneBox_)
        {
            painter.setPen(QPen(QColor(kEarthshineColour), 1.5, Qt::DashLine));
            painter.drawRect(toWidget(*tuneBox_));
            drawBoxLabel(painter, toWidget(*tuneBox_), "Earthshine", QColor(kEarthshineColour));
        }
        painter.restore();
    }
    if (lockBox_ || foundBox_ || autoStabilizeBox_ || anchorBox_ || dragging_)
    {
        painter.save();
        painter.setClipRect(visibleImage);
        if (anchorBox_ && anchorBox_->isValid())
        {
            painter.setPen(QPen(QColor("#ff99dd"), 1.5, Qt::DashDotLine));
            painter.drawRect(toWidget(*anchorBox_));
        }
        if (autoStabilizeBox_ && autoStabilizeBox_->isValid() && !lockBox_)
        {
            const QColor col("#f0c04a");
            const QRectF r = toWidget(*autoStabilizeBox_);
            painter.setPen(QPen(col, 1.5, Qt::DashLine));
            painter.drawRect(r);
            drawBoxLabel(painter, r, autoStabilizeLabel_.isEmpty() ? QString("Auto stabilize") : autoStabilizeLabel_, col);
        }
        if (lockBox_)
        {
            painter.setPen(QPen(QColor(kLockColour), 1, Qt::DashLine));
            painter.drawRect(toWidget(*lockBox_));
            drawBoxLabel(painter, toWidget(*lockBox_), "Guiding lock", QColor(kLockColour));
        }
        if (foundBox_)
        {
            painter.setPen(QPen(foundValid_ ? QColor("#66ff88") : QColor("#ffaa33"), 2));
            painter.drawRect(toWidget(*foundBox_));
        }
        if (dragging_ && pressPosition_)
        {
            painter.setPen(QPen(dragColour_, 1));
            painter.drawRect(QRectF(*pressPosition_, dragPosition_).normalized());
        }
        painter.restore();
    }
    if (selected_)
    {
        const QPointF centre(display.x() + (selected_->x() + 0.5) * display.width() / image_.width(),
            display.y() + (selected_->y() + 0.5) * display.height() / image_.height());
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
        updateIdleCursor();
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
    updateIdleCursor();
    update();
    emit zoomChanged(zoom_, false);
    emit viewChanged(false, zoom_, pan_);
    event->accept();
}

// In DragMode::Pan, a left-drag pans when zoomed (and a short click still selects
// a pixel when enabled). In DragMode::Region, a left-drag selects a frame box.
// Right- or middle-drag always pans when zoomed.
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
        if (dragMode_ == DragMode::Pan && !pixelSelectionEnabled_ && canPan())
        {
            leftPanning_ = true;
            panLastPos_ = event->position();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        pressPosition_ = event->position();
        dragPosition_ = event->position();
        dragging_ = false;
        leftPanning_ = false;
        return;
    }
    QWidget::mousePressEvent(event);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    const bool middleOrRightPan = panning_ && (event->buttons() & (Qt::MiddleButton | Qt::RightButton));
    const bool activeLeftPan = leftPanning_ && (event->buttons() & Qt::LeftButton);
    if (middleOrRightPan || activeLeftPan)
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
            emit viewChanged(false, zoom_, pan_);
        }
        event->accept();
        return;
    }
    if (pressPosition_ && (event->buttons() & Qt::LeftButton))
    {
        dragPosition_ = event->position();
        if ((dragPosition_ - *pressPosition_).manhattanLength() > kDragThreshold)
        {
            if (dragMode_ == DragMode::Region)
            {
                dragging_ = true;
                update();
            }
            else if (canPan())
            {
                const QPointF delta = event->position() - *pressPosition_;
                pressPosition_.reset();
                dragging_ = false;
                leftPanning_ = true;
                panLastPos_ = event->position();
                const QRect area = contentsRect().adjusted(1, 1, -1, -1);
                const int w = std::max(1, static_cast<int>(std::lround(image_.width() * zoom_)));
                const int h = std::max(1, static_cast<int>(std::lround(image_.height() * zoom_)));
                pan_ = clampedPanOffset(area, w, h, pan_ + delta);
                setCursor(Qt::ClosedHandCursor);
                update();
                emit viewChanged(false, zoom_, pan_);
            }
            else
            {
                dragging_ = true;
            }
        }
        else if (dragging_ && dragMode_ == DragMode::Region)
        {
            update();
        }
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton))
    {
        panning_ = false;
        updateIdleCursor();
        event->accept();
        return;
    }
    if (leftPanning_ && event->button() == Qt::LeftButton)
    {
        leftPanning_ = false;
        pressPosition_.reset();
        dragging_ = false;
        updateIdleCursor();
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
        if (dragMode_ == DragMode::Region)
        {
            const QRect region = dragRegion(press, event->position());
            if (!region.isEmpty()) emit regionSelected(region);
        }
        return;
    }
    if (!pixelSelectionEnabled_) return;
    const auto position = framePosition(press);
    if (position)
    {
        selected_ = position;
        update();
        emit positionSelected(*position);
    }
}
