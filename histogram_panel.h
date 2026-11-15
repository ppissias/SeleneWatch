#pragma once

#include <QtWidgets/QWidget>

struct Frame;
class HistogramPlot;
class QCheckBox;
class QLabel;

// Draws native sample counts for the latest displayed frame. The window
// controls visibility and refresh rate.
class HistogramPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit HistogramPanel(QWidget* parent = nullptr);
    void setFrame(const Frame& frame);
    void clear();
    void setOptions(bool fitRange, bool logCounts);
    [[nodiscard]] bool fitRange() const;
    [[nodiscard]] bool logCounts() const;

signals:
    void rangeChanged();

private:
    HistogramPlot* plot_;
    QCheckBox* fitRange_;
    QCheckBox* logCounts_;
    QLabel* sampleRange_;
};
