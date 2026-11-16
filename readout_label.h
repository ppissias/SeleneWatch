#pragma once

#include <QtWidgets/QLabel>

#include <vector>

// A word-wrapped label for a readout or message whose text changes while the
// window is in use. It keeps the height its longest text needs at its current
// width, so a shorter text, or a text that wraps differently, does not move
// the widgets below it. A text longer than all of them still gets its room.
// A shorter text sits at the top of the room.
//
// The longest texts are given at construction: each kind of text the label
// shows, filled with the widest values it is expected to hold. blankLines()
// reserves a number of lines for a message that comes and goes.
class ReadoutLabel : public QLabel
{
public:
    explicit ReadoutLabel(const QStringList& longest, QWidget* parent = nullptr);

    // A text of count lines, each one character wide.
    [[nodiscard]] static QString blankLines(int count);

    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

private:
    // Hidden children holding the longest texts. They share this label's
    // font and wrapping, so they measure as this label would.
    std::vector<QLabel*> longest_;
};
