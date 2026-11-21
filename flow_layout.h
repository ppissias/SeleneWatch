#pragma once

#include <QtCore/QList>
#include <QtWidgets/QLayout>

// Lays its items out left to right, like words in a paragraph: an item that
// does not fit on the line starts the next one. Each item gets its preferred
// size, and items on a line are centred on it. The minimum width is that of
// the widest item, so a narrow window wraps the items instead of squeezing
// their text (Feature-23). Hidden items take no space.
class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr, int horizontalSpacing = 6, int verticalSpacing = 4);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;

    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize minimumSize() const override;
    [[nodiscard]] QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    // Places the items within rect when apply is set; returns the height used.
    int arrange(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> items_;
    int horizontalSpacing_;
    int verticalSpacing_;
};
