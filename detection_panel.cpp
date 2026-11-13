#include "detection_panel.h"

#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>

#include <algorithm>
#include <cmath>

// Logarithmic travel keeps small thresholds easy to place while a spin box
// provides exact values. Green marks now; amber holds the recent maximum.
class MeterSlider : public QSlider
{
public:
    MeterSlider(double lower, double upper, QWidget* parent)
        : QSlider(Qt::Horizontal, parent), lower_(lower), upper_(upper)
    {
        setRange(0, 1000);
        setMinimumHeight(30);
    }
    int position(double value) const
    {
        return static_cast<int>(std::round(1000 * std::log(std::clamp(value, lower_, upper_) / lower_)
            / std::log(upper_ / lower_)));
    }
    double number(int position) const { return lower_ * std::pow(upper_ / lower_, position / 1000.0); }
    void markers(double now, double high) { now_ = position(now); high_ = position(high); update(); }
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QSlider::paintEvent(event);
        QPainter painter(this);
        const auto x = [this](int value) { return 8 + value * (width() - 16) / 1000; };
        painter.setPen(QPen(QColor("#008b83"), 2));
        painter.drawLine(x(now_), height() / 2 - 6, x(now_), height() / 2 + 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#b37900"));
        painter.drawPolygon(QPolygonF{QPointF(x(high_) - 4, height() - 1),
            QPointF(x(high_) + 4, height() - 1), QPointF(x(high_), height() - 7)});
    }
private:
    double lower_, upper_;
    int now_{0}, high_{0};
};

class PeakHistory : public QWidget
{
public:
    explicit PeakHistory(QWidget* parent) : QWidget(parent) { setMinimumHeight(105); }
    void setView(const DetectionView& view) { points_ = view.history; threshold_ = view.summary.settings.sigma; update(); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#151c27"));
        const QRectF plot = QRectF(rect()).adjusted(8, 8, -8, -22);
        double high = std::max(threshold_ * 1.3, 1.0);
        for (const auto& p : points_) high = std::max(high, p.peakSigma * 1.05);
        const auto y = [&](double v) { return plot.bottom() - std::log1p(std::max(0.0, v)) / std::log1p(high) * plot.height(); };
        painter.setPen(QPen(QColor("#e3b34a"), 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y(threshold_)), QPointF(plot.right(), y(threshold_)));
        painter.drawText(QPointF(plot.left() + 2, y(threshold_) - 2), QString("K %1").arg(threshold_, 0, 'f', 1));
        QPolygonF trace;
        if (!points_.empty())
        {
            const auto end = points_.back().time;
            for (const auto& p : points_)
            {
                const double seconds = std::chrono::duration<double>(end - p.time).count();
                trace << QPointF(plot.right() - std::clamp(seconds / 10, 0.0, 1.0) * plot.width(), y(p.peakSigma));
            }
        }
        painter.setPen(QPen(QColor("#65d5bd"), 1));
        painter.drawPolyline(trace);
        painter.setPen(QColor("#c8cfda"));
        painter.drawText(8, height() - 6, "Peak sigma, last 10 s of stream time (log scale)");
    }
private:
    std::vector<DetectionHistoryPoint> points_;
    double threshold_{6};
};

DetectionPanel::DetectionPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("detectionPanel");
    setMinimumWidth(350);
    auto* layout = new QGridLayout(this);
    layout->setColumnStretch(0, 1);
    sigma_ = new QDoubleSpinBox(this);
    sigma_->setObjectName("detectionSigma");
    sigma_->setRange(0.1, 1000); sigma_->setDecimals(1); sigma_->setSingleStep(0.1); sigma_->setSuffix(" sigma");
    minimum_ = new QSpinBox(this); minimum_->setObjectName("detectionMinPixels");
    maximum_ = new QSpinBox(this); maximum_->setObjectName("detectionMaxPixels");
    for (auto* spin : {minimum_, maximum_}) { spin->setRange(1, 1'000'000); spin->setSuffix(" px"); }
    sigmaSlider_ = new MeterSlider(0.1, 1000, this); sigmaSlider_->setObjectName("sigmaSlider");
    minSlider_ = new MeterSlider(1, 1'000'000, this); minSlider_->setObjectName("minimumSlider");
    maxSlider_ = new MeterSlider(1, 1'000'000, this); maxSlider_->setObjectName("maximumSlider");
    sigmaReading_ = new QLabel("Waiting for measurements", this); sigmaReading_->setObjectName("sigmaReading");
    sigmaReading_->setWordWrap(true);
    sizeReading_ = new QLabel("Patch: --", this);
    layout->addWidget(new QLabel("Sensitivity K", this), 0, 0); layout->addWidget(sigma_, 0, 1);
    layout->addWidget(sigmaSlider_, 1, 0, 1, 2);
    layout->addWidget(sigmaReading_, 2, 0, 1, 2);
    layout->addWidget(new QLabel("Minimum patch", this), 3, 0); layout->addWidget(minimum_, 3, 1);
    layout->addWidget(minSlider_, 4, 0, 1, 2);
    layout->addWidget(new QLabel("Maximum patch", this), 5, 0); layout->addWidget(maximum_, 5, 1);
    layout->addWidget(maxSlider_, 6, 0, 1, 2);
    layout->addWidget(sizeReading_, 7, 0, 1, 2);
    oneFrame_ = new QCheckBox("Allow a one-frame trigger", this); oneFrame_->setObjectName("oneFrameTrigger");
    oneFrame_->setToolTip("Off requires two consecutive passing frames. A single-frame flash can then be missed.");
    layout->addWidget(oneFrame_, 8, 0, 1, 2);
    history_ = new PeakHistory(this); history_->setObjectName("peakHistory");
    layout->addWidget(history_, 9, 0, 1, 2);
    state_ = new QLabel("Start a source to measure its noise.", this); state_->setObjectName("detectionState");
    state_->setWordWrap(true);
    layout->addWidget(state_, 10, 0, 1, 2);
    layout->addWidget(new QLabel("Preview overlay", this), 11, 0);
    overlay_ = new QComboBox(this); overlay_->setObjectName("detectionOverlay");
    overlay_->addItems({"None", "Sigma map", "Noise map"});
    layout->addWidget(overlay_, 11, 1);
    mapScale_ = new QLabel("Overlays affect the display only.", this); mapScale_->setWordWrap(true);
    layout->addWidget(mapScale_, 12, 0, 1, 2);
    layout->setRowStretch(13, 1);

    connect(sigma_, &QDoubleSpinBox::valueChanged, this, [this] { controlsChanged(); });
    connect(minimum_, &QSpinBox::valueChanged, this, [this] { controlsChanged(); });
    connect(maximum_, &QSpinBox::valueChanged, this, [this] { controlsChanged(true); });
    connect(oneFrame_, &QCheckBox::toggled, this, [this] { controlsChanged(); });
    connect(sigmaSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) sigma_->setValue(sigmaSlider_->number(p)); });
    connect(minSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) minimum_->setValue(static_cast<int>(std::round(minSlider_->number(p)))); });
    connect(maxSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) maximum_->setValue(static_cast<int>(std::round(maxSlider_->number(p)))); });
    connect(overlay_, &QComboBox::currentIndexChanged, this, [this] { emit displayChanged(); });
    setSettings({});
}

DetectionSettings DetectionPanel::settings() const
{
    return {sigma_->value(), minimum_->value(), maximum_->value(), oneFrame_->isChecked()};
}

void DetectionPanel::setSettings(const DetectionSettings& settings)
{
    if (!settings.valid()) return;
    updating_ = true;
    sigma_->setValue(settings.sigma); minimum_->setValue(settings.minPixels); maximum_->setValue(settings.maxPixels);
    oneFrame_->setChecked(settings.oneFrame);
    sigmaSlider_->setValue(sigmaSlider_->position(sigma_->value()));
    minSlider_->setValue(minSlider_->position(minimum_->value()));
    maxSlider_->setValue(maxSlider_->position(maximum_->value()));
    updating_ = false;
}

void DetectionPanel::controlsChanged(bool maximumChanged)
{
    if (updating_) return;
    auto values = settings();
    if (values.minPixels > values.maxPixels)
    {
        if (maximumChanged) values.minPixels = values.maxPixels;
        else values.maxPixels = values.minPixels;
    }
    setSettings(values);
    emit settingsChanged();
}

DetectionOverlay DetectionPanel::overlay() const { return static_cast<DetectionOverlay>(overlay_->currentIndex()); }
QImage DetectionPanel::overlayImage() const { return overlayImage_; }

void DetectionPanel::setView(const DetectionView& view)
{
    const auto& summary = view.summary;
    double high = 0;
    int big = 0;
    for (const auto& point : view.history) { high = std::max(high, point.peakSigma); big = std::max(big, point.brightPixels); }
    sigmaSlider_->markers(summary.peakSigma, high);
    minSlider_->markers(summary.brightPixels, big);
    maxSlider_->markers(summary.brightPixels, big);
    sigmaReading_->setText(QString("Now %1 sigma   Recent high %2 sigma").arg(summary.peakSigma, 0, 'f', 1).arg(high, 0, 'f', 1));
    sizeReading_->setText(QString("Patch now %1 px   Recent high %2 px").arg(summary.brightPixels).arg(big));
    state_->setText(QString("Noise floor %1 ADU; peak (%2, %3).\n%4   Detector %5 ms/frame")
        .arg(summary.noiseFloor, 0, 'f', 2).arg(summary.peakX).arg(summary.peakY)
        .arg(!summary.ready ? "Reference initialized" : summary.passed ? (summary.firing ? "Candidate" : "Passing; awaiting persistence") : "Quiet / patch rejected")
        .arg(summary.processingMs, 0, 'f', 2));
    history_->setView(view);
    overlayImage_ = {};
    if (!view.map || overlay() == DetectionOverlay::None) return;
    const auto& map = *view.map;
    if (map.kind != overlay() || map.values.empty()) return;
    double scale = summary.settings.sigma;
    if (map.kind == DetectionOverlay::Noise)
    {
        std::vector<float> sorted;
        const size_t step = std::max(size_t{1}, (map.values.size() + 4095) / 4096);
        for (size_t i = 0; i < map.values.size(); i += step) sorted.push_back(map.values[i]);
        auto at = sorted.begin() + static_cast<ptrdiff_t>(sorted.size() * 95 / 100);
        std::nth_element(sorted.begin(), at, sorted.end());
        scale = std::max(0.5, static_cast<double>(*at));
    }
    overlayImage_ = QImage(map.width, map.height, QImage::Format_ARGB32);
    QRgb palette[256];
    for (int level = 0; level < 256; ++level)
    {
        const QColor color = level == 255 && map.kind == DetectionOverlay::Sigma ? QColor("#ff3838")
            : QColor::fromHsvF((1.0f - level / 255.0f) * 0.65f, 0.9f, 1.0f);
        palette[level] = qRgba(color.red(), color.green(), color.blue(), 50 + level * 140 / 255);
    }
    for (int y = 0; y < map.height; ++y)
    {
        auto* row = reinterpret_cast<QRgb*>(overlayImage_.scanLine(y));
        for (int x = 0; x < map.width; ++x)
        {
            const double value = map.values[static_cast<size_t>(y) * map.width + x];
            row[x] = palette[static_cast<int>(std::clamp(value / scale, 0.0, 1.0) * 255)];
        }
    }
    mapScale_->setText(map.kind == DetectionOverlay::Sigma
        ? QString("Red: at or above K (%1 sigma). Display only.").arg(scale, 0, 'f', 1)
        : QString("Noise colour scale: 0 to %1 ADU (95th percentile). Display only.").arg(scale, 0, 'f', 2));
}
