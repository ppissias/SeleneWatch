#include "ui/histogram_panel.h"
#include "display/preview_image.h"
#include "ui/widgets/theme.h"
#include "ui/widgets/elided_label.h"

#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QString sampleRangeText(int minimum, int maximum, const QString& atZero, int maximumCode, const QString& atMaximum)
{
    return QString("Samples %1–%2 ADU · At 0 %3 · At max code (%4) %5")
        .arg(minimum).arg(maximum).arg(atZero).arg(maximumCode).arg(atMaximum);
}
}

// Paints one native-sample histogram snapshot and its selected count scale on the GUI thread.
class HistogramPlot final : public QWidget
{
public:
    explicit HistogramPlot(QWidget* parent) : QWidget(parent)
    {
        setMinimumHeight(82);
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
        const bool nightRed = currentUiTheme() == UiTheme::NightRed;
        painter.fillRect(rect(), nightRed ? QColor("#0b0202") : QColor("#151c27"));
        const auto& histogram = histogram_;
        if (!histogram.sampleCount) return;

        const int labelHeight = painter.fontMetrics().height();
        const QRectF plot = QRectF(rect()).adjusted(8, 7, -8, -labelHeight - 7);
        if (plot.isEmpty()) return;
        painter.setPen(nightRed ? QColor("#4d1414") : QColor("#354657"));
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
        const QColor barColor = nightRed ? QColor("#cc2e2e") : QColor("#65d5bd");
        for (int i = 0; i < histogram.binCount; ++i)
        {
            const uint64_t count = histogram.bins[static_cast<size_t>(i)];
            if (count == 0) continue;
            const double h = std::max(1.0, plot.height() * scale(count) / top);
            painter.fillRect(QRectF(plot.left() + i * step, plot.bottom() - h, std::max(1.0, step), h), barColor);
        }

        painter.setPen(nightRed ? QColor("#e65555") : QColor("#dbe4ed"));
        const QRectF labels(plot.left(), plot.bottom() + 2, plot.width(), labelHeight);
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
    layout->setContentsMargins(6, 4, 6, 6);
    layout->setSpacing(4);
    // Compact single-line readout that shortens gracefully in the right sidebar
    // while keeping full text for tooltips and tests.
    sampleRange_ = new ElidedLabel("Waiting for frames", Qt::ElideRight, this);
    sampleRange_->setObjectName("histogramSampleRange");
    sampleRange_->setTextFormat(Qt::PlainText);
    sampleRange_->setToolTip("Counts use the original samples. The highest code is based on the stream's declared sample range.");
    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    fitRange_ = new QCheckBox("Fit range", this);
    fitRange_->setObjectName("histogramFitRange");
    fitRange_->setToolTip("Zoom the horizontal axis to the smallest and largest samples in the frame.");
    logCounts_ = new QCheckBox("Log counts", this);
    logCounts_->setObjectName("histogramLogCounts");
    logCounts_->setToolTip("Use a logarithmic count scale to reveal bins with fewer samples.");
    logCounts_->setChecked(true);
    controls->addWidget(fitRange_);
    controls->addWidget(logCounts_);
    controls->addStretch(1);
    layout->addWidget(sampleRange_);
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
    sampleRange_->setText(sampleRangeText(histogram.observedMinimum, histogram.observedMaximum,
        percent(histogram.zeroCount), histogram.maximumCode, percent(histogram.maximumCodeCount)));
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
