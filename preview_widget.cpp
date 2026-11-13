#include "preview_widget.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

#include <utility>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(640, 480);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
    setToolTip("Click the image to place a test flash. Coordinates refer to the original frame.");
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
    selected_.reset();
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

void PreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        const auto position = framePosition(event->position());
        if (position)
        {
            selected_ = position;
            update();
            emit positionSelected(*position);
            return;
        }
    }
    QWidget::mousePressEvent(event);
}
