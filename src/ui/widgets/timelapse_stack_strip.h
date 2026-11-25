#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <deque>

class QHBoxLayout;
class QLabel;
class QScrollArea;
class QToolButton;

// Compact, collapsible horizontal thumbnail filmstrip for the Time-lapse
// screen (Feature-51 Phase 4). Displays recent completed stacks below the main
// preview; clicking a card displays that stack in the central viewport.
class TimelapseStackStrip : public QWidget
{
    Q_OBJECT
public:
    static constexpr size_t kMaxStacks = 60;

    explicit TimelapseStackStrip(QWidget* parent = nullptr);

    void addStack(int stackNumber, const QImage& image, const QString& tip = {});
    void clear();
    void setHighlightedStack(int stackNumber);
    void setExpanded(bool expanded);

    [[nodiscard]] bool isExpanded() const;
    [[nodiscard]] int stackCount() const { return static_cast<int>(entries_.size()); }
    [[nodiscard]] int latestStackNumber() const { return entries_.empty() ? 0 : entries_.back().number; }
    [[nodiscard]] QImage stackImage(int stackNumber) const;
    [[nodiscard]] QImage stackImageAtIndex(int index) const;
    [[nodiscard]] int stackNumberAtIndex(int index) const;
    [[nodiscard]] QToolButton* toggleButton() const { return toggle_; }

signals:
    void stackSelected(int stackNumber);

private:
    struct Entry
    {
        int number{0};
        QImage image;
        QToolButton* button{nullptr};
    };

    void updateHeader();
    void updateCardStyles();

    QToolButton* toggle_{nullptr};
    QLabel* hintLabel_{nullptr};
    QScrollArea* scroll_{nullptr};
    QWidget* stripContent_{nullptr};
    QHBoxLayout* stripLayout_{nullptr};
    QLabel* emptyLabel_{nullptr};
    std::deque<Entry> entries_;
    int highlightedNumber_{-1};
};
