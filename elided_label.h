#pragma once

#include <QtWidgets/QLabel>

// A one-line label that may be narrower than its text. The text it cannot
// show is replaced by "…" instead of being cut off mid-letter, and the whole
// text is its tool tip while it is shortened. text() is always the whole text.
//
// It asks for its whole text's width, so a layout shortens it only when the
// window is too narrow. Its minimum is a few characters, unless a minimum
// width is set.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(const QString& text = {}, Qt::TextElideMode mode = Qt::ElideRight, QWidget* parent = nullptr);

    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] bool shortened() const;

    Qt::TextElideMode mode_;
};
