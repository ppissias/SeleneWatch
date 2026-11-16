#include "elided_label.h"

#include <QtGui/QHelpEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolTip>

ElidedLabel::ElidedLabel(const QString& text, Qt::TextElideMode mode, QWidget* parent)
    : QLabel(text, parent), mode_(mode)
{
    setTextFormat(Qt::PlainText);
}

QSize ElidedLabel::minimumSizeHint() const
{
    return {fontMetrics().horizontalAdvance(QStringLiteral("Wwww…")), QLabel::minimumSizeHint().height()};
}

bool ElidedLabel::shortened() const
{
    return fontMetrics().horizontalAdvance(text()) > contentsRect().width();
}

// The whole text as the tool tip, but only while it is shortened and no
// other tool tip was set.
bool ElidedLabel::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip && toolTip().isEmpty() && shortened())
    {
        QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), text(), this);
        return true;
    }
    return QLabel::event(event);
}

void ElidedLabel::paintEvent(QPaintEvent* event)
{
    if (!shortened())
    {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    const QRect area = contentsRect();
    style()->drawItemText(&painter, area, alignment(), palette(), isEnabled(),
        fontMetrics().elidedText(text(), mode_, area.width()), foregroundRole());
}
