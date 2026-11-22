#include "ui/widgets/readout_label.h"

#include <algorithm>

ReadoutLabel::ReadoutLabel(const QStringList& longest, QWidget* parent)
    : QLabel(parent)
{
    setWordWrap(true);
    // A shorter text starts at the top of the room, next to its form label.
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    for (const QString& text : longest)
    {
        auto* sample = new QLabel(text, this);
        sample->setWordWrap(true);
        sample->hide();
        longest_.push_back(sample);
    }
}

QString ReadoutLabel::blankLines(int count)
{
    return QString("M\n").repeated(std::max(count, 1)).chopped(1);
}

int ReadoutLabel::heightForWidth(int width) const
{
    int height = QLabel::heightForWidth(width);
    for (const QLabel* sample : longest_) height = std::max(height, sample->heightForWidth(width));
    return height;
}

QSize ReadoutLabel::sizeHint() const
{
    QSize hint = QLabel::sizeHint();
    for (const QLabel* sample : longest_) hint = hint.expandedTo(sample->sizeHint());
    return hint;
}

QSize ReadoutLabel::minimumSizeHint() const
{
    QSize hint = QLabel::minimumSizeHint();
    for (const QLabel* sample : longest_) hint = hint.expandedTo(sample->minimumSizeHint());
    return hint;
}
