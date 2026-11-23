#include "ui/timelapse_panel.h"

#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QPixmap>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

TimelapsePanel::TimelapsePanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("timelapsePanel");
    setMinimumWidth(360);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget(scroll);
    auto* root = new QVBoxLayout(body);

    form_ = new QFormLayout();
    auto* form = form_;
    preset_ = new QComboBox(body);
    preset_->setObjectName("timelapsePreset");
    preset_->setToolTip("Moon: rigid alignment with field rotation, keeps the best 25 %; select a lunar anchor, or a "
        "passing cloud can rotate a stack wrongly. Planet: keeps the disk where it was in the first stack, by its "
        "brightness-weighted centre, never rotated; keeps the best 15 %. Sets the defaults below.");
    preset_->addItem("Moon", static_cast<int>(TimelapsePreset::Moon));
    preset_->addItem("Planet", static_cast<int>(TimelapsePreset::Planet));
    form->addRow("Target preset", preset_);

    interval_ = new QDoubleSpinBox(body);
    interval_->setObjectName("timelapseInterval");
    interval_->setToolTip("Time from the start of one burst to the start of the next: one stack, one video frame, per interval. At least the capture window.");
    interval_->setRange(0.5, 86400.0);
    interval_->setDecimals(1);
    interval_->setSuffix(" s");
    interval_->setValue(60.0);
    form->addRow("Interval", interval_);

    window_ = new QDoubleSpinBox(body);
    window_->setObjectName("timelapseWindow");
    window_->setToolTip("How long each burst collects frames. Keep it short enough that the target does not change during it (for planets, under a minute or so).");
    window_->setRange(0.1, 3600.0);
    window_->setDecimals(1);
    window_->setSuffix(" s");
    window_->setValue(15.0);
    form->addRow("Capture window", window_);

    keepPercent_ = new QDoubleSpinBox(body);
    keepPercent_->setObjectName("timelapseKeepPercent");
    keepPercent_->setToolTip("Share of the burst's sharpest frames that are stacked; the rest are discarded.");
    keepPercent_->setRange(1.0, 100.0);
    keepPercent_->setDecimals(0);
    keepPercent_->setSuffix(" %");
    keepPercent_->setValue(25.0);
    form->addRow("Keep best", keepPercent_);

    maxFrames_ = new QSpinBox(body);
    maxFrames_->setObjectName("timelapseMaxFrames");
    maxFrames_->setRange(1, TimelapseSettings::kMaxFramesCap);
    maxFrames_->setValue(150);
    maxFrames_->setToolTip("Maximum kept frames stacked per period (100-200 recommended; hard limit 200).");
    form->addRow("Max frames/period", maxFrames_);

    sharpening_ = new QComboBox(body);
    sharpening_->setObjectName("timelapseSharpening");
    sharpening_->setToolTip("Sharpening applied to each stack for the video and sharpened/ copies. The unsharpened stacks are always kept, and Re-sharpen can change it later.");
    form->addRow("Sharpening", sharpening_);

    cropToLock_ = new QCheckBox("Crop around locked feature", body);
    cropToLock_->setObjectName("timelapseCropLock");
    cropToLock_->setToolTip("Crops each stack around the feature locked for guiding, instead of keeping the full frame.");
    form->addRow(cropToLock_);

    auto* anchorRow = new QHBoxLayout();
    selectAnchor_ = new QToolButton(body);
    selectAnchor_->setObjectName("timelapseSelectAnchor");
    selectAnchor_->setCheckable(true);
    selectAnchor_->setText("Select lunar anchor");
    selectAnchor_->setToolTip("Strongly recommended for the Moon. Drag a box on a sunlit region that keeps steady "
        "shadows for the whole session: each stack's shift and rotation are then fitted inside it only. Without an "
        "anchor they are fitted over the whole frame, sky included, and a passing cloud can rotate a stack wrongly.");
    clearAnchor_ = new QPushButton("Clear", body);
    clearAnchor_->setObjectName("timelapseClearAnchor");
    clearAnchor_->setToolTip("Removes the lunar anchor: shift and rotation are fitted over the whole frame again, "
        "sky included.");
    anchorRow->addWidget(selectAnchor_, 1);
    anchorRow->addWidget(clearAnchor_);
    form->addRow("Lunar anchor", anchorRow);

    anchorLabel_ = new QLabel(body);
    anchorLabel_->setObjectName("timelapseAnchorLabel");
    anchorLabel_->setWordWrap(true);
    form->addRow(QString(), anchorLabel_);
    showAnchor();

    stopKind_ = new QComboBox(body);
    stopKind_->setObjectName("timelapseStopKind");
    stopKind_->setToolTip("When the time-lapse ends: when the session ends (the Ends setting above the preview, or "
        "Stop), or also after a number of stacks. The video is built when it ends.");
    stopKind_->addItem("When the session ends", static_cast<int>(StopConditionKind::Manual));
    stopKind_->addItem("After a number of stacks", static_cast<int>(StopConditionKind::StackCount));
    form->addRow("Stops", stopKind_);

    targetStacks_ = new QSpinBox(body);
    targetStacks_->setObjectName("timelapseTargetStacks");
    targetStacks_->setToolTip("The time-lapse stops after this many stacks.");
    targetStacks_->setRange(1, 10000);
    targetStacks_->setValue(60);
    form->addRow("Stack count", targetStacks_);

    auto* outRow = new QHBoxLayout();
    writeMp4_ = new QCheckBox("MP4", body);
    writeMp4_->setObjectName("timelapseMp4");
    writeMp4_->setToolTip("Builds timelapse.mp4 (H.264) from the stacks when the time-lapse ends.");
    writeMp4_->setChecked(true);
    writeGif_ = new QCheckBox("GIF", body);
    writeGif_->setObjectName("timelapseGif");
    writeGif_->setToolTip("Also builds timelapse.gif, which plays everywhere but is larger and has fewer colours.");
    writeSer_ = new QCheckBox("SER", body);
    writeSer_->setObjectName("timelapseSer");
    writeSer_->setToolTip("Also builds timelapse.ser (one lossless frame per stack with UTC timestamps) for SER "
        "Player or further processing. Much smaller than raw SER bursts below.");
    videoFps_ = new QSpinBox(body);
    videoFps_->setObjectName("timelapseVideoFps");
    videoFps_->setToolTip("Frames per second of the MP4/GIF video: each stack is one frame.");
    videoFps_->setRange(1, 60);
    videoFps_->setValue(12);
    videoFps_->setSuffix(" fps");
    outRow->addWidget(writeMp4_);
    outRow->addWidget(writeGif_);
    outRow->addWidget(writeSer_);
    outRow->addWidget(videoFps_, 1);
    form->addRow("Output", outRow);

    stretchSer_ = new QCheckBox("Stretch SER (match MP4)", body);
    stretchSer_->setObjectName("timelapseStretchSer");
    stretchSer_->setToolTip("Off (default) writes unclipped linear 16-bit mono / RGB stacks to timelapse.ser for "
        "further processing in SER Player. On applies the same black/white stretch and gamma as timelapse.mp4.");
    form->addRow(stretchSer_);

    saveRawSer_ = new QCheckBox("Also save raw SER bursts (uses a lot of disk space)", body);
    saveRawSer_->setObjectName("timelapseSaveSer");
    saveRawSer_->setToolTip("Writes every stacked frame of every burst to raw/*.ser. This can fill a drive in "
        "one night. Turning it on asks for confirmation, and it is off again each time SeleneWatch starts.");
    form->addRow(saveRawSer_);

    watchTransits_ = new QCheckBox("Also watch for transits", body);
    watchTransits_->setObjectName("timelapseWatchTransits");
    watchTransits_->setToolTip("Watch for crossings between time-lapse bursts and save triggered clips in native SER. Watching pauses during burst capture and stacking.");
    form->addRow(watchTransits_);
    transitHelp_ = new QLabel(body);
    transitHelp_->setWordWrap(true);
    form->addRow(transitHelp_);
    configureTransits_ = new QPushButton("Configure transit detection…", body);
    configureTransits_->setObjectName("timelapseConfigureTransits");
    configureTransits_->setToolTip("Opens the shadows watch in the Detection dock. Use Preview to check the settings on the current stream.");
    form->addRow(configureTransits_);

    estimateLabel_ = new QLabel(body);
    estimateLabel_->setObjectName("timelapseEstimate");
    estimateLabel_->setWordWrap(true);
    form->addRow(estimateLabel_);
    root->addLayout(form);

    auto* btnRow = new QHBoxLayout();
    startStopButton_ = new QPushButton("Start time-lapse", body);
    startStopButton_->setObjectName("timelapseStartButton");
    startStopButton_->setToolTip("Starts the time-lapse from the current stream (starting Preview if needed), or stops it and builds the video.");
    resharpenButton_ = new QPushButton("Re-sharpen…", body);
    resharpenButton_->setObjectName("timelapseResharpenButton");
    resharpenButton_->setToolTip("Opens Re-sharpen for a finished time-lapse: try other sharpening and tone settings, and rebuild the video, without changing the stacks.");
    btnRow->addWidget(startStopButton_, 1);
    btnRow->addWidget(resharpenButton_);
    root->addLayout(btnRow);
    openTransitsButton_ = new QPushButton("Open transit clips", body);
    openTransitsButton_->setObjectName("timelapseOpenTransits");
    openTransitsButton_->setToolTip("Opens the finished session's transit clips and candidate log.");
    openTransitsButton_->hide();
    root->addWidget(openTransitsButton_);
    connect(openTransitsButton_, &QPushButton::clicked, this, [this] {
        if (!lastTransitFolder_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(lastTransitFolder_));
    });

    auto* liveBox = new QGroupBox("Session progress && preview", body);
    auto* liveLayout = new QVBoxLayout(liveBox);
    progressBar_ = new QProgressBar(liveBox);
    progressBar_->setObjectName("timelapseProgress");
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    liveLayout->addWidget(progressBar_);

    timingLabel_ = new QLabel("Idle", liveBox);
    timingLabel_->setObjectName("timelapseTiming");
    timingLabel_->setWordWrap(true);
    liveLayout->addWidget(timingLabel_);
    transitStatus_ = new QLabel(liveBox);
    transitStatus_->setObjectName("timelapseTransitStatus");
    transitStatus_->setWordWrap(true);
    transitStatus_->hide();
    liveLayout->addWidget(transitStatus_);

    statsLabel_ = new QLabel(liveBox);
    statsLabel_->setObjectName("timelapseStats");
    statsLabel_->setWordWrap(true);
    liveLayout->addWidget(statsLabel_);

    warningLabel_ = new QLabel(liveBox);
    warningLabel_->setObjectName("timelapseWarning");
    warningLabel_->setWordWrap(true);
    warningLabel_->setStyleSheet("color: #b45309; font-weight: bold;");
    warningLabel_->hide();
    liveLayout->addWidget(warningLabel_);

    auto* thumbs = new QHBoxLayout();
    auto* latestCol = new QVBoxLayout();
    latestCol->addWidget(new QLabel("Latest stack", liveBox));
    latestThumb_ = new QLabel(liveBox);
    latestThumb_->setObjectName("timelapseLatestThumb");
    latestThumb_->setMinimumSize(140, 100);
    latestThumb_->setAlignment(Qt::AlignCenter);
    latestThumb_->setStyleSheet("background:#111; color:#888; border:1px solid #333;");
    latestThumb_->setText("--");
    latestCol->addWidget(latestThumb_);
    auto* latestNote = new QLabel("Most recent", liveBox);
    latestNote->setAlignment(Qt::AlignCenter);
    latestCol->addWidget(latestNote);

    auto* filmCol = new QVBoxLayout();
    auto* filmHead = new QHBoxLayout();
    filmHead->addWidget(new QLabel("Filmstrip loop", liveBox));
    loopFilmstrip_ = new QCheckBox("Loop", liveBox);
    loopFilmstrip_->setToolTip("Plays the stacks so far as a loop in the filmstrip, a preview of the video.");
    loopFilmstrip_->setChecked(true);
    filmHead->addWidget(loopFilmstrip_);
    filmCol->addLayout(filmHead);
    filmstripThumb_ = new QLabel(liveBox);
    filmstripThumb_->setObjectName("timelapseFilmstripThumb");
    filmstripThumb_->setMinimumSize(140, 100);
    filmstripThumb_->setAlignment(Qt::AlignCenter);
    filmstripThumb_->setStyleSheet("background:#111; color:#888; border:1px solid #333;");
    filmstripThumb_->setText("--");
    filmCol->addWidget(filmstripThumb_);
    filmstripInfo_ = new QLabel("0 stacks", liveBox);
    filmstripInfo_->setAlignment(Qt::AlignCenter);
    filmCol->addWidget(filmstripInfo_);

    thumbs->addLayout(latestCol, 1);
    thumbs->addLayout(filmCol, 1);
    liveLayout->addLayout(thumbs);
    root->addWidget(liveBox);
    root->addStretch(1);

    scroll->setWidget(body);
    outer->addWidget(scroll);

    filmstripTimer_ = new QTimer(this);
    filmstripTimer_->setInterval(250);
    connect(filmstripTimer_, &QTimer::timeout, this, &TimelapsePanel::updateFilmstripFrame);
    connect(loopFilmstrip_, &QCheckBox::toggled, this, [this](bool on) {
        if (on && filmstripFrames_.size() > 1) filmstripTimer_->start();
        else filmstripTimer_->stop();
    });

    refreshProfiles();
    loadFromQSettings();
    updateTransitControls();

    connect(preset_, &QComboBox::currentIndexChanged, this, [this] {
        showAnchor();
        if (updating_) return;
        applyPresetDefaults(static_cast<TimelapsePreset>(preset_->currentData().toInt()));
        updateTransitControls();
        saveToQSettings();
        updateEstimates();
    });
    connect(interval_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (window_->value() > v) window_->setValue(v);
        if (!updating_) { saveToQSettings(); updateEstimates(); }
    });
    connect(window_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (v > interval_->value()) interval_->setValue(v);
        if (!updating_) { saveToQSettings(); updateEstimates(); }
    });
    connect(stopKind_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        saveToQSettings();
        updateEstimates();
    });
    for (QObject* w : std::initializer_list<QObject*>{keepPercent_, maxFrames_, sharpening_, cropToLock_,
             targetStacks_, writeMp4_, writeGif_, writeSer_, stretchSer_, videoFps_, saveRawSer_})
    {
        if (auto* sb = qobject_cast<QDoubleSpinBox*>(w))
            connect(sb, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) { saveToQSettings(); updateEstimates(); } });
        else if (auto* sp = qobject_cast<QSpinBox*>(w))
            connect(sp, &QSpinBox::valueChanged, this, [this] { if (!updating_) { saveToQSettings(); updateEstimates(); } });
        else if (auto* cb = qobject_cast<QComboBox*>(w))
            connect(cb, &QComboBox::currentIndexChanged, this, [this] { if (!updating_) { saveToQSettings(); updateEstimates(); } });
        else if (auto* ck = qobject_cast<QCheckBox*>(w))
            connect(ck, &QCheckBox::toggled, this, [this] { if (!updating_) { saveToQSettings(); updateEstimates(); } });
    }

    // Only the observer's click asks; setSettings() applies a value without asking.
    connect(saveRawSer_, &QCheckBox::clicked, this, [this](bool checked) {
        if (checked && !confirmRawSer()) saveRawSer_->setChecked(false);
    });
    connect(watchTransits_, &QCheckBox::toggled, this, [this] {
        if (!updating_) saveToQSettings();
        updateTransitControls();
    });
    connect(configureTransits_, &QPushButton::clicked, this, &TimelapsePanel::configureTransitsRequested);

    connect(selectAnchor_, &QToolButton::toggled, this, &TimelapsePanel::anchorSelectionChanged);
    connect(clearAnchor_, &QPushButton::clicked, this, [this] {
        setLunarAnchor({});
        emit anchorChanged(std::nullopt);
    });
    connect(startStopButton_, &QPushButton::clicked, this, [this] {
        if (timeLapsing_) emit stopRequested();
        else emit startRequested();
    });
    connect(resharpenButton_, &QPushButton::clicked, this, [this] {
        emit resharpenRequested(lastSessionDir_);
    });

    updateEstimates();
}

void TimelapsePanel::refreshProfiles()
{
    const QSignalBlocker b(sharpening_);
    const QString prev = sharpening_->currentData().toString();
    sharpening_->clear();
    sharpening_->addItem("Off", "preset:off");
    sharpening_->addItem("Mild (default)", "preset:mild");
    sharpening_->addItem("Medium", "preset:medium");
    savedProfiles_ = loadSavedSharpeningProfiles();
    for (const auto& p : savedProfiles_)
        sharpening_->addItem(QString("Profile: %1").arg(p.name), "profile:" + p.name);
    const int idx = sharpening_->findData(prev);
    sharpening_->setCurrentIndex(idx >= 0 ? idx : 1);
}

void TimelapsePanel::applyPresetDefaults(TimelapsePreset preset)
{
    const QSignalBlocker b1(keepPercent_);
    const QSignalBlocker b2(maxFrames_);
    const QSignalBlocker b3(selectAnchor_);
    const TimelapseSettings def = defaultTimelapseSettings(preset);
    keepPercent_->setValue(def.keepPercent);
    maxFrames_->setValue(def.maxFramesPerPeriod);
    if (preset == TimelapsePreset::Planet)
    {
        const bool wasSelecting = selectAnchor_->isChecked();
        selectAnchor_->setChecked(false);
        selectAnchor_->setEnabled(false);
        if (wasSelecting) emit anchorSelectionChanged(false);
    }
    else
    {
        selectAnchor_->setEnabled(!timeLapsing_);
    }
}

TimelapseSettings TimelapsePanel::settings() const
{
    TimelapseSettings s;
    s.preset = static_cast<TimelapsePreset>(preset_->currentData().toInt());
    s.intervalSeconds = interval_->value();
    s.captureWindowSeconds = std::min(window_->value(), s.intervalSeconds);
    s.keepPercent = keepPercent_->value();
    s.maxFramesPerPeriod = maxFrames_->value();
    s.cropToLock = cropToLock_->isChecked();
    s.lunarAnchor = (s.preset == TimelapsePreset::Moon) ? lunarAnchor_ : std::nullopt;
    s.stopKind = static_cast<StopConditionKind>(stopKind_->currentData().toInt());
    s.targetStacks = targetStacks_->value();
    s.saveRawSer = saveRawSer_->isChecked();
    s.watchTransits = moonWatch_ && s.preset == TimelapsePreset::Moon && watchTransits_->isChecked();

    const QString key = sharpening_->currentData().toString();
    if (key == "preset:off") s.sharpening = sharpeningForPreset(SharpeningPreset::Off);
    else if (key == "preset:medium") s.sharpening = sharpeningForPreset(SharpeningPreset::Medium);
    else if (key.startsWith("profile:"))
    {
        const QString name = key.mid(8);
        s.sharpening = sharpeningForPreset(SharpeningPreset::Mild);
        for (const auto& p : savedProfiles_)
        {
            if (p.name == name)
            {
                s.sharpening = p.sharpening;
                s.sharpening.profileName = p.name;
                s.video = p.video;
                break;
            }
        }
    }
    else
    {
        s.sharpening = sharpeningForPreset(SharpeningPreset::Mild);
    }

    s.video.writeMp4 = writeMp4_->isChecked();
    s.video.writeGif = writeGif_->isChecked();
    s.video.writeSer = writeSer_->isChecked();
    s.video.stretchSer = stretchSer_->isChecked();
    s.video.fps = videoFps_->value();
    return s;
}

void TimelapsePanel::setSettings(const TimelapseSettings& s)
{
    updating_ = true;
    preset_->setCurrentIndex(s.preset == TimelapsePreset::Planet ? 1 : 0);
    interval_->setValue(s.intervalSeconds);
    window_->setValue(s.captureWindowSeconds);
    keepPercent_->setValue(s.keepPercent);
    maxFrames_->setValue(std::clamp(s.maxFramesPerPeriod, 1, TimelapseSettings::kMaxFramesCap));
    cropToLock_->setChecked(s.cropToLock);
    setLunarAnchor(s.lunarAnchor);
    stopKind_->setCurrentIndex(std::max(0, stopKind_->findData(static_cast<int>(s.stopKind))));
    targetStacks_->setValue(s.targetStacks);
    writeMp4_->setChecked(s.video.writeMp4);
    writeGif_->setChecked(s.video.writeGif);
    writeSer_->setChecked(s.video.writeSer);
    stretchSer_->setChecked(s.video.stretchSer);
    videoFps_->setValue(s.video.fps);
    saveRawSer_->setChecked(s.saveRawSer);
    watchTransits_->setChecked(s.watchTransits);
    updating_ = false;
    updateTransitControls();
    updateEstimates();
}

void TimelapsePanel::setTransitOptions(bool moonWatch, bool hasSavedSettings, bool editable)
{
    moonWatch_ = moonWatch;
    transitSettingsSaved_ = hasSavedSettings;
    transitEditable_ = editable;
    updateTransitControls();
}

void TimelapsePanel::updateTransitControls()
{
    const bool available = moonWatch_ && preset_->currentData().toInt() == static_cast<int>(TimelapsePreset::Moon);
    const bool selected = available && watchTransits_->isChecked();
    watchTransits_->setVisible(available);
    watchTransits_->setEnabled(transitEditable_);
    transitHelp_->setVisible(selected);
    transitHelp_->setText(transitSettingsSaved_
        ? "Uses your saved The Moon: shadows on the sunlit side settings. Watching pauses during burst capture and stacking."
        : "Using default transit settings. Watching pauses during burst capture and stacking.");
    configureTransits_->setVisible(selected);
    configureTransits_->setEnabled(transitEditable_);
}

void TimelapsePanel::setStreamInfo(bool running, bool monitoring, bool recording, bool timeLapsing,
    double estimatedFps, int frameWidth, int frameHeight, int bytesPerPixel, bool hasLock,
    bool finalizing)
{
    running_ = running;
    monitoring_ = monitoring;
    recording_ = recording;
    timeLapsing_ = timeLapsing;
    if (estimatedFps > 0.5) estimatedFps_ = estimatedFps;
    if (frameWidth > 0) frameWidth_ = frameWidth;
    if (frameHeight > 0) frameHeight_ = frameHeight;
    if (bytesPerPixel > 0) bytesPerPixel_ = bytesPerPixel;
    hasLock_ = hasLock;

    const bool busy = timeLapsing_ || finalizing;
    const bool editable = !busy;
    for (QWidget* w : std::initializer_list<QWidget*>{preset_, interval_, window_, keepPercent_, maxFrames_,
             sharpening_, cropToLock_, clearAnchor_, stopKind_, targetStacks_,
             writeMp4_, writeGif_, writeSer_, stretchSer_, videoFps_, saveRawSer_})
    {
        w->setEnabled(editable);
    }
    selectAnchor_->setEnabled(editable && running_
        && static_cast<TimelapsePreset>(preset_->currentData().toInt()) == TimelapsePreset::Moon);
    cropToLock_->setEnabled(editable && hasLock_);

    if (finalizing)
    {
        startStopButton_->setText("Finalizing…");
        startStopButton_->setEnabled(false);
    }
    else
    {
        startStopButton_->setText(timeLapsing_ ? "Stop time-lapse" : "Start time-lapse");
        startStopButton_->setEnabled(timeLapsing_ || (!monitoring_ && !recording_));
    }
    resharpenButton_->setEnabled(!busy);
    updateEstimates();
}

void TimelapsePanel::setLunarAnchor(const std::optional<QRect>& rect)
{
    lunarAnchor_ = rect;
    {
        const QSignalBlocker b(selectAnchor_);
        selectAnchor_->setChecked(false);
    }
    showAnchor();
    clearAnchor_->setEnabled(!timeLapsing_ && rect.has_value());
}

// What the alignment between stacks is fitted on. Without an anchor, a Moon
// time-lapse fits shift and rotation over the whole frame, sky included, and
// a passing cloud can rotate a stack wrongly, so the row warns.
void TimelapsePanel::showAnchor()
{
    const bool planet = static_cast<TimelapsePreset>(preset_->currentData().toInt()) == TimelapsePreset::Planet;
    const bool anchored = lunarAnchor_ && lunarAnchor_->isValid();
    if (planet)
        anchorLabel_->setText("Not used: a planet is aligned on its disk, without rotation.");
    else if (anchored)
        anchorLabel_->setText(QString("Anchor (%1, %2) %3 × %4 px").arg(lunarAnchor_->x()).arg(lunarAnchor_->y())
            .arg(lunarAnchor_->width()).arg(lunarAnchor_->height()));
    else
        anchorLabel_->setText("No anchor: shift and rotation are fitted over the whole frame, sky included, so a "
            "passing cloud can rotate a stack wrongly. Select a steady sunlit area.");
    // The same amber as the time-lapse warnings below.
    anchorLabel_->setStyleSheet(!planet && !anchored ? "color: #b45309; font-weight: bold;" : QString());
}

bool TimelapsePanel::selectingAnchor() const
{
    return selectAnchor_->isChecked();
}

void TimelapsePanel::cancelAnchorSelection()
{
    const QSignalBlocker b(selectAnchor_);
    selectAnchor_->setChecked(false);
}

void TimelapsePanel::updateEstimates()
{
    const auto stop = static_cast<StopConditionKind>(stopKind_->currentData().toInt());
    form_->setRowVisible(targetStacks_, stop == StopConditionKind::StackCount);
    form_->setRowVisible(stretchSer_, writeSer_->isChecked());

    const double estCaptured = std::max(1.0, window_->value() * estimatedFps_);
    const int estKept = estimatedKeptFrames();
    const double stacksPerHour = 3600.0 / std::max(0.5, interval_->value());
    const double fitsBytesPerStack = static_cast<double>(frameWidth_) * frameHeight_ * 4.0 * 2.0; // unsharpened + sharpened
    double gbPerHour = (stacksPerHour * fitsBytesPerStack) / (1024.0 * 1024.0 * 1024.0);
    if (saveRawSer_->isChecked()) gbPerHour += rawSerGiBPerHour();

    QString note = QString("~%1 frames/window at %2 fps → stacking up to %3 best. Est. disk: %4 GB/hr.")
        .arg(static_cast<int>(std::round(estCaptured))).arg(estimatedFps_, 0, 'f', 0)
        .arg(estKept).arg(gbPerHour, 0, 'f', 2);
    if (saveRawSer_->isChecked())
        note += QString("\nRaw SER bursts are ON: about %1 GB/hr of that.").arg(rawSerGiBPerHour(), 0, 'f', 1);

    const auto preset = static_cast<TimelapsePreset>(preset_->currentData().toInt());
    if (preset == TimelapsePreset::Planet && window_->value() > 90.0)
        note += "\nWarning: >90 s window can smear detail from planet rotation.";
    if (estKept < 10)
        note += "\nNote: fewer than 10 frames kept per stack at this FPS/window.";
    estimateLabel_->setText(note);
}

int TimelapsePanel::estimatedKeptFrames() const
{
    const double estCaptured = std::max(1.0, window_->value() * estimatedFps_);
    return std::clamp(static_cast<int>(std::ceil(estCaptured * keepPercent_->value() / 100.0)),
        1, maxFrames_->value());
}

// Only the kept (stacked) frames of each burst are written to raw SER.
double TimelapsePanel::rawSerGiBPerHour() const
{
    const double stacksPerHour = 3600.0 / std::max(0.5, interval_->value());
    const double burstBytes = static_cast<double>(estimatedKeptFrames()) * frameWidth_ * frameHeight_
        * std::max(1, bytesPerPixel_);
    return stacksPerHour * burstBytes / (1024.0 * 1024.0 * 1024.0);
}

bool TimelapsePanel::confirmRawSer()
{
    const double burstMiB = static_cast<double>(estimatedKeptFrames()) * frameWidth_ * frameHeight_
        * std::max(1, bytesPerPixel_) / (1024.0 * 1024.0);
    QMessageBox box(this);
    box.setObjectName("timelapseRawSerConfirm");
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Save raw SER bursts?");
    box.setText("Raw SER bursts use a lot of disk space and can fill a drive in one night.");
    box.setInformativeText(QString(
        "With the current settings each burst keeps about %1 frames of %2 × %3 (%4 MB), "
        "about %5 GB per hour.\n\n"
        "The time-lapse stops only when free space falls below the disk reserve in the Display panel. "
        "With the reserve set to None it continues until the drive is full.\n\n"
        "This option is off again each time SeleneWatch starts.")
        .arg(estimatedKeptFrames()).arg(frameWidth_).arg(frameHeight_)
        .arg(burstMiB, 0, 'f', 0).arg(rawSerGiBPerHour(), 0, 'f', 1));
    QPushButton* enable = box.addButton("Save raw SER bursts", QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == enable;
}

void TimelapsePanel::updateStatus(const TimelapseStatus& s)
{
    if (!s.sessionDir.isEmpty()) lastSessionDir_ = s.sessionDir;
    QString transit;
    switch (s.transitWatch)
    {
    case TransitWatchState::Off: break;
    case TransitWatchState::Watching: transit = "Watching for transits"; break;
    case TransitWatchState::Saving: transit = "Saving transit clip"; break;
    case TransitWatchState::Capturing: transit = "Transit watch paused · capturing burst"; break;
    case TransitWatchState::Stacking: transit = "Transit watch paused · stacking"; break;
    case TransitWatchState::Finished: transit = "Transit watch ended"; break;
    }
    transitStatus_->setText(transit);
    transitStatus_->setVisible(!transit.isEmpty());

    if (s.finalizing)
    {
        if (s.exportTotal > 0)
        {
            progressBar_->setRange(0, s.exportTotal);
            progressBar_->setValue(std::min(s.exportDone, s.exportTotal));
            timingLabel_->setText(QString("Building the video: %1 / %2 frames…").arg(s.exportDone).arg(s.exportTotal));
        }
        else
        {
            progressBar_->setRange(0, 0);
            timingLabel_->setText("Finalizing MP4/GIF video…");
        }
    }
    else if (s.active)
    {
        if (static_cast<StopConditionKind>(stopKind_->currentData().toInt()) == StopConditionKind::StackCount)
        {
            progressBar_->setRange(0, targetStacks_->value());
            progressBar_->setValue(std::min(s.stacksCompleted, targetStacks_->value()));
            timingLabel_->setText(QString("Elapsed %1 s · %2 / %3 stacks · Next burst in %4 s")
                .arg(s.elapsedSeconds, 0, 'f', 0).arg(s.stacksCompleted).arg(targetStacks_->value()).arg(s.nextBurstInSeconds, 0, 'f', 1));
        }
        else
        {
            progressBar_->setRange(0, 0);
            timingLabel_->setText(QString("Elapsed %1 s · %2 stacks · Next burst in %3 s")
                .arg(s.elapsedSeconds, 0, 'f', 0).arg(s.stacksCompleted).arg(s.nextBurstInSeconds, 0, 'f', 1));
        }
    }

    if (s.stacksCompleted > 0)
    {
        statsLabel_->setText(QString("Stacks: %1 (skipped: %2) · Last: %3/%4 frames · Q=%5 · Shift (%6, %7) px, %8° · %9 s")
            .arg(s.stacksCompleted).arg(s.periodsSkipped).arg(s.lastKeptFrames).arg(s.lastCapturedFrames)
            .arg(s.lastQuality, 0, 'g', 3).arg(s.lastShiftX, 0, 'f', 1).arg(s.lastShiftY, 0, 'f', 1)
            .arg(s.lastRotationDeg, 0, 'f', 2).arg(s.lastProcessingSeconds, 0, 'f', 1));
    }

    if (!s.warningText.isEmpty())
    {
        warningLabel_->setText(s.warningText);
        warningLabel_->show();
    }
    else
    {
        warningLabel_->hide();
    }

    if (s.stacksCompleted > lastSeenStacks_ && !s.latestThumbnail.isNull())
    {
        lastSeenStacks_ = s.stacksCompleted;
        const QPixmap pm = QPixmap::fromImage(s.latestThumbnail)
            .scaled(latestThumb_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        latestThumb_->setPixmap(pm);

        QImage compactThumb = s.latestThumbnail;
        if (std::max(compactThumb.width(), compactThumb.height()) > kFilmstripThumbMaxEdge)
        {
            compactThumb = compactThumb.scaled(kFilmstripThumbMaxEdge, kFilmstripThumbMaxEdge,
                Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (filmstripFrames_.size() >= kMaxFilmstripFrames)
        {
            filmstripFrames_.pop_front();
            if (filmstripIndex_ > 0) --filmstripIndex_;
        }
        filmstripFrames_.push_back(std::move(compactThumb));
        if (filmstripFrames_.size() == 1)
        {
            filmstripThumb_->setPixmap(pm);
        }
        else if (loopFilmstrip_->isChecked() && !filmstripTimer_->isActive())
        {
            filmstripTimer_->start();
        }
        filmstripInfo_->setText(QString("%1 stacks").arg(s.stacksCompleted));
    }
}

void TimelapsePanel::onSessionFinished(const TimelapseFinishResult& result)
{
    filmstripTimer_->stop();
    if (!result.sessionDir.isEmpty()) lastSessionDir_ = result.sessionDir;
    lastTransitFolder_ = result.transitFolder;
    openTransitsButton_->setVisible(!lastTransitFolder_.isEmpty());
    transitStatus_->hide();
    progressBar_->setRange(0, 100);
    progressBar_->setValue(result.stacksCompleted > 0 ? 100 : 0);
    timingLabel_->setText(QString("Finished (%1). %2 stacks, %3 skipped.")
        .arg(result.stopReason).arg(result.stacksCompleted).arg(result.periodsSkipped));
    if (!result.exportResult.error.isEmpty())
    {
        warningLabel_->setText(result.exportResult.error);
        warningLabel_->show();
    }
}

void TimelapsePanel::clearRunVisuals()
{
    filmstripTimer_->stop();
    filmstripFrames_.clear();
    filmstripIndex_ = 0;
    lastSeenStacks_ = 0;
    latestThumb_->setPixmap({});
    latestThumb_->setText("--");
    filmstripThumb_->setPixmap({});
    filmstripThumb_->setText("--");
    filmstripInfo_->setText("0 stacks");
    warningLabel_->hide();
    transitStatus_->hide();
    lastTransitFolder_.clear();
    openTransitsButton_->hide();
    statsLabel_->clear();
    timingLabel_->setText("Running…");
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
}

void TimelapsePanel::updateFilmstripFrame()
{
    if (filmstripFrames_.empty()) return;
    filmstripIndex_ %= filmstripFrames_.size();
    const QPixmap pm = QPixmap::fromImage(filmstripFrames_[filmstripIndex_])
        .scaled(filmstripThumb_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    filmstripThumb_->setPixmap(pm);
    filmstripInfo_->setText(QString("%1 / %2").arg(filmstripIndex_ + 1).arg(filmstripFrames_.size()));
    ++filmstripIndex_;
}

void TimelapsePanel::loadFromQSettings()
{
    QSettings saved;
    saved.beginGroup("timelapse");
    updating_ = true;
    const int p = saved.value("preset", 0).toInt();
    preset_->setCurrentIndex(p == 1 ? 1 : 0);
    interval_->setValue(saved.value("interval", 60.0).toDouble());
    window_->setValue(saved.value("window", 15.0).toDouble());
    keepPercent_->setValue(saved.value("keepPercent", p == 1 ? 15.0 : 25.0).toDouble());
    maxFrames_->setValue(std::clamp(saved.value("maxFrames", p == 1 ? 200 : 150).toInt(), 1, TimelapseSettings::kMaxFramesCap));
    cropToLock_->setChecked(saved.value("cropToLock", false).toBool());
    const QString sharpKey = saved.value("sharpeningKey", "preset:mild").toString();
    const int si = sharpening_->findData(sharpKey);
    if (si >= 0) sharpening_->setCurrentIndex(si);
    // Feature-32: a saved duration or UTC end time is now part of Ends; it reads as "when the session ends".
    stopKind_->setCurrentIndex(std::max(0, stopKind_->findData(saved.value("stopKind", 0).toInt())));
    saved.remove("durationMin");
    targetStacks_->setValue(saved.value("targetStacks", 60).toInt());
    writeMp4_->setChecked(saved.value("writeMp4", true).toBool());
    writeGif_->setChecked(saved.value("writeGif", false).toBool());
    writeSer_->setChecked(saved.value("writeSer", false).toBool());
    stretchSer_->setChecked(saved.value("stretchSer", false).toBool());
    videoFps_->setValue(std::clamp(saved.value("videoFps", 12).toInt(), 1, 60));
    // Raw SER bursts are never remembered: each run asks again (and an older
    // saved choice is dropped).
    saveRawSer_->setChecked(false);
    watchTransits_->setChecked(saved.value("watchTransits", false).toBool());
    saved.remove("saveRawSer");
    saved.endGroup();
    updating_ = false;
}

void TimelapsePanel::saveToQSettings() const
{
    QSettings saved;
    saved.beginGroup("timelapse");
    saved.setValue("preset", preset_->currentData().toInt());
    saved.setValue("interval", interval_->value());
    saved.setValue("window", window_->value());
    saved.setValue("keepPercent", keepPercent_->value());
    saved.setValue("maxFrames", maxFrames_->value());
    saved.setValue("cropToLock", cropToLock_->isChecked());
    saved.setValue("sharpeningKey", sharpening_->currentData().toString());
    saved.setValue("stopKind", stopKind_->currentData().toInt());
    saved.setValue("targetStacks", targetStacks_->value());
    saved.setValue("writeMp4", writeMp4_->isChecked());
    saved.setValue("writeGif", writeGif_->isChecked());
    saved.setValue("writeSer", writeSer_->isChecked());
    saved.setValue("stretchSer", stretchSer_->isChecked());
    saved.setValue("videoFps", videoFps_->value());
    saved.setValue("watchTransits", watchTransits_->isChecked());
    saved.endGroup();
}
