#include "ui/detection_panel.h"
#include "ui/widgets/control_help.h"
#include "ui/widgets/theme.h"
#include "settings/detection_profiles.h"
#include "display/preview_image.h"
#include "ui/widgets/readout_label.h"
#include "sky/sky_exposure.h"
#include "detection/target_defaults.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
double boxArea(const DetectionSummary& summary)
{
    return static_cast<double>(summary.boxWidth) * summary.boxHeight;
}

const QString kSettlingReading = "Settling after camera change — relearning before threshold feedback.";
const QString kSettlingState = "Preview capture is OFF. Settling is an estimate; Monitoring starts fresh with verified camera settings.";
const QString kOverlaySkipped = "Overlay skipped: RAM ceiling reached. Detection continues.";

// Feature-45: with darkening, the peak is the deepest fall.
QString peakText(double peak, double threshold, double high, bool falls)
{
    return QString("%1 %2 / K %3 sigma (%4% of threshold). Recent %5 %6.").arg(falls ? "Deepest fall" : "Peak")
        .arg(peak, 0, 'f', 1).arg(threshold, 0, 'f', 1).arg(100 * peak / threshold, 0, 'f', 0)
        .arg(falls ? "deepest" : "high").arg(high, 0, 'f', 1);
}

QString patchText(int pixels, int high, double boxRatio, const DetectionSettings& settings)
{
    return QString("Patch %1 px (allowed %2–%3 px). Recent high %4.\nBox / patch %5. Maximum %6.")
        .arg(pixels).arg(settings.minPixels).arg(settings.patchMaxPixels()).arg(high)
        .arg(boxRatio, 0, 'f', 2).arg(settings.patchMaxBoxRatio(), 0, 'f', 2);
}

// Pixels at K in the direction looked for.
QString atKText(const DetectionSettings& settings)
{
    return settings.darkening ? "fell by K or more" : "at or above K";
}

QString decisionText(const DetectionSummary& summary, bool monitoring)
{
    if (summary.change == ChangeKind::LargeRise || summary.change == ChangeKind::LargeFall)
        return QString("Large %1, %2% of the frame at K — %3")
            .arg(summary.change == ChangeKind::LargeRise ? "brightening" : "darkening")
            .arg(100 * summary.share, 0, 'f', 1).arg(monitoring ? "capture ON" : "capture OFF");
    if (summary.firing && summary.change == ChangeKind::Fall)
        return monitoring ? "Darkening candidate — capture ON" : "Would trigger on darkening — capture OFF";
    if (summary.firing) return monitoring ? "Candidate — capture ON" : "Would trigger — capture OFF";
    if (summary.passed) return "Passing patch (1 / 2 frames)";
    if (summary.patchSearchLimited) return "Too many pixels at K to measure individual patches";
    if (!summary.brightPixels) return "Below K";
    QStringList reasons;
    const int pixels = summary.brightPixels;
    const DetectionSettings& settings = summary.settings;
    if (pixels < settings.minPixels)
        reasons << QString("%1 px, needs %2").arg(pixels).arg(settings.minPixels);
    else if (pixels > settings.patchMaxPixels())
        reasons << QString("%1 px, maximum %2").arg(pixels).arg(settings.patchMaxPixels());
    if (boxArea(summary) > settings.patchMaxBoxRatio() * pixels)
        reasons << QString("Box / patch %1 exceeds %2")
            .arg(boxArea(summary) / pixels, 0, 'f', 2).arg(settings.patchMaxBoxRatio(), 0, 'f', 2);
    return reasons.join(" · ");
}

QString stateText(const DetectionSummary& summary, const QString& decision)
{
    return QString("Noise floor %1 ADU (minimum %2). %3 (%4, %5).\n%6   Detector %7 ms/frame\nActive patches %8. Longest %9 s. Background learning outside %10 protected pixels.\n%11")
        .arg(summary.noiseFloor, 0, 'f', 2).arg(summary.settings.minNoiseAdu, 0, 'f', 2)
        .arg(summary.settings.darkening ? "Deepest fall" : "Peak").arg(summary.peakX).arg(summary.peakY)
        .arg(decision).arg(summary.processingMs, 0, 'f', 2).arg(summary.activePatches).arg(summary.longestActiveSeconds, 0, 'f', 1)
        .arg(summary.protectedPixels)
        .arg(summary.settings.oneFrame ? "One passing frame can trigger." : "Two consecutive passing frames required.");
}

QString noiseScaleText(double scale)
{
    return QString("Noise colour scale: 0 to %1 ADU (95th percentile). Display only.").arg(scale, 0, 'f', 2);
}

// The widest values the readouts are expected to show, with both reasons to
// reject a patch, so the readouts can keep room for them.
DetectionSummary widestSummary()
{
    DetectionSummary summary;
    summary.ready = true;
    summary.settings.sigma = 100.0;
    summary.settings.minPixels = 99;
    summary.settings.maxPixels = 9999;
    summary.settings.maxPatchBoxRatio = 9.99;
    summary.settings.minNoiseAdu = 99.99;
    summary.settings.oneFrame = false;
    summary.noiseFloor = 999.99;
    summary.peakSigma = 9999.9;
    summary.peakX = summary.peakY = 9999;
    summary.brightPixels = 99999;
    summary.boxWidth = summary.boxHeight = 9999;
    summary.processingMs = 99.99;
    summary.activePatches = 99;
    summary.longestActiveSeconds = 99.9;
    summary.protectedPixels = 9'999'999;
    return summary;
}

// Feature-45: the same with darkening, whose words and limits differ.
DetectionSummary widestFallSummary()
{
    DetectionSummary summary = widestSummary();
    summary.settings.darkening = true;
    summary.settings.maxDarkPixels = 99999;
    summary.settings.maxDarkBoxRatio = 99.99;
    summary.brightPixels = 999999;
    return summary;
}
}

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
    void markers(double now, double high = 0)
    {
        now_ = now > 0 ? position(now) : -1;
        high_ = high > 0 ? position(high) : -1;
        update();
    }
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QSlider::paintEvent(event);
        QPainter painter(this);
        const auto x = [this](int value) { return 8 + value * (width() - 16) / 1000; };
        const bool nightRed = currentUiTheme() == UiTheme::NightRed;
        if (now_ >= 0)
        {
            painter.setPen(QPen(nightRed ? QColor("#e04444") : QColor("#008b83"), 2));
            painter.drawLine(x(now_), height() / 2 - 6, x(now_), height() / 2 + 6);
        }
        if (high_ >= 0)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(nightRed ? QColor("#ff7777") : QColor("#b37900"));
            painter.drawPolygon(QPolygonF{QPointF(x(high_) - 4, height() - 1),
                QPointF(x(high_) + 4, height() - 1), QPointF(x(high_), height() - 7)});
        }
    }
private:
    double lower_, upper_;
    int now_{-1}, high_{-1};
};

// Plots peak sigma against K over the last ten seconds of source time: the
// deepest fall with darkening (Feature-45).
class PeakHistory : public QWidget
{
public:
    explicit PeakHistory(QWidget* parent) : QWidget(parent) { setMinimumHeight(105); }
    void setView(const DetectionView& view)
    {
        points_ = view.history;
        threshold_ = view.summary.settings.sigma;
        falls_ = view.summary.settings.darkening;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const bool nightRed = currentUiTheme() == UiTheme::NightRed;
        painter.fillRect(rect(), nightRed ? QColor("#0b0202") : QColor("#151c27"));
        const QRectF plot = QRectF(rect()).adjusted(8, 8, -8, -22);
        double high = std::max(threshold_ * 1.3, 1.0);
        for (const auto& p : points_) high = std::max(high, p.peakSigma * 1.05);
        const auto y = [&](double v) { return plot.bottom() - std::log1p(std::max(0.0, v)) / std::log1p(high) * plot.height(); };
        painter.setPen(QPen(nightRed ? QColor("#ff6666") : QColor("#e3b34a"), 1, Qt::DashLine));
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
        painter.setPen(QPen(nightRed ? QColor("#cc3333") : QColor("#65d5bd"), 1));
        painter.drawPolyline(trace);
        painter.setPen(nightRed ? QColor("#d94a4a") : QColor("#c8cfda"));
        painter.drawText(8, height() - 6, QString("%1, last 10 s of stream time (log scale)")
            .arg(falls_ ? "Deepest fall in sigma" : "Peak sigma"));
    }
private:
    std::vector<DetectionHistoryPoint> points_;
    double threshold_{6};
    bool falls_{false};
};

DetectionPanel::DetectionPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("detectionPanel");
    setMinimumWidth(330);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    // Feature-26: above the scrolled settings, so it is always in view. It
    // opens auto-tune's own window (Feature-28), leaving the settings in place.
    auto* autoTuneRow = new QHBoxLayout;
    autoTuneRow->setContentsMargins(6, 4, 6, 2);
    autoTuneRow->setSpacing(6);
    autoTune_ = new QPushButton("Tune camera && detection…", this);
    autoTune_->setObjectName("autoTuneButton");
    autoTune_->setStyleSheet(autoTuneButtonStyle());
    autoTune_->setToolTip("Measures the live Preview to suggest and apply camera settings (exposure, gain, offset) "
        "and detection thresholds (sensitivity K and minimum patch) for what you are watching. Nothing changes until "
        "you apply it.");
    auto* autoTuneHelp = makeHelpIcon(autoTune_, this);
    autoTuneHelp->setObjectName("autoTuneHelp");
    autoTuneRow->addWidget(autoTune_);
    autoTuneRow->addWidget(autoTuneHelp);
    autoTuneRow->addStretch(1);
    outer->addLayout(autoTuneRow);
    connect(autoTune_, &QPushButton::clicked, this, &DetectionPanel::autoTuneRequested);
    timeLapseBanner_ = new QLabel(this);
    timeLapseBanner_->setObjectName("detectionTimeLapseBanner");
    timeLapseBanner_->setWordWrap(true);
    timeLapseBanner_->hide();
    outer->addWidget(timeLapseBanner_);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    content_ = new QWidget;
    auto* content = content_;
    auto* column = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    // Feature-11: what is watched, and the settings only the sky needs.
    auto* targetRow = new QHBoxLayout;
    targetRow->addWidget(new QLabel("Watching", content));
    target_ = new QComboBox(content);
    target_->setObjectName("targetChoice");
    // Feature-31: the Moon's flashes and its shadows are watched apart.
    target_->addItem("The Moon: flashes on the earthlit side", static_cast<int>(Watch::MoonFlashes));
    target_->addItem("The Moon (or Sun): shadows on the sunlit side", static_cast<int>(Watch::MoonShadows));
    // Feature-35: the sky's fast events and its slow, faint ones are watched apart.
    target_->addItem("The sky: fast events (meteors, lightning)", static_cast<int>(Watch::Sky));
    target_->addItem("The sky: slow and faint (long exposures)", static_cast<int>(Watch::SkySlow));
    target_->setToolTip("What is watched. Each keeps its own detection settings and profiles; both Moon watches share "
        "the Moon's capture settings, folder and guiding. Flashes: sensitive, on the earthlit side, the sunlit side "
        "left out. Shadows: planes, the ISS or birds against the sunlit side, insensitive to seeing. Sky, fast events: "
        "short exposures (40 ms at most by default) for meteors, lightning and flares, their timing and trajectory; a "
        "moving meteor leaves no more light in a pixel when exposed longer, only more sky. Sky, slow and faint: "
        "exposures up to 2 s by default for aurora, airglow, slow satellites and long glows, with its own capture "
        "settings and an optional time-lapse. Both sky watches use the sky's folder. Changes only while stopped.");
    targetRow->addWidget(target_, 1);
    column->addLayout(targetRow);

    auto* profileRow = new QHBoxLayout;
    profileRow->addWidget(new QLabel("Profile", content));
    profileCombo_ = new QComboBox(content);
    profileCombo_->setObjectName("detectionProfileCombo");
    profileCombo_->setAccessibleName("Profile");
    profileCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    profileCombo_->setMinimumContentsLength(10);
    profileCombo_->setToolTip("Saved detection profiles for the selected target. In Sky mode, the horizon mask is only changed by profiles saved with a mask.");
    saveProfile_ = new QToolButton(content);
    saveProfile_->setObjectName("detectionSaveProfile");
    saveProfile_->setText("Save…");
    saveProfile_->setToolTip("Save the current detection settings as a named profile.");
    deleteProfile_ = new QToolButton(content);
    deleteProfile_->setObjectName("detectionDeleteProfile");
    deleteProfile_->setText("Delete");
    deleteProfile_->setToolTip("Remove the currently selected saved detection profile.");
    deleteProfile_->setEnabled(false);
    profileRow->addWidget(profileCombo_, 1);
    profileRow->addWidget(saveProfile_);
    profileRow->addWidget(deleteProfile_);
    column->addLayout(profileRow);

    sky_ = new QWidget(content);
    sky_->setObjectName("skySettings");
    auto* skyForm = new QFormLayout(sky_);
    skyForm->setContentsMargins(0, 0, 0, 0);
    brightening_ = new QDoubleSpinBox(sky_);
    brightening_->setObjectName("brighteningShare");
    brightening_->setRange(0, 100); brightening_->setDecimals(1); brightening_->setSingleStep(0.5);
    brightening_->setSuffix(" % of frame"); brightening_->setSpecialValueText("Off");
    brightening_->setToolTip("A frame fires when this share of the unmasked pixels rises by K at once: lightning, a fireball's flare.");
    release_ = new QDoubleSpinBox(sky_);
    release_->setObjectName("releaseSeconds");
    release_->setRange(0, 3600); release_->setDecimals(0); release_->setSuffix(" s"); release_->setSpecialValueText("Never");
    release_->setToolTip("A patch that keeps firing this long is learned into the background, so a star drifting past "
        "a fixed camera stops firing. Aircraft and satellites move on faster and keep firing.");
    skyForm->addRow("Large brightening", brightening_);
    skyForm->addRow("Learn a patch after", release_);
    auto* maskRow = new QHBoxLayout;
    maskLabel_ = new QLabel(sky_);
    maskLabel_->setObjectName("maskSummary");
    drawMask_ = new QToolButton(sky_);
    drawMask_->setObjectName("drawMask");
    drawMask_->setText("Draw mask");
    drawMask_->setCheckable(true);
    drawMask_->setToolTip("While on, shapes drawn on the preview are left out of detection: the ground, trees, roofs, lights. "
        "Drag for a rectangle, or click the corners of a shape and double-click to close it.");
    clearMask_ = new QToolButton(sky_);
    clearMask_->setObjectName("clearMask");
    clearMask_->setToolTip("Removes every shape of the horizon mask: the whole frame is watched again.");
    clearMask_->setText("Clear");
    maskRow->addWidget(maskLabel_, 1);
    maskRow->addWidget(drawMask_);
    maskRow->addWidget(clearMask_);
    skyForm->addRow(maskRow);

    // Feature-15: the exposure follower.
    follow_ = new QCheckBox("Follow the sky's brightness", sky_);
    follow_->setObjectName("followSky");
    // The fixed rules come from the follower itself, so the help cannot disagree with it.
    const SkyExposureRule rule;
    follow_->setToolTip(QString(
        "Automatically adjusts camera exposure (and optionally gain) overnight to keep the sky between %1% and %2% "
        "brightness—bright enough for faint events, dark enough to avoid clipping at dusk or dawn.<br><br>"
        "If the sky stays outside that range for %3 s, it adjusts exposure toward %4% (up to %5× per step) within "
        "your limits, then steps gain; changes are at least %6 s apart. During Monitoring, each change briefly "
        "pauses and restarts capture.")
        .arg(100 * rule.low, 0, 'f', 0).arg(100 * rule.high, 0, 'f', 0).arg(rule.hold.count())
        .arg(100 * rule.aim, 0, 'f', 0).arg(rule.maxFactor, 0, 'g', 3).arg(rule.spacing.count()));
    auto* followRow = new QHBoxLayout;
    followRow->setContentsMargins(0, 0, 0, 0);
    followRow->setSpacing(6);
    auto* followHelp = makeHelpIcon(follow_, sky_);
    followHelp->setObjectName("followSkyHelp");
    followRow->addWidget(follow_);
    followRow->addWidget(followHelp);
    followRow->addStretch(1);
    skyForm->addRow(followRow);

    followControls_ = new QWidget(sky_);
    followControls_->setObjectName("followControls");
    auto* followForm = new QFormLayout(followControls_);
    followForm->setContentsMargins(16, 0, 0, 0);
    minExposure_ = new QDoubleSpinBox(followControls_);
    minExposure_->setObjectName("followMinExposure");
    minExposure_->setToolTip("Shortest exposure the follower may use (for a bright dusk or dawn sky).");
    maxExposure_ = new QDoubleSpinBox(followControls_);
    maxExposure_->setObjectName("followMaxExposure");
    maxExposure_->setToolTip("Longest exposure the follower may use before raising gain. Each sky watch keeps its own limits.");
    for (auto* spin : {minExposure_, maxExposure_}) { spin->setRange(0.01, 60000); spin->setDecimals(2); spin->setSuffix(" ms"); }
    maxGain_ = new QDoubleSpinBox(followControls_);
    maxGain_->setObjectName("followMaxGain");
    maxGain_->setToolTip("Highest gain the follower may use once exposure reaches its longest limit. Set the step to Gain fixed (0) to adjust only exposure.");
    gainStep_ = new QDoubleSpinBox(followControls_);
    gainStep_->setObjectName("followGainStep");
    gainStep_->setToolTip("How much the follower changes gain per step (0 = Gain fixed).");
    for (auto* spin : {maxGain_, gainStep_}) { spin->setRange(0, 10000); spin->setDecimals(0); }
    gainStep_->setSpecialValueText("Gain fixed");
    followReading_ = new QLabel(followControls_);
    followReading_->setObjectName("followReading");
    followReading_->setWordWrap(true);
    followReading_->hide();
    auto* exposureRow = new QHBoxLayout;
    exposureRow->addWidget(minExposure_);
    exposureRow->addWidget(new QLabel("to", followControls_));
    exposureRow->addWidget(maxExposure_);
    // Each box is as wide as its widest value; the rest of the row stays empty.
    exposureRow->addStretch(1);
    followForm->addRow("Exposure", exposureRow);
    auto* gainRow = new QHBoxLayout;
    gainRow->addWidget(maxGain_);
    gainRow->addWidget(new QLabel("in steps of", followControls_));
    gainRow->addWidget(gainStep_);
    gainRow->addStretch(1);
    followForm->addRow("Gain up to", gainRow);
    followForm->addRow(followReading_);
    followControls_->hide();
    skyForm->addRow(followControls_);
    // Feature-36: the slow sky's time-lapse.
    timelapseRow_ = new QWidget(sky_);
    auto* timelapseLayout = new QHBoxLayout(timelapseRow_);
    timelapseLayout->setContentsMargins(0, 0, 0, 0);
    timelapse_ = new QCheckBox("Time-lapse while monitoring, every", timelapseRow_);
    timelapse_->setObjectName("watchTimelapse");
    timelapseInterval_ = new QSpinBox(timelapseRow_);
    timelapseInterval_->setObjectName("watchTimelapseInterval");
    timelapseInterval_->setRange(1, 3600);
    timelapseInterval_->setValue(30);
    timelapseInterval_->setSuffix(" s");
    const QString timelapseHelp = "Alongside detection, keeps one frame every this many seconds of source time while "
        "Monitoring, in the watch's timelapse folder: each as a PNG with the camera's samples unchanged (16-bit for "
        "RAW16), with its time in frames.txt, and in timelapse.mp4 (25 frames a second, stretched for display like the "
        "preview's auto-stretch). The video is written as the night goes, so it is ready when Monitoring stops. 30 s "
        "keeps about 1 GB of PNG per hour for a 2-megapixel 16-bit camera. Set before Monitoring starts.";
    timelapse_->setToolTip(timelapseHelp);
    timelapseInterval_->setToolTip(timelapseHelp);
    timelapseLayout->addWidget(timelapse_);
    timelapseLayout->addWidget(timelapseInterval_);
    timelapseLayout->addStretch(1);
    skyForm->addRow(timelapseRow_);
    timelapseRow_->hide();
    connect(timelapse_, &QCheckBox::toggled, this, [this] { if (!updating_) emit watchTimelapseChanged(); });
    connect(timelapseInterval_, &QSpinBox::valueChanged, this, [this] { if (!updating_) emit watchTimelapseChanged(); });
    column->addWidget(sky_);
    sky_->hide();

    auto* grid = new QWidget(content);
    auto* layout = new QGridLayout(grid);
    layout->setContentsMargins(0, 0, 0, 0);
    column->addWidget(grid);
    column->addStretch(1);
    layout->setColumnStretch(0, 1);
    // Feature-45: the help of the controls for the change looked for is set
    // with the watch, in showWatchedChange().
    sigma_ = new QDoubleSpinBox(this);
    sigma_->setObjectName("detectionSigma");
    sigma_->setRange(0.1, 1000); sigma_->setDecimals(1); sigma_->setSingleStep(0.1); sigma_->setSuffix(" sigma");
    minimum_ = new QSpinBox(this); minimum_->setObjectName("detectionMinPixels");
    maximum_ = new QSpinBox(this); maximum_->setObjectName("detectionMaxPixels");
    for (auto* spin : {minimum_, maximum_}) { spin->setRange(1, 1'000'000); spin->setSuffix(" px"); }
    sigmaSlider_ = new MeterSlider(0.1, 1000, this); sigmaSlider_->setObjectName("sigmaSlider");
    minSlider_ = new MeterSlider(1, 1'000'000, this); minSlider_->setObjectName("minimumSlider");
    minSlider_->setToolTip("Minimum patch. The marks show the current and recent largest patch.");
    maxSlider_ = new MeterSlider(1, 1'000'000, this); maxSlider_->setObjectName("maximumSlider");
    maxSlider_->setToolTip("Maximum patch. The marks show the current and recent largest patch.");
    compactness_ = new QDoubleSpinBox(this);
    compactness_->setObjectName("detectionMaxBoxRatio");
    compactness_->setRange(1.0, 1000.0);
    compactness_->setDecimals(2);
    compactness_->setSingleStep(0.1);
    compactness_->setSuffix(" ×");
    compactnessSlider_ = new MeterSlider(compactness_->minimum(), compactness_->maximum(), this);
    compactnessSlider_->setObjectName("compactnessSlider");
    // The readouts keep room for their longest texts, so the controls below
    // them stay where they are while the texts change.
    const DetectionSummary widest = widestSummary();
    const DetectionSummary widestFall = widestFallSummary();
    sigmaReading_ = new ReadoutLabel({peakText(widest.peakSigma, widest.settings.sigma, widest.peakSigma, false),
        peakText(widest.peakSigma, widest.settings.sigma, widest.peakSigma, true), kSettlingReading}, this);
    sigmaReading_->setObjectName("sigmaReading");
    sigmaReading_->setText("Waiting for measurements");
    // A measured patch is at most a few thousand pixels (a shadow some tens
    // of thousands); the recent high counts pixels at K across the frame,
    // which can be most of it.
    sizeReading_ = new ReadoutLabel({patchText(widest.settings.maxPixels, 9'999'999, 99.99, widest.settings),
        patchText(widestFall.settings.maxDarkPixels, 9'999'999, 99.99, widestFall.settings)}, this);
    sizeReading_->setObjectName("patchReading");
    sizeReading_->setText("Patch: --");
    // A label naming its control, so the control's help shows on it (Feature-24).
    const auto named = [this](const QString& text, QWidget* control) {
        auto* label = new QLabel(text, this);
        label->setBuddy(control);
        return label;
    };
    layout->addWidget(named("Sensitivity K", sigma_), 0, 0); layout->addWidget(sigma_, 0, 1);
    layout->addWidget(sigmaSlider_, 1, 0, 1, 2);
    layout->addWidget(sigmaReading_, 2, 0, 1, 2);
    layout->addWidget(named("Minimum patch", minimum_), 3, 0); layout->addWidget(minimum_, 3, 1);
    layout->addWidget(minSlider_, 4, 0, 1, 2);
    layout->addWidget(named("Maximum patch", maximum_), 5, 0); layout->addWidget(maximum_, 5, 1);
    layout->addWidget(maxSlider_, 6, 0, 1, 2);
    layout->addWidget(named("Maximum box / patch", compactness_), 7, 0);
    layout->addWidget(compactness_, 7, 1);
    layout->addWidget(compactnessSlider_, 8, 0, 1, 2);
    layout->addWidget(sizeReading_, 9, 0, 1, 2);
    oneFrame_ = new QCheckBox("Allow a one-frame trigger", this); oneFrame_->setObjectName("oneFrameTrigger");
    layout->addWidget(oneFrame_, 10, 0, 1, 2);
    history_ = new PeakHistory(this); history_->setObjectName("peakHistory");
    layout->addWidget(history_, 11, 0, 1, 2);
    state_ = new ReadoutLabel({stateText(widest, decisionText(widest, false)),
        stateText(widestFall, decisionText(widestFall, false)), kSettlingState}, this);
    state_->setObjectName("detectionState");
    state_->setText("Start a source to measure its noise.");
    layout->addWidget(state_, 12, 0, 1, 2);
    overlay_ = new QComboBox(this); overlay_->setObjectName("detectionOverlay");
    overlay_->addItems({"None", "Sigma map", "Noise map"});
    layout->addWidget(named("Preview overlay", overlay_), 13, 0);
    layout->addWidget(overlay_, 13, 1);
    mapScale_ = new ReadoutLabel({noiseScaleText(9999.99), kOverlaySkipped}, this);
    mapScale_->setText("Overlays affect the display only.");
    layout->addWidget(mapScale_, 14, 0, 1, 2);

    auto* showBackground = new QToolButton(content);
    showBackground->setObjectName("backgroundSettings");
    showBackground->setToolTip("Shows the settings for how the background and its noise are learned.");
    showBackground->setText("Background and noise");
    showBackground->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    showBackground->setArrowType(Qt::RightArrow);
    showBackground->setCheckable(true);
    layout->addWidget(showBackground, 15, 0, 1, 2);
    auto* background = new QWidget(content);
    auto* model = new QFormLayout(background);
    model->setContentsMargins(0, 0, 0, 0);
    referenceTime_ = new QDoubleSpinBox(background);
    referenceTime_->setObjectName("detectionReferenceTime");
    referenceTime_->setRange(0.05, 300.0);
    referenceTime_->setSuffix(" s");
    referenceTime_->setSingleStep(0.1);
    referenceTime_->setToolTip("Time constant for background learning. Lower values follow brightness changes faster.");
    referenceSlider_ = new MeterSlider(referenceTime_->minimum(), referenceTime_->maximum(), background);
    referenceSlider_->setObjectName("referenceTimeSlider");
    referenceSlider_->setToolTip("Reference time: how quickly the background learns changes in brightness.");
    minimumNoise_ = new QDoubleSpinBox(background);
    minimumNoise_->setObjectName("detectionMinNoise");
    minimumNoise_->setRange(0.01, 65535.0);
    minimumNoise_->setSuffix(" ADU");
    minimumNoise_->setSingleStep(0.1);
    minimumNoise_->setToolTip("Lower bound for estimated noise in native sample units.");
    noiseSlider_ = new MeterSlider(minimumNoise_->minimum(), minimumNoise_->maximum(), background);
    noiseSlider_->setObjectName("minimumNoiseSlider");
    noiseSlider_->setToolTip("Minimum noise: the lowest noise a pixel is assumed to have.");
    model->addRow("Reference time", referenceTime_);
    model->addRow(referenceSlider_);
    model->addRow("Minimum noise", minimumNoise_);
    model->addRow(noiseSlider_);
    layout->addWidget(background, 16, 0, 1, 2);

    // Feature-31: the flashes' ceiling. The sunlit side cannot show a flash.
    flashControls_ = new QWidget(content);
    auto* flashForm = new QFormLayout(flashControls_);
    flashForm->setContentsMargins(0, 0, 0, 0);
    ignoreAbove_ = new QDoubleSpinBox(flashControls_);
    ignoreAbove_->setObjectName("detectionIgnoreAbove");
    ignoreAbove_->setRange(0, 100); ignoreAbove_->setDecimals(0); ignoreAbove_->setSingleStep(5);
    ignoreAbove_->setSuffix(" % of full scale"); ignoreAbove_->setSpecialValueText("Off");
    ignoreAbove_->setToolTip("Pixels whose learned brightness is above this are not tested: the sunlit side and the "
        "glare by the terminator, where no flash can be seen and seeing only makes false triggers. It follows the "
        "Moon's drift. Off tests every pixel.");
    flashForm->addRow("Ignore brighter than", ignoreAbove_);
    layout->addWidget(flashControls_, 17, 0, 1, 2);
    // Feature-31: the shadows' large darkening. Their patch limits are the
    // Maximum patch and Maximum box / patch above (Feature-45).
    auto* dark = new QWidget(content);
    darkControls_ = dark;
    auto* darkForm = new QFormLayout(dark);
    darkForm->setContentsMargins(0, 0, 0, 0);
    darkShare_ = new QDoubleSpinBox(dark);
    darkShare_->setObjectName("darkeningShare");
    darkShare_->setRange(0, 100); darkShare_->setDecimals(1); darkShare_->setSingleStep(0.5);
    darkShare_->setSuffix(" % of frame"); darkShare_->setSpecialValueText("Off");
    darkShare_->setToolTip("A frame fires when this share of the unmasked pixels falls by K at once: a plane or bird "
        "filling the field. A cloud edge does the same.");
    darkForm->addRow("Large darkening", darkShare_);
    layout->addWidget(dark, 18, 0, 1, 2);
    layout->setRowStretch(19, 1);
    background->hide();
    connect(showBackground, &QToolButton::toggled, this, [showBackground, background](bool shown) {
        showBackground->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
        background->setVisible(shown);
    });

    connect(sigma_, &QDoubleSpinBox::valueChanged, this, [this] { controlsChanged(); });
    connect(minimum_, &QSpinBox::valueChanged, this, [this] { controlsChanged(); });
    connect(maximum_, &QSpinBox::valueChanged, this, [this] { controlsChanged(true); });
    for (auto* spin : {compactness_, referenceTime_, minimumNoise_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { controlsChanged(); });
    connect(oneFrame_, &QCheckBox::toggled, this, [this] { controlsChanged(); });
    connect(sigmaSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) sigma_->setValue(sigmaSlider_->number(p)); });
    connect(minSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) minimum_->setValue(static_cast<int>(std::round(minSlider_->number(p)))); });
    connect(maxSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) maximum_->setValue(static_cast<int>(std::round(maxSlider_->number(p)))); });
    connect(compactnessSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) compactness_->setValue(compactnessSlider_->number(p)); });
    connect(referenceSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) referenceTime_->setValue(referenceSlider_->number(p)); });
    connect(noiseSlider_, &QSlider::valueChanged, this, [this](int p) { if (!updating_) minimumNoise_->setValue(noiseSlider_->number(p)); });
    connect(overlay_, &QComboBox::currentIndexChanged, this, [this] { emit displayChanged(); });
    connect(target_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!updating_) emit watchRequested(static_cast<Watch>(target_->itemData(index).toInt()));
    });
    for (auto* spin : {brightening_, release_, darkShare_, ignoreAbove_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { controlsChanged(); });
    connect(drawMask_, &QToolButton::toggled, this, [this](bool on) { emit maskDrawingChanged(on); });
    connect(follow_, &QCheckBox::toggled, this, [this](bool on) {
        followControls_->setVisible(on);
        if (!updating_) emit skyExtrasChanged();
    });
    for (auto* spin : {minExposure_, maxExposure_, maxGain_, gainStep_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) emit skyExtrasChanged(); });
    connect(clearMask_, &QToolButton::clicked, this, [this] {
        mask_ = {};
        showMask();
        emit settingsChanged();
    });
    connect(profileCombo_, &QComboBox::currentIndexChanged, this, &DetectionPanel::onProfileComboChanged);
    connect(saveProfile_, &QToolButton::clicked, this, &DetectionPanel::onSaveProfileClicked);
    connect(deleteProfile_, &QToolButton::clicked, this, &DetectionPanel::onDeleteProfileClicked);
    reloadProfiles();
    showWatchedChange();
    setSettings({});
}

// Feature-45: the controls at the top are for the change the watch looks
// for, a rise or, in the shadows' watch, a fall. Their help says which.
void DetectionPanel::showWatchedChange()
{
    const bool falls = looksForFalls(watch_);
    sigma_->setToolTip(falls
        ? "Sensitivity K: a pixel fires when it falls K times its own noise below the learned background. On sunlit "
          "terrain K 12 or more keeps seeing from triggering. Lower is more sensitive and gives more false triggers."
        : "Sensitivity K: a pixel fires when it rises K times its own noise above the learned background. Lower is more "
          "sensitive and gives more false triggers.");
    sigmaSlider_->setToolTip(QString("Sensitivity K. The teal mark is the frame's %1 now, the amber mark the %2 of the "
        "last seconds: a mark beyond K would trigger.").arg(falls ? "deepest fall" : "highest peak", falls ? "deepest" : "highest"));
    minimum_->setToolTip(falls
        ? "The fewest connected darkened pixels that count as a shadow. Raise it to ignore noise and small seeing ripples."
        : "The fewest connected firing pixels that count as an event. Raise it to ignore single hot pixels and noise.");
    maximum_->setToolTip(falls
        ? "The most connected darkened pixels a shadow may have: a plane or the ISS covers many. Larger patches (a cloud "
          "edge) are ignored."
        : "The most connected firing pixels an event may have. Larger patches (a seeing ripple across a crater rim, a "
          "cloud) are ignored.");
    minSlider_->setToolTip(QString("Minimum patch. The marks show the current and recent largest %1.").arg(falls ? "shadow" : "patch"));
    maxSlider_->setToolTip(QString("Maximum patch. The marks show the current and recent largest %1.").arg(falls ? "shadow" : "patch"));
    compactness_->setToolTip(QString("Largest allowed bounding-box area divided by the number of pixels in the %1.")
        .arg(falls ? "shadow" : "patch"));
    compactnessSlider_->setToolTip(falls
        ? "Maximum box / patch: how spread out a shadow may be. A plane's or the ISS's silhouette is near 1-2; a seeing "
          "ripple along a crater rim is much higher."
        : "Maximum box / patch: how spread out a patch may be. A compact flash is near 1-2; a long thin edge is much higher.");
    oneFrame_->setToolTip(QString("Off requires two consecutive passing frames. %1 can then be missed.")
        .arg(falls ? "A shadow seen on one frame only" : "A single-frame flash"));
    overlay_->setToolTip(QString("Draws a map on the preview, for the screen only: none, the sigma map (pixels that %1 "
        "in red), or the noise map.").arg(falls ? "fell by K or more" : "are at or above K"));
}

DetectionSettings DetectionPanel::settings() const
{
    // shown_ holds what has no control here: the limits of the change the
    // watch does not look for keep the values last set (Feature-45).
    DetectionSettings values = shown_;
    values.sigma = sigma_->value();
    values.minPixels = minimum_->value();
    values.oneFrame = oneFrame_->isChecked();
    values.referenceTimeConstantSeconds = referenceTime_->value();
    values.minNoiseAdu = minimumNoise_->value();
    // Feature-45: Maximum patch and Maximum box / patch are the limits of
    // the change looked for.
    if (looksForFalls(watch_))
    {
        values.maxDarkPixels = maximum_->value();
        values.maxDarkBoxRatio = compactness_->value();
    }
    else
    {
        values.maxPixels = maximum_->value();
        values.maxPatchBoxRatio = compactness_->value();
    }
    // Feature-31: darkening is the shadows' watch, the ceiling the flashes'.
    values.darkeningShare = darkShare_->value() / 100;
    values.ignoreAboveShare = ignoreAbove_->value() / 100;
    values.brighteningShare = brightening_->value() / 100;
    values.releaseSeconds = release_->value();
    values.mask = mask_;
    return settingsForWatch(values, watch_);
}

Target DetectionPanel::target() const
{
    return watchTarget(watch_);
}

Watch DetectionPanel::watch() const
{
    return watch_;
}

QString DetectionPanel::watchTitle() const
{
    const int index = target_->findData(static_cast<int>(watch_));
    return index >= 0 ? target_->itemText(index) : QString();
}

void DetectionPanel::setWatch(Watch watch)
{
    const DetectionSettings current = settings();
    watch_ = watch;
    const bool updating = updating_;
    updating_ = true;
    target_->setCurrentIndex(std::max(0, target_->findData(static_cast<int>(watch))));
    updating_ = updating;
    sky_->setVisible(watchTarget(watch) == Target::Sky);
    timelapseRow_->setVisible(watch == Watch::SkySlow);
    flashControls_->setVisible(watch == Watch::MoonFlashes);
    darkControls_->setVisible(watch == Watch::MoonShadows);
    if (watchTarget(watch) != Target::Sky) drawMask_->setChecked(false);
    // Feature-45: until the window sets the watch's own settings, the
    // controls show these ones' limits for the change this watch looks for.
    showWatchedChange();
    setSettings(current);
    reloadProfiles();
}

void DetectionPanel::setTargetEditable(bool editable)
{
    target_->setEnabled(editable);
}

bool DetectionPanel::addMaskShape(const MaskShape& shape)
{
    if (target() != Target::Sky) return false;
    const DetectionMask grown = mask_.with(shape);
    if (grown.count() == mask_.count()) return false;
    mask_ = grown;
    showMask();
    emit settingsChanged();
    return true;
}

bool DetectionPanel::addMaskRect(QRect rect)
{
    return !rect.isEmpty() && addMaskShape(DetectionMask::rectangle(rect.x(), rect.y(), rect.width(), rect.height()));
}

bool DetectionPanel::drawingMask() const
{
    return drawMask_->isChecked();
}

void DetectionPanel::setDrawingMask(bool on)
{
    const QSignalBlocker blocked(drawMask_);
    drawMask_->setChecked(on);
}

SkyExtras DetectionPanel::skyExtras() const
{
    return {follow_->isChecked(), minExposure_->value(), maxExposure_->value(), maxGain_->value(), gainStep_->value()};
}

void DetectionPanel::setSkyExtras(const SkyExtras& extras)
{
    const bool updating = updating_;
    updating_ = true;
    follow_->setChecked(extras.follow);
    followControls_->setVisible(extras.follow);
    minExposure_->setValue(extras.minExposureMs);
    maxExposure_->setValue(extras.maxExposureMs);
    maxGain_->setValue(extras.maxGain);
    gainStep_->setValue(extras.gainStep);
    updating_ = updating;
}


WatchTimelapse DetectionPanel::watchTimelapse() const
{
    return {timelapse_->isChecked(), timelapseInterval_->value()};
}

void DetectionPanel::setWatchTimelapse(const WatchTimelapse& timelapse)
{
    const bool updating = updating_;
    updating_ = true;
    timelapse_->setChecked(timelapse.on);
    timelapseInterval_->setValue(timelapse.intervalSeconds);
    updating_ = updating;
}

void DetectionPanel::setWatchTimelapseEditable(bool editable)
{
    timelapse_->setEnabled(editable);
    timelapseInterval_->setEnabled(editable);
}

void DetectionPanel::showFollow(const QString& text)
{
    followReading_->setText(text);
    followReading_->setVisible(!text.isEmpty());
}

void DetectionPanel::showMask()
{
    maskLabel_->setText(mask_.empty() ? QString("No mask")
        : QString("Mask: %1 of %2 shapes").arg(mask_.count()).arg(DetectionMask::kMaxShapes));
    clearMask_->setEnabled(!mask_.empty());
}

void DetectionPanel::setSettings(const DetectionSettings& given)
{
    const DetectionSettings settings = settingsForWatch(given, watch_);
    if (!settings.valid()) return;
    shown_ = settings;
    updating_ = true;
    brightening_->setValue(100 * settings.brighteningShare);
    release_->setValue(settings.releaseSeconds);
    mask_ = settings.mask;
    showMask();
    ignoreAbove_->setValue(100 * settings.ignoreAboveShare);
    darkShare_->setValue(100 * settings.darkeningShare);
    // Feature-45: the maximum patch and box are those of the change looked for.
    sigma_->setValue(settings.sigma); minimum_->setValue(settings.minPixels); maximum_->setValue(settings.patchMaxPixels());
    oneFrame_->setChecked(settings.oneFrame);
    compactness_->setValue(settings.patchMaxBoxRatio());
    referenceTime_->setValue(settings.referenceTimeConstantSeconds);
    minimumNoise_->setValue(settings.minNoiseAdu);
    sigmaSlider_->setValue(sigmaSlider_->position(sigma_->value()));
    minSlider_->setValue(minSlider_->position(minimum_->value()));
    maxSlider_->setValue(maxSlider_->position(maximum_->value()));
    compactnessSlider_->setValue(compactnessSlider_->position(compactness_->value()));
    referenceSlider_->setValue(referenceSlider_->position(referenceTime_->value()));
    noiseSlider_->setValue(noiseSlider_->position(minimumNoise_->value()));
    updating_ = false;
}

void DetectionPanel::controlsChanged(bool maximumChanged)
{
    if (updating_) return;
    // The minimum and maximum patch shown are the limits of the change the
    // watch looks for (Feature-45); the one not just changed follows.
    if (minimum_->value() > maximum_->value())
    {
        const QSignalBlocker blockMinimum(minimum_), blockMaximum(maximum_);
        if (maximumChanged) minimum_->setValue(maximum_->value());
        else maximum_->setValue(minimum_->value());
    }
    setSettings(settings());
    if (profileCombo_->currentIndex() != 0)
    {
        const QSignalBlocker blocked(profileCombo_);
        profileCombo_->setCurrentIndex(0);
        deleteProfile_->setEnabled(false);
    }
    emit settingsChanged();
}

void DetectionPanel::reloadProfiles()
{
    const QString selectedName = profileCombo_->currentData().toString();
    const QSignalBlocker blocked(profileCombo_);
    profileCombo_->clear();
    profileCombo_->addItem("Custom / current", QString());
    const auto profiles = loadSavedDetectionProfiles(watch_);
    for (const SavedDetectionProfile& p : profiles)
    {
        const QString label = p.hasMask ? QString("%1 (+mask)").arg(p.name) : p.name;
        profileCombo_->addItem(label, p.name);
    }
    const int idx = selectedName.isEmpty() ? 0 : profileCombo_->findData(selectedName);
    profileCombo_->setCurrentIndex(std::max(0, idx));
    deleteProfile_->setEnabled(profileCombo_->currentIndex() > 0);
}

bool DetectionPanel::saveCurrentProfile(const QString& name, bool includeMask)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    SavedDetectionProfile profile;
    profile.name = trimmed;
    profile.watch = watch_;
    profile.settings = settings();
    profile.hasMask = (watchTarget(watch_) == Target::Sky) && includeMask;
    if (profile.hasMask)
        profile.mask = mask_;
    else
        profile.settings.mask = {};
    saveNamedDetectionProfile(profile);
    reloadProfiles();
    const int idx = profileCombo_->findData(trimmed);
    if (idx > 0)
    {
        const QSignalBlocker blocked(profileCombo_);
        profileCombo_->setCurrentIndex(idx);
        deleteProfile_->setEnabled(true);
    }
    return true;
}

bool DetectionPanel::applyProfile(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    const auto profiles = loadSavedDetectionProfiles(watch_);
    for (const SavedDetectionProfile& p : profiles)
    {
        if (p.name.compare(trimmed, Qt::CaseInsensitive) != 0) continue;
        DetectionSettings next = p.settings;
        if (watchTarget(watch_) == Target::Sky)
            next.mask = p.hasMask ? p.mask : mask_;
        else
            next.mask = {};
        setSettings(next);
        const int idx = profileCombo_->findData(p.name);
        if (idx > 0)
        {
            const QSignalBlocker blocked(profileCombo_);
            profileCombo_->setCurrentIndex(idx);
            deleteProfile_->setEnabled(true);
        }
        emit settingsChanged();
        return true;
    }
    return false;
}

bool DetectionPanel::deleteProfile(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return false;
    removeNamedDetectionProfile(watch_, trimmed);
    reloadProfiles();
    return true;
}

void DetectionPanel::onProfileComboChanged(int index)
{
    deleteProfile_->setEnabled(index > 0);
    if (index <= 0) return;
    const QString name = profileCombo_->itemData(index).toString();
    applyProfile(name);
}

void DetectionPanel::onSaveProfileClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Save detection profile");
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* nameEdit = new QLineEdit(&dialog);
    nameEdit->setObjectName("detectionProfileNameEdit");
    nameEdit->setPlaceholderText(watchTarget(watch_) == Target::Sky
        ? "e.g. Backyard meteors, Dark site sky"
        : "e.g. Moon dark side, Lunar transit");
    if (profileCombo_->currentIndex() > 0)
        nameEdit->setText(profileCombo_->currentData().toString());
    form->addRow("Profile name", nameEdit);
    layout->addLayout(form);

    QCheckBox* includeMaskBox = nullptr;
    if (watchTarget(watch_) == Target::Sky)
    {
        includeMaskBox = new QCheckBox(
            mask_.empty()
                ? QString("Include horizon mask (currently empty)")
                : QString("Include horizon mask (%1 shape(s))").arg(mask_.count()),
            &dialog);
        includeMaskBox->setObjectName("detectionProfileIncludeMask");
        includeMaskBox->setChecked(false);
        includeMaskBox->setToolTip("Off (default): loading this profile keeps whatever horizon mask you already have. "
            "On: loading this profile replaces the horizon mask with the saved one.");
        layout->addWidget(includeMaskBox);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    nameEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;

    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty())
    {
        QMessageBox::warning(this, "Save detection profile", "Enter a name for the detection profile.");
        return;
    }
    saveCurrentProfile(name, includeMaskBox && includeMaskBox->isChecked());
}

void DetectionPanel::onDeleteProfileClicked()
{
    if (profileCombo_->currentIndex() <= 0) return;
    const QString name = profileCombo_->currentData().toString();
    deleteProfile(name);
}

DetectionOverlay DetectionPanel::overlay() const { return static_cast<DetectionOverlay>(overlay_->currentIndex()); }
QImage DetectionPanel::overlayImage() const { return overlayImage_; }

void DetectionPanel::setView(const DetectionView& view)
{
    const auto& summary = view.summary;
    latest_ = summary;
    settling_ = view.settling;
    const bool measuring = summary.ready && !view.settling;
    const bool hasPatch = measuring && summary.brightPixels > 0 && !summary.patchSearchLimited;
    double high = 0;
    int big = 0;
    for (const auto& point : view.history) { high = std::max(high, point.peakSigma); big = std::max(big, point.brightPixels); }
    const int patchPixels = hasPatch ? summary.brightPixels : 0;
    const double compactness = hasPatch ? boxArea(summary) / patchPixels : 0;
    sigmaSlider_->markers(measuring ? summary.peakSigma : 0, measuring ? high : 0);
    minSlider_->markers(patchPixels, measuring ? big : 0);
    maxSlider_->markers(patchPixels, measuring ? big : 0);
    compactnessSlider_->markers(compactness);
    noiseSlider_->markers(measuring ? summary.noiseFloor : 0);
    if (measuring)
    {
        // Feature-45: with darkening every measurement is of falls.
        sigmaReading_->setText(peakText(summary.peakSigma, summary.settings.sigma, high, summary.settings.darkening));
        if (hasPatch)
            sizeReading_->setText(patchText(patchPixels, big, compactness, summary.settings));
        else if (summary.patchSearchLimited)
            sizeReading_->setText(QString("%1 px %2 across the frame.").arg(summary.brightPixels).arg(atKText(summary.settings)));
        else
            sizeReading_->setText("Patch size and shape appear when pixels reach K.");
    }
    else
    {
        sigmaReading_->setText(view.settling ? kSettlingReading : "Learning the reference — waiting for measurements.");
        const DetectionSettings shown = settings();
        sizeReading_->setText(QString("Allowed patch size %1–%2 px.").arg(shown.minPixels).arg(shown.patchMaxPixels()));
    }
    updateDecision();
    history_->setView(view);
    overlayImage_ = {};
    if (!view.map || overlay() == DetectionOverlay::None)
    {
        mapScale_->setText(summary.overlaySkipped ? kOverlaySkipped : "Overlays affect the display only.");
        return;
    }
    const auto& map = *view.map;
    if (map.kind != overlay() || map.values.empty()) return;
    double scale = summary.settings.sigma;
    if (map.kind == DetectionOverlay::Noise)
    {
        BudgetVector<float> sorted{BudgetAllocator<float>(map.values.get_allocator().budget)};
        const size_t step = std::max(size_t{1}, (map.values.size() + 4095) / 4096);
        for (size_t i = 0; i < map.values.size(); i += step) sorted.push_back(map.values[i]);
        auto at = sorted.begin() + static_cast<ptrdiff_t>(sorted.size() * 95 / 100);
        std::nth_element(sorted.begin(), at, sorted.end());
        scale = std::max(0.5, static_cast<double>(*at));
    }
    overlayImage_ = makeBudgetImage(map.width, map.height, QImage::Format_ARGB32, map.values.get_allocator().budget);
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
        ? QString("Red: %1 (K %2 sigma). Display only.").arg(atKText(summary.settings)).arg(scale, 0, 'f', 1)
        : noiseScaleText(scale));
}

void DetectionPanel::setMonitoring(bool monitoring)
{
    if (monitoring_==monitoring) return;
    monitoring_=monitoring;
    updateDecision();
}

void DetectionPanel::setTimeLapseState(bool active, bool watchingTransits, const QString& statusDetail)
{
    timeLapseActive_ = active;
    timeLapseTransits_ = active && watchingTransits;
    timeLapseDetail_ = active ? statusDetail : QString();
    autoTune_->setEnabled(!active);
    content_->setEnabled(!active);
    if (!active)
    {
        timeLapseBanner_->hide();
    }
    else if (watchingTransits)
    {
        timeLapseBanner_->setStyleSheet(timeLapseBannerStyle(true));
        QString text = "Active monitoring for shadows / transits between time-lapse bursts";
        if (!statusDetail.isEmpty()) text += " (" + statusDetail + ")";
        text += ". Settings are locked during time-lapse.";
        timeLapseBanner_->setText(text);
        timeLapseBanner_->show();
    }
    else
    {
        timeLapseBanner_->setStyleSheet(timeLapseBannerStyle(false));
        timeLapseBanner_->setText("Time-lapse running — detection is OFF. Settings are locked during time-lapse.");
        timeLapseBanner_->show();
    }
    updateDecision();
}

void DetectionPanel::refreshTheme()
{
    if (autoTune_) autoTune_->setStyleSheet(autoTuneButtonStyle());
    if (timeLapseActive_ && timeLapseBanner_)
        timeLapseBanner_->setStyleSheet(timeLapseBannerStyle(timeLapseTransits_));
    update();
}

void DetectionPanel::updateDecision()
{
    if (timeLapseActive_)
    {
        if (timeLapseTransits_)
            state_->setText(timeLapseDetail_.isEmpty()
                ? QString("Active monitoring for shadows / transits between bursts.")
                : QString("Active shadow / transit monitoring — %1.").arg(timeLapseDetail_));
        else
            state_->setText("Time-lapse running — detection OFF.");
        return;
    }
    const auto& summary=latest_;
    if (settling_)
        state_->setText(kSettlingState);
    else if (!summary.ready)
        state_->setText(QString("Learning reference (%1 frame(s)). Capture %2.").arg(summary.learningFrames).arg(monitoring_?"ON":"OFF"));
    else
        state_->setText(stateText(summary, decisionText(summary, monitoring_)));
}
