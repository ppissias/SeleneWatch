#include "histogram_panel.h"
#include "preview_image.h"

#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

// Paints one native-sample histogram snapshot and its selected count scale on the GUI thread.
class HistogramPlot final : public QWidget
{
public:
    explicit HistogramPlot(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(108);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setHistogram(const PreviewHistogram& histogram)
    {
        histogram_ = histogram;
        update();
    }

    void setLogCounts(bool enabled)
    {
        logCounts_ = enabled;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#151c27"));
        const auto& histogram = histogram_;
        if (!histogram.sampleCount) return;

        const int labelHeight = painter.fontMetrics().height();
        const QRectF plot = QRectF(rect()).adjusted(10, 9, -10, -labelHeight - 9);
        if (plot.isEmpty()) return;
        painter.setPen(QColor("#354657"));
        for (double fraction : {0.0, 0.5, 1.0})
        {
            const double y = plot.top() + fraction * plot.height();
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        const uint64_t maximum = *std::max_element(histogram.bins.begin(), histogram.bins.begin() + histogram.binCount);
        const auto scale = [this](uint64_t count) {
            const double value = static_cast<double>(count);
            return logCounts_ ? std::log1p(value) : value;
        };
        const double top = scale(maximum);
        const double step = plot.width() / histogram.binCount;
        for (int i = 0; i < histogram.binCount; ++i)
        {
            const uint64_t count = histogram.bins[static_cast<size_t>(i)];
            if (count == 0) continue;
            const double h = std::max(1.0, plot.height() * scale(count) / top);
            painter.fillRect(QRectF(plot.left() + i * step, plot.bottom() - h, std::max(1.0, step), h), QColor("#65d5bd"));
        }

        painter.setPen(QColor("#dbe4ed"));
        const QRectF labels(plot.left(), plot.bottom() + 3, plot.width(), labelHeight);
        painter.drawText(labels, Qt::AlignLeft, QString::number(histogram.rangeMinimum));
        if (histogram.rangeMaximum != histogram.rangeMinimum)
            painter.drawText(labels, Qt::AlignRight, QString::number(histogram.rangeMaximum));
        painter.drawText(labels, Qt::AlignHCenter, "Native ADU");
    }

private:
    PreviewHistogram histogram_;
    bool logCounts_{true};
};

HistogramPanel::HistogramPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    sampleRange_ = new QLabel("Waiting for frames", this);
    sampleRange_->setObjectName("histogramSampleRange");
    sampleRange_->setWordWrap(true);
    sampleRange_->setTextFormat(Qt::PlainText);
    sampleRange_->setToolTip("Counts use the original samples. The highest code is based on the stream's declared sample range.");
    fitRange_ = new QCheckBox("Fit range", this);
    fitRange_->setObjectName("histogramFitRange");
    fitRange_->setToolTip("Zoom the horizontal axis to the smallest and largest samples in the frame.");
    logCounts_ = new QCheckBox("Log counts", this);
    logCounts_->setObjectName("histogramLogCounts");
    logCounts_->setToolTip("Use a logarithmic count scale to reveal bins with fewer samples.");
    logCounts_->setChecked(true);
    controls->addWidget(sampleRange_, 1);
    controls->addWidget(fitRange_);
    controls->addWidget(logCounts_);
    layout->addLayout(controls);
    plot_ = new HistogramPlot(this);
    plot_->setObjectName("histogramPlot");
    layout->addWidget(plot_, 1);

    connect(fitRange_, &QCheckBox::toggled, this, &HistogramPanel::rangeChanged);
    connect(logCounts_, &QCheckBox::toggled, plot_, &HistogramPlot::setLogCounts);
}

// Both plot ranges report endpoint percentages for the native code range.
void HistogramPanel::setFrame(const Frame& frame)
{
    const auto histogram = frameToPreviewHistogram(frame, fitRange());
    if (!histogram.sampleCount)
    {
        clear();
        return;
    }
    plot_->setHistogram(histogram);
    const auto percent = [total = static_cast<double>(histogram.sampleCount)](uint64_t count) {
        const double value = 100.0 * static_cast<double>(count) / total;
        return count > 0 && value < 0.01 ? QString("<0.01%") : QString::number(value, 'f', 2) + '%';
    };
    sampleRange_->setText(QString("Samples %1–%2 ADU   ·   At 0 %3   ·   At max code (%4) %5")
        .arg(histogram.observedMinimum).arg(histogram.observedMaximum)
        .arg(percent(histogram.zeroCount)).arg(histogram.maximumCode).arg(percent(histogram.maximumCodeCount)));
}

void HistogramPanel::clear()
{
    plot_->setHistogram({});
    sampleRange_->setText("Waiting for frames");
}

void HistogramPanel::setOptions(bool fitRange, bool logCounts)
{
    fitRange_->setChecked(fitRange);
    logCounts_->setChecked(logCounts);
}

bool HistogramPanel::fitRange() const { return fitRange_->isChecked(); }
bool HistogramPanel::logCounts() const { return logCounts_->isChecked(); }
