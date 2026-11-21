#include "auto_tune_panel.h"

#include "recording/metadata_text.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QLabel* wrapped(QWidget* parent, const QString& objectName = {})
{
    auto* label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    if (!objectName.isEmpty()) label->setObjectName(objectName);
    return label;
}

QString aimText(TuneWorkflow workflow)
{
    switch (workflow)
    {
    case TuneWorkflow::MoonFlashes:
        return "Watching the Moon for impact flashes: the earthshine should be at 3-10 % of full scale, with the exposure "
               "between 10 and 40 ms. Draw a box on the earthshine first.";
    case TuneWorkflow::MoonTransits:
        return "Watching the Moon for transits and shadows: the brightest sunlit areas should be at 60-80 % of full "
               "scale with nothing saturated, and the exposure at most 10 ms.";
    case TuneWorkflow::Sky:
        return "Watching the sky: the sky outside the mask should be at 8-30 % of full scale, the band the exposure "
               "follower keeps. Auto-tune is best run in full darkness.";
    }
    return {};
}

} // namespace

AutoTunePanel::AutoTunePanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("autoTunePanel");
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);

    aim_ = wrapped(this, "autoTuneAim");
    column->addWidget(aim_);
    twilight_ = wrapped(this, "autoTuneTwilight");
    twilight_->setStyleSheet("color: #b37900;");
    twilight_->hide();
    column->addWidget(twilight_);

    // Moon flashes: the observer's box on the earthshine.
    boxRow_ = new QWidget(this);
    auto* boxLayout = new QVBoxLayout(boxRow_);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    auto* boxButtons = new QHBoxLayout;
    drawBox_ = new QToolButton(boxRow_);
    drawBox_->setObjectName("autoTuneDrawBox");
    drawBox_->setText("Draw earthshine box");
    drawBox_->setCheckable(true);
    drawBox_->setToolTip("While on, drag a box on the preview over the earthshine, the part of the Moon where flashes are "
        "searched. Auto-tune grows it to the whole earthshine area and outlines that on the preview.");
    boxOnly_ = new QCheckBox("Measure only my box", boxRow_);
    boxOnly_->setObjectName("autoTuneBoxOnly");
    boxOnly_->setToolTip("Measure the box you drew and nothing more, when the outlined area is not the earthshine "
        "(for example a mare taken for the sky beyond the Moon's edge).");
    boxButtons->addWidget(drawBox_);
    boxButtons->addWidget(boxOnly_, 1);
    boxLayout->addLayout(boxButtons);
    area_ = wrapped(boxRow_, "autoTuneArea");
    boxLayout->addWidget(area_);
    column->addWidget(boxRow_);
    connect(drawBox_, &QToolButton::toggled, this, &AutoTunePanel::drawBoxToggled);
    connect(boxOnly_, &QCheckBox::toggled, this, &AutoTunePanel::boxOnlyToggled);

    // Step 1: the camera.
    auto* cameraBox = new QGroupBox("1. Camera", this);
    auto* cameraColumn = new QVBoxLayout(cameraBox);
    measureCamera_ = new QPushButton("Measure camera", cameraBox);
    measureCamera_->setObjectName("autoTuneMeasureCamera");
    measureCamera_->setToolTip("Measures the brightness of the measured area for two seconds and suggests exposure and "
        "gain for the aim above. Nothing changes until you press Apply.");
    cameraColumn->addWidget(measureCamera_);
    skyMaxRow_ = new QWidget(cameraBox);
    auto* skyMaxForm = new QFormLayout(skyMaxRow_);
    skyMaxForm->setContentsMargins(0, 0, 0, 0);
    skyMax_ = new QDoubleSpinBox(skyMaxRow_);
    skyMax_->setObjectName("autoTuneSkyMaxExposure");
    skyMax_->setRange(10, 10'000);
    skyMax_->setDecimals(0);
    skyMax_->setValue(250);
    skyMax_->setSuffix(" ms");
    skyMax_->setToolTip("The longest exposure auto-tune suggests for the sky, and the follower's longest. Longer "
        "exposures reach fainter meteors but blur fast ones; 250 ms is a usual limit.");
    skyMaxForm->addRow("Longest exposure for meteors", skyMax_);
    cameraColumn->addWidget(skyMaxRow_);
    cameraVerdict_ = wrapped(cameraBox, "autoTuneCameraVerdict");
    cameraColumn->addWidget(cameraVerdict_);
    cameraRowsLayout_ = new QVBoxLayout;
    cameraColumn->addLayout(cameraRowsLayout_);
    cameraWarnings_ = wrapped(cameraBox, "autoTuneCameraWarnings");
    cameraWarnings_->setStyleSheet("color: #b37900;");
    cameraWarnings_->hide();
    cameraColumn->addWidget(cameraWarnings_);
    applyCamera_ = new QPushButton("Apply", cameraBox);
    applyCamera_->setObjectName("autoTuneApplyCamera");
    applyCamera_->setToolTip("Sends the ticked camera settings to the camera together, waits until it reports them back "
        "and Preview has settled, then measures again to check.");
    cameraColumn->addWidget(applyCamera_);
    column->addWidget(cameraBox);
    connect(measureCamera_, &QPushButton::clicked, this, &AutoTunePanel::measureCameraRequested);
    connect(applyCamera_, &QPushButton::clicked, this, &AutoTunePanel::applyCameraRequested);

    // Step 2: K.
    auto* kBox = new QGroupBox("2. Sensitivity (K)", this);
    auto* kColumn = new QVBoxLayout(kBox);
    auto* watchRow = new QHBoxLayout;
    auto* lengthLabel = new QLabel("Watch for", kBox);
    length_ = new QComboBox(kBox);
    length_->setObjectName("autoTuneLength");
    length_->addItem("Quick (15 s)", 15.0);
    length_->addItem("Thorough (60 s)", 60.0);
    length_->setToolTip("How long to watch. A thorough watch sees more of the rare large noise values a night brings.");
    lengthLabel->setBuddy(length_);
    watch_ = new QPushButton("Watch", kBox);
    watch_->setObjectName("autoTuneWatch");
    watch_->setToolTip("Runs complete detectors at the current K and the values around it on the same frames, and counts "
        "what each would have triggered. Use it with the camera settings you will observe with.");
    higher_ = new QPushButton("Test higher K", kBox);
    higher_->setObjectName("autoTuneHigher");
    higher_->setToolTip("Watches again at K values above the highest tested.");
    watchRow->addWidget(lengthLabel);
    watchRow->addWidget(length_, 1);
    watchRow->addWidget(watch_);
    watchRow->addWidget(higher_);
    kColumn->addLayout(watchRow);
    tryRow_ = new QWidget(kBox);
    auto* tryLayout = new QHBoxLayout(tryRow_);
    tryLayout->setContentsMargins(0, 0, 0, 0);
    tryK_ = new QDoubleSpinBox(tryRow_);
    tryK_->setObjectName("autoTuneTryKValue");
    tryK_->setRange(kMinimumK, 30);
    tryK_->setDecimals(1);
    tryK_->setSingleStep(0.5);
    tryK_->setToolTip("The K value to try next.");
    tryButton_ = new QPushButton("Try K", tryRow_);
    tryButton_->setObjectName("autoTuneTryK");
    tryButton_->setToolTip("Sets this K in the Detection panel, as an ordinary change, and watches again with it. Used "
        "when the computer has no room to compare several K values at once.");
    tryLayout->addWidget(tryK_);
    tryLayout->addWidget(tryButton_);
    tryLayout->addStretch(1);
    tryRow_->hide();
    kColumn->addWidget(tryRow_);
    kTable_ = new QTableWidget(0, 4, kBox);
    kTable_->setObjectName("autoTuneKTable");
    kTable_->setHorizontalHeaderLabels({"K", "Events", "Measured", "Per-pixel threshold"});
    kTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    kTable_->horizontalHeader()->setStretchLastSection(true);
    kTable_->verticalHeader()->hide();
    kTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    kTable_->setSelectionMode(QAbstractItemView::NoSelection);
    kTable_->setToolTip("What each K would have done during the watch. Events are the candidates Monitoring would have "
        "recorded. The per-pixel threshold is how far a pixel must rise at that K; a flash must also cover the minimum "
        "patch size to be caught.");
    kTable_->setMaximumHeight(140);
    kColumn->addWidget(kTable_);
    events_ = new QListWidget(kBox);
    events_->setObjectName("autoTuneEvents");
    events_->setToolTip("The events found. Tick one that was real (a meteor, a plane, a satellite) to exclude it from the "
        "noise count; a plane crossing quickly can appear as several.");
    events_->setMaximumHeight(110);
    events_->hide();
    kColumn->addWidget(events_);
    kVerdict_ = wrapped(kBox, "autoTuneKVerdict");
    kColumn->addWidget(kVerdict_);
    applyK_ = new QPushButton("Apply K", kBox);
    applyK_->setObjectName("autoTuneApplyK");
    applyK_->setToolTip("Sets the suggested K in the Detection panel. Refused when a camera or detection setting has "
        "changed since the watch.");
    kColumn->addWidget(applyK_);
    checkFlash_ = new QPushButton("Check with a test flash", kBox);
    checkFlash_->setObjectName("autoTuneCheckFlash");
    checkFlash_->setToolTip("Testing tools: injects one marked test flash, of the size set under Test flash, into the "
        "measured area, and reports whether the detector at the current K finds it at its place and time. Never "
        "during a recording.");
    kColumn->addWidget(checkFlash_);
    checkResult_ = wrapped(kBox, "autoTuneCheckResult");
    checkResult_->hide();
    kColumn->addWidget(checkResult_);
    column->addWidget(kBox);

    // Feature-30, step 3: the minimum patch, for flashes only.
    auto* minBox = new QGroupBox("3. Minimum patch (flashes)", this);
    minPatchBox_ = minBox;
    auto* minColumn = new QVBoxLayout(minBox);
    auto* seeingForm = new QFormLayout;
    seeing_ = new QDoubleSpinBox(minBox);
    seeing_->setObjectName("autoTuneSeeing");
    seeing_->setRange(0.5, 10);
    seeing_->setDecimals(1);
    seeing_->setSingleStep(0.1);
    seeing_->setValue(2.5);
    seeing_->setSuffix(" ″");
    seeing_->setToolTip("Your typical seeing (the FWHM of a star). A flash is a point source, as wide as a star: its "
        "width in pixels is the seeing over the image scale (Session → Site and instrument).");
    seeingForm->addRow("Typical seeing", seeing_);
    minColumn->addLayout(seeingForm);
    flashWidth_ = wrapped(minBox, "autoTuneFlashWidth");
    minColumn->addWidget(flashWidth_);
    minPatchWatch_ = new QPushButton("Measure (about 70 s)", minBox);
    minPatchWatch_->setObjectName("autoTuneMinPatchWatch");
    minPatchWatch_->setToolTip("At the current K: injects one marked flash just above K, of the width above, and counts "
        "the pixels it covers (the footprint); then watches for 60 s with the current minimum patch and about 2/3, 1/2 "
        "and 1/3 of it. Suggests the smallest value with no events, no larger than the footprint.");
    minColumn->addWidget(minPatchWatch_);
    footprint_ = wrapped(minBox, "autoTuneFootprint");
    minColumn->addWidget(footprint_);
    minTable_ = new QTableWidget(0, 3, minBox);
    minTable_->setObjectName("autoTuneMinPatchTable");
    minTable_->setHorizontalHeaderLabels({"Minimum patch", "Events", "Measured"});
    minTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    minTable_->horizontalHeader()->setStretchLastSection(true);
    minTable_->verticalHeader()->hide();
    minTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    minTable_->setSelectionMode(QAbstractItemView::NoSelection);
    minTable_->setToolTip("What each minimum patch would have triggered at the current K during the watch.");
    minTable_->setMaximumHeight(130);
    minColumn->addWidget(minTable_);
    minVerdict_ = wrapped(minBox, "autoTuneMinPatchVerdict");
    minColumn->addWidget(minVerdict_);
    applyMinPatch_ = new QPushButton("Apply minimum patch", minBox);
    applyMinPatch_->setObjectName("autoTuneApplyMinPatch");
    applyMinPatch_->setToolTip("Sets the suggested minimum patch in the Detection panel. Refused when a camera or "
        "detection setting has changed since the watch.");
    minColumn->addWidget(applyMinPatch_);
    column->addWidget(minBox);
    connect(seeing_, &QDoubleSpinBox::valueChanged, this, &AutoTunePanel::seeingChanged);
    connect(minPatchWatch_, &QPushButton::clicked, this, &AutoTunePanel::minPatchRequested);
    connect(applyMinPatch_, &QPushButton::clicked, this, &AutoTunePanel::applyMinPatchRequested);
    connect(watch_, &QPushButton::clicked, this, [this] { emit watchRequested(false); });
    connect(higher_, &QPushButton::clicked, this, [this] { emit watchRequested(true); });
    connect(tryButton_, &QPushButton::clicked, this, [this] { emit tryKRequested(tryK_->value()); });
    connect(applyK_, &QPushButton::clicked, this, &AutoTunePanel::applyKRequested);
    connect(checkFlash_, &QPushButton::clicked, this, &AutoTunePanel::checkFlashRequested);
    connect(events_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const int group = item->data(Qt::UserRole).toInt();
        if (group < 0 || group >= static_cast<int>(excluded_.size())) return;
        excluded_[group] = item->checkState() == Qt::Checked;
        choice_ = chooseK(kRows_, groups_, excluded_, kWatchSeconds_);
        showK();
    });

    auto* statusRow = new QHBoxLayout;
    progress_ = new QProgressBar(this);
    progress_->setObjectName("autoTuneProgress");
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->setMaximumHeight(8);
    progress_->hide();
    cancel_ = new QPushButton("Cancel", this);
    cancel_->setObjectName("autoTuneCancel");
    cancel_->setToolTip("Stops the measurement in progress. Nothing has been changed by it.");
    statusRow->addWidget(progress_, 1);
    statusRow->addWidget(cancel_);
    column->addLayout(statusRow);
    status_ = wrapped(this, "autoTuneStatus");
    column->addWidget(status_);
    // In a tall window the spare height goes below everything, not between rows.
    column->addStretch(1);
    connect(cancel_, &QPushButton::clicked, this, &AutoTunePanel::cancelRequested);

    clearCamera({});
    clearK({});
    clearMinPatch({});
    setSituation(situation_);
}

void AutoTunePanel::setSituation(const Situation& situation)
{
    const bool workflowChanged = situation.workflow != situation_.workflow;
    situation_ = situation;
    aim_->setText(aimText(situation.workflow));
    twilight_->setText(situation.twilight);
    twilight_->setVisible(!situation.twilight.isEmpty());
    boxRow_->setVisible(situation.workflow == TuneWorkflow::MoonFlashes);
    skyMaxRow_->setVisible(situation.workflow == TuneWorkflow::Sky);
    minPatchBox_->setVisible(situation.workflow == TuneWorkflow::MoonFlashes);
    // A result for another target or workflow no longer applies.
    if (workflowChanged)
    {
        clearCamera(camera_ ? "The target changed: measure again." : QString());
        clearK(kRows_.empty() ? QString() : "The target changed: watch again.");
        clearMinPatch(minRows_.empty() ? QString() : "The target changed: measure again.");
    }
    if (!camera_ && situation.preview && !situation.camera)
        cameraVerdict_->setText("Not a camera: only the sensitivity (K) can be tuned.");
    else if (!camera_ && cameraVerdict_->text().startsWith("Not a camera")) cameraVerdict_->clear();
    updateButtons();
}

void AutoTunePanel::updateButtons()
{
    const Situation& s = situation_;
    const bool needBox = s.workflow == TuneWorkflow::MoonFlashes && !s.haveBox;
    const bool idle = s.preview && !s.busy;
    measureCamera_->setEnabled(idle && s.camera && !needBox);
    const bool anyTicked = std::any_of(cameraRows_.begin(), cameraRows_.end(), [](const auto& row) { return row.first->isChecked(); });
    applyCamera_->setEnabled(idle && s.camera && !s.recording && camera_.has_value() && anyTicked);
    watch_->setEnabled(idle && !needBox);
    higher_->setEnabled(idle && !needBox && !kRows_.empty());
    tryButton_->setEnabled(idle && !needBox);
    applyK_->setEnabled(!s.busy && choice_.k.has_value());
    minPatchWatch_->setEnabled(idle && !needBox && s.workflow == TuneWorkflow::MoonFlashes && !s.recording);
    applyMinPatch_->setEnabled(!s.busy && minChoice_.minPixels.has_value());
    checkFlash_->setEnabled(idle && s.testingTools && !s.recording);
    checkFlash_->setVisible(s.testingTools);
    cancel_->setVisible(s.busy);
    drawBox_->setEnabled(s.preview);
    QString why;
    if (!s.preview) why = "Auto-tune measures in Preview, once frames arrive.";
    else if (needBox) why = "Draw a box on the earthshine first.";
    else if (!s.camera) why = "A recording is playing: only the sensitivity (K) can be tuned.";
    else if (s.recording) why = "A recording is running: camera changes and the test flash wait until it ends.";
    measureCamera_->setToolTip(why.isEmpty() ? "Measures the brightness of the measured area for two seconds and "
        "suggests exposure and gain for the aim above. Nothing changes until you press Apply." : why);
    // A hint about what is missing, unless a measurement's result is shown.
    if (!s.busy && (status_->text().isEmpty() || status_->property("hint").toBool()))
    {
        status_->setText(why);
        status_->setProperty("hint", true);
    }
}

void AutoTunePanel::setAreaText(const QString& text) { area_->setText(text); }

void AutoTunePanel::setDrawingBox(bool on)
{
    const QSignalBlocker blocked(drawBox_);
    drawBox_->setChecked(on);
}

bool AutoTunePanel::boxOnly() const { return boxOnly_->isChecked(); }
double AutoTunePanel::skyMaxExposureUs() const { return skyMax_->value() * 1000; }
double AutoTunePanel::watchSeconds() const { return length_->currentData().toDouble(); }

void AutoTunePanel::setProgress(const QString& text, double progress, bool running)
{
    status_->setText(text);
    status_->setProperty("hint", false);
    progress_->setVisible(running);
    progress_->setValue(static_cast<int>(std::lround(100 * std::clamp(progress, 0.0, 1.0))));
}

void AutoTunePanel::setCameraSuggestion(const CameraSuggestion& suggestion)
{
    clearCamera({});
    camera_ = suggestion;
    cameraVerdict_->setText(QString::fromStdString(suggestion.verdict));
    for (const TuneRow& row : suggestion.rows)
    {
        auto* box = new QCheckBox(QString("%1: %2 → %3").arg(QString::fromStdString(row.name),
            QString::fromStdString(row.now), QString::fromStdString(row.suggested)), this);
        box->setObjectName(QString("autoTuneRow%1").arg(static_cast<int>(row.kind)));
        box->setChecked(row.ticked);
        box->setEnabled(row.now != row.suggested);
        box->setToolTip(QString::fromStdString(row.why));
        box->setWhatsThis(box->toolTip());
        auto* why = wrapped(this);
        why->setText(QString::fromStdString(row.why));
        why->setStyleSheet("color: palette(mid); margin-left: 22px;");
        cameraRowsLayout_->addWidget(box);
        cameraRowsLayout_->addWidget(why);
        cameraRows_.emplace_back(box, row);
        connect(box, &QCheckBox::toggled, this, &AutoTunePanel::updateButtons);
    }
    QStringList warnings;
    for (const std::string& warning : suggestion.warnings) warnings << QString::fromStdString(warning);
    cameraWarnings_->setText(warnings.join("\n"));
    cameraWarnings_->setVisible(!warnings.isEmpty());
    updateButtons();
}

void AutoTunePanel::clearCamera(const QString& why)
{
    camera_.reset();
    while (QLayoutItem* item = cameraRowsLayout_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    cameraRows_.clear();
    cameraVerdict_->setText(why);
    cameraWarnings_->clear();
    cameraWarnings_->hide();
    updateButtons();
}

std::vector<TuneRow> AutoTunePanel::tickedCameraRows() const
{
    std::vector<TuneRow> rows;
    for (const auto& [box, row] : cameraRows_)
        if (box->isChecked() && box->isEnabled()) rows.push_back(row);
    return rows;
}

void AutoTunePanel::setKResults(const TuneWatchStatus& status)
{
    // With only the live detector, each watch adds its K to the earlier ones.
    if (!(status.liveOnly && liveOnly_)) kRows_.clear();
    for (const KRowResult& row : status.rows)
    {
        std::erase_if(kRows_, [&](const KRowResult& old) { return old.k == row.k; });
        kRows_.push_back(row);
    }
    std::sort(kRows_.begin(), kRows_.end(), [](const KRowResult& a, const KRowResult& b) { return a.k < b.k; });
    liveOnly_ = status.liveOnly;
    kWatchSeconds_ = status.requestedSeconds;
    areaNoise_ = status.areaNoiseAdu;
    format_ = status.format;
    groups_ = groupEvents(kRows_);
    excluded_.assign(groups_.size(), false);
    choice_ = chooseK(kRows_, groups_, excluded_, kWatchSeconds_);
    tryRow_->setVisible(liveOnly_);
    tryK_->setValue(highestTestedK() + 1);

    const QSignalBlocker blocked(events_);
    events_->clear();
    for (size_t g = 0; g < groups_.size(); ++g)
    {
        const TuneEvent& e = groups_[g].first;
        QStringList ks;
        for (const auto& [row, index] : groups_[g].members) ks << QString::number(kRows_[row].k, 'f', 1);
        auto* item = new QListWidgetItem(QString("Exclude: %1, at (%2, %3), %4 px, found at K %5")
            .arg(utcDateTime(e.time).toLocalTime().toString("HH:mm:ss.zzz")).arg(e.x + e.width / 2).arg(e.y + e.height / 2)
            .arg(e.pixels).arg(ks.join(", ")), events_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, static_cast<int>(g));
    }
    events_->setVisible(!groups_.empty());
    QString note = QString::fromStdString(status.note);
    if (format_ == PixelFormat::Mono8 && areaNoise_ > 0 && areaNoise_ < 1)
        note += (note.isEmpty() ? "" : "\n") + QString("The noise is below 1 ADU on this 8-bit stream: each ADU is a coarse "
            "step for faint flashes. Raise the gain or use RAW16.");
    kTable_->setProperty("note", note);
    showK();
}

void AutoTunePanel::showK()
{
    kTable_->setRowCount(static_cast<int>(kRows_.size()));
    for (size_t i = 0; i < kRows_.size(); ++i)
    {
        const KRowResult& row = kRows_[i];
        const uint64_t remaining = remainingEvents(kRows_, i, groups_, excluded_);
        QString measured = QString("%1 s").arg(row.seconds, 0, 'f', 1);
        if (!row.stopped.empty()) measured = QString::fromStdString(row.stopped);
        else if (row.limitedFrames) measured += QString(", search skipped on %1 frame(s)").arg(row.limitedFrames);
        const QString events = remaining == row.events ? QString::number(row.events)
            : QString("%1 (%2 found)").arg(remaining).arg(row.events);
        const QString threshold = areaNoise_ > 0 ? QString("%1 ADU").arg(row.k * areaNoise_, 0, 'f', areaNoise_ < 10 ? 1 : 0) : "-";
        const QStringList cells{QString::number(row.k, 'f', 1) + (choice_.k && *choice_.k == row.k ? " ✓" : ""),
            events, measured, threshold};
        for (int c = 0; c < cells.size(); ++c) kTable_->setItem(static_cast<int>(i), c, new QTableWidgetItem(cells[c]));
    }
    QString verdict;
    if (kRows_.empty()) verdict = kVerdict_->property("why").toString();
    else if (choice_.k)
        verdict = QString("Suggested K %1: no events left in %2 s, with the patch search never skipped. Per-pixel "
            "threshold in the measured area: %3.").arg(*choice_.k, 0, 'f', 1).arg(kWatchSeconds_, 0, 'f', 0)
            .arg(areaNoise_ > 0 ? QString("%1 ADU").arg(*choice_.k * areaNoise_, 0, 'f', 1) : QString("not measured"));
    else verdict = QString::fromStdString(choice_.reason);
    const QString note = kTable_->property("note").toString();
    kVerdict_->setText(note.isEmpty() ? verdict : verdict + "\n" + note);
    updateButtons();
}

void AutoTunePanel::clearK(const QString& why)
{
    kRows_.clear();
    groups_.clear();
    excluded_.clear();
    choice_ = {};
    liveOnly_ = false;
    events_->clear();
    events_->hide();
    tryRow_->hide();
    kTable_->setProperty("note", QString());
    kVerdict_->setProperty("why", why);
    showK();
}

std::optional<double> AutoTunePanel::suggestedK() const { return choice_.k; }

double AutoTunePanel::highestTestedK() const
{
    double highest = 0;
    for (const KRowResult& row : kRows_) highest = std::max(highest, row.k);
    return highest;
}

QString AutoTunePanel::kRecord() const
{
    QStringList rows;
    for (size_t i = 0; i < kRows_.size(); ++i)
        rows << QString("%1:%2/%3:%4s:%5skipped").arg(kRows_[i].k, 0, 'f', 1).arg(remainingEvents(kRows_, i, groups_, excluded_))
            .arg(kRows_[i].events).arg(kRows_[i].seconds, 0, 'f', 1).arg(kRows_[i].limitedFrames);
    return QString("k_tested=\"%1\" excluded_events=%2 watch_s=%3 noise_adu=%4")
        .arg(rows.join(' ')).arg(std::count(excluded_.begin(), excluded_.end(), true)).arg(kWatchSeconds_, 0, 'f', 0)
        .arg(areaNoise_, 0, 'g', 4);
}

void AutoTunePanel::setCheckResult(const QString& text, std::optional<bool> caught)
{
    checkResult_->setText(text);
    checkResult_->setStyleSheet(!caught ? QString() : *caught ? "color: #2e8b57;" : "color: #c0392b;");
    checkResult_->setVisible(!text.isEmpty());
}

double AutoTunePanel::seeingArcsec() const { return seeing_->value(); }

void AutoTunePanel::setSeeingArcsec(double arcsec)
{
    const QSignalBlocker blocked(seeing_);
    seeing_->setValue(arcsec);
}

void AutoTunePanel::setFlashWidthText(const QString& text) { flashWidth_->setText(text); }
void AutoTunePanel::setFootprintText(const QString& text) { footprint_->setText(text); }

void AutoTunePanel::setMinPatchResults(const TuneWatchStatus& status, std::optional<int> footprint)
{
    minRows_ = status.rows;
    std::sort(minRows_.begin(), minRows_.end(), [](const KRowResult& a, const KRowResult& b) { return a.minPixels > b.minPixels; });
    minWatchSeconds_ = status.requestedSeconds;
    footprintPixels_ = footprint;
    const auto groups = groupEvents(minRows_);
    minChoice_ = chooseMinPatch(minRows_, groups, std::vector<bool>(groups.size(), false), minWatchSeconds_, footprint);
    minTable_->setRowCount(static_cast<int>(minRows_.size()));
    for (size_t i = 0; i < minRows_.size(); ++i)
    {
        const KRowResult& row = minRows_[i];
        QString measured = QString("%1 s").arg(row.seconds, 0, 'f', 1);
        if (!row.stopped.empty()) measured = QString::fromStdString(row.stopped);
        else if (row.limitedFrames) measured += QString(", search skipped on %1 frame(s)").arg(row.limitedFrames);
        const bool chosen = minChoice_.minPixels && *minChoice_.minPixels == row.minPixels;
        const QStringList cells{QString("%1 px%2").arg(row.minPixels).arg(chosen ? " ✓" : ""), QString::number(row.events), measured};
        for (int c = 0; c < cells.size(); ++c) minTable_->setItem(static_cast<int>(i), c, new QTableWidgetItem(cells[c]));
    }
    QString verdict = minChoice_.minPixels
        ? QString("Suggested minimum patch %1 px. %2").arg(*minChoice_.minPixels).arg(QString::fromStdString(minChoice_.reason))
        : QString::fromStdString(minChoice_.reason);
    if (!status.note.empty()) verdict += "\n" + QString::fromStdString(status.note);
    minVerdict_->setText(verdict);
    minVerdict_->setStyleSheet(minChoice_.noiseLimited ? "color: #b37900;" : QString());
    updateButtons();
}

void AutoTunePanel::clearMinPatch(const QString& why)
{
    minRows_.clear();
    footprintPixels_.reset();
    minChoice_ = {};
    minTable_->setRowCount(0);
    minVerdict_->setText(why);
    minVerdict_->setStyleSheet({});
    if (why.isEmpty()) footprint_->clear();
    updateButtons();
}

std::optional<int> AutoTunePanel::suggestedMinPatch() const { return minChoice_.minPixels; }

QString AutoTunePanel::minPatchRecord() const
{
    QStringList rows;
    for (const KRowResult& row : minRows_)
        rows << QString("%1:%2:%3s:%4skipped").arg(row.minPixels).arg(row.events).arg(row.seconds, 0, 'f', 1).arg(row.limitedFrames);
    return QString("min_patch_tested=\"%1\" footprint_px=%2 noise_limited=%3 watch_s=%4").arg(rows.join(' '))
        .arg(footprintPixels_ ? QString::number(*footprintPixels_) : QString("unmeasured"))
        .arg(minChoice_.noiseLimited ? "yes" : "no").arg(minWatchSeconds_, 0, 'f', 0);
}
