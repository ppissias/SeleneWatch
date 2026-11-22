#include "ui/widgets/flow_layout.h"

#include <QtWidgets/QWidget>

#include <algorithm>
#include <vector>

FlowLayout::FlowLayout(QWidget* parent, int horizontalSpacing, int verticalSpacing)
    : QLayout(parent), horizontalSpacing_(horizontalSpacing), verticalSpacing_(verticalSpacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }
int FlowLayout::count() const { return static_cast<int>(items_.size()); }
QLayoutItem* FlowLayout::itemAt(int index) const { return items_.value(index); }

QLayoutItem* FlowLayout::takeAt(int index)
{
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }
bool FlowLayout::hasHeightForWidth() const { return true; }
int FlowLayout::heightForWidth(int width) const { return arrange(QRect(0, 0, width, 0), false); }

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem* item : items_)
        if (!item->isEmpty()) size = size.expandedTo(item->minimumSize());
    const QMargins margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}

// Every item on one line.
QSize FlowLayout::sizeHint() const
{
    int width = 0, height = 0, shown = 0;
    for (const QLayoutItem* item : items_)
    {
        if (item->isEmpty()) continue;
        const QSize size = item->sizeHint();
        width += size.width();
        height = std::max(height, size.height());
        ++shown;
    }
    width += std::max(0, shown - 1) * horizontalSpacing_;
    const QMargins margins = contentsMargins();
    return {width + margins.left() + margins.right(), height + margins.top() + margins.bottom()};
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect& rect, bool apply) const
{
    const QRect area = rect.marginsRemoved(contentsMargins());
    std::vector<std::pair<QLayoutItem*, QSize>> line;
    int y = area.y();
    int lineWidth = 0, lineHeight = 0;
    const auto finishLine = [&] {
        if (apply)
        {
            // The line's spare width goes to its first item that expands.
            const auto grows = std::find_if(line.begin(), line.end(),
                [](const auto& placed) { return placed.first->expandingDirections() & Qt::Horizontal; });
            if (grows != line.end()) grows->second.rwidth() += std::max(0, area.width() - lineWidth);
            int x = area.x();
            for (const auto& [item, size] : line)
            {
                item->setGeometry(QRect(QPoint(x, y + (lineHeight - size.height()) / 2), size));
                x += size.width() + horizontalSpacing_;
            }
        }
        y += lineHeight + verticalSpacing_;
        line.clear();
        lineWidth = lineHeight = 0;
    };
    for (QLayoutItem* item : items_)
    {
        if (item->isEmpty()) continue;
        QSize size = item->sizeHint().expandedTo(item->minimumSize());
        // An item wider than the whole line is narrowed, down to its minimum.
        size.setWidth(std::max(std::min(size.width(), area.width()), item->minimumSize().width()));
        const int needed = line.empty() ? size.width() : lineWidth + horizontalSpacing_ + size.width();
        if (!line.empty() && needed > area.width()) finishLine();
        lineWidth = line.empty() ? size.width() : lineWidth + horizontalSpacing_ + size.width();
        lineHeight = std::max(lineHeight, size.height());
        line.emplace_back(item, size);
    }
    if (!line.empty()) finishLine();
    const int used = y - area.y() - (y > area.y() ? verticalSpacing_ : 0);
    const QMargins margins = contentsMargins();
    return used + margins.top() + margins.bottom();
}
