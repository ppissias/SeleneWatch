#include "ui/resharpen_dialog.h"

#include "timelapse/stack_processor.h"
#include "ui/preview_widget.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QWidget* makeSliderRow(QWidget* parent, QDoubleSpinBox* spin, QSlider*& outSlider,
    const QString& sliderObjectName, double sliderMin, double sliderMax, double scale)
{
    auto* row = new QWidget(parent);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    outSlider = new QSlider(Qt::Horizontal, row);
    outSlider->setObjectName(sliderObjectName);
    outSlider->setRange(static_cast<int>(std::lround(sliderMin * scale)),
                        static_cast<int>(std::lround(sliderMax * scale)));
    outSlider->setValue(std::clamp(static_cast<int>(std::lround(spin->value() * scale)),
                                   outSlider->minimum(), outSlider->maximum()));
    spin->setMinimumWidth(84);
    lay->addWidget(outSlider, 1);
    lay->addWidget(spin);

    QObject::connect(outSlider, &QSlider::valueChanged, spin, [spin, scale](int v) {
        spin->setValue(static_cast<double>(v) / scale);
    });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, outSlider, [outSlider, scale](double v) {
        const QSignalBlocker b(outSlider);
        outSlider->setValue(std::clamp(static_cast<int>(std::lround(v * scale)),
                                       outSlider->minimum(), outSlider->maximum()));
    });
    return row;
}

} // namespace

ResharpenDialog::ResharpenDialog(QString timelapsesRoot, const QString& initialSessionDir, QWidget* parent)
    : QDialog(parent)
    , timelapsesRoot_(std::move(timelapsesRoot))
{
    setObjectName("resharpenDialog");
    setWindowTitle("Re-sharpen & rebuild time-lapse");
    resize(980, 680);

    auto* root = new QVBoxLayout(this);

    auto* topRow = new QHBoxLayout();
    sessionLabel_ = new QLabel("No session chosen", this);
    sessionLabel_->setObjectName("resharpenSessionLabel");
    browseBtn_ = new QPushButton("Open session…", this);
    browseBtn_->setObjectName("resharpenBrowse");
    stackCombo_ = new QComboBox(this);
    stackCombo_->setObjectName("resharpenStackCombo");
    topRow->addWidget(sessionLabel_, 1);
    topRow->addWidget(new QLabel("Preview stack:", this));
    topRow->addWidget(stackCombo_);
    topRow->addWidget(browseBtn_);
    root->addLayout(topRow);

    auto* mainRow = new QHBoxLayout();

    // Left: synchronized full-resolution before / after previews with zoom & pan
    auto* previewCol = new QVBoxLayout();
    auto* zoomRow = new QHBoxLayout();
    zoomRow->setContentsMargins(0, 0, 0, 0);
    zoomRow->setSpacing(4);
    auto makeZoomBtn = [&](const QString& text, const QString& objName, const QString& tip) {
        auto* btn = new QToolButton(this);
        btn->setObjectName(objName);
        btn->setText(text);
        btn->setCheckable(true);
        btn->setToolTip(tip);
        zoomRow->addWidget(btn);
        return btn;
    };
    zoomFitBtn_ = makeZoomBtn("Fit", "resharpenZoomFit", "Fit the stack to the preview pane.");
    zoomFitBtn_->setChecked(true);
    zoom100Btn_ = makeZoomBtn("100%", "resharpenZoom100", "Show actual pixels (1:1) to judge sharpening.");
    zoom200Btn_ = makeZoomBtn("200%", "resharpenZoom200", "Zoom to 200% of actual pixels.");
    zoom400Btn_ = makeZoomBtn("400%", "resharpenZoom400", "Zoom to 400% of actual pixels.");
    zoomReadout_ = new QLabel("Fit", this);
    zoomReadout_->setObjectName("resharpenZoomLabel");
    auto* zoomHint = new QLabel("Mouse wheel: zoom · Drag: pan (synced)", this);
    zoomHint->setStyleSheet("color: #888888;");
    zoomRow->addSpacing(6);
    zoomRow->addWidget(zoomReadout_);
    zoomRow->addStretch(1);
    zoomRow->addWidget(zoomHint);
    previewCol->addLayout(zoomRow);

    auto makePreviewView = [](QWidget* parent, const QString& objName) {
        auto* view = new PreviewWidget(parent);
        view->setObjectName(objName);
        view->setDragMode(PreviewWidget::DragMode::Pan);
        view->setPixelSelectionEnabled(false);
        view->setEmptyText("No FITS stacks found");
        view->setMinimumSize(320, 220);
        view->setToolTip("Mouse wheel: zoom · Drag: pan. Before and After stay synchronized.");
        return view;
    };

    auto* beforeBox = new QGroupBox("Before (unsharpened)", this);
    auto* beforeLay = new QVBoxLayout(beforeBox);
    beforeLay->setContentsMargins(4, 4, 4, 4);
    beforeView_ = makePreviewView(beforeBox, "resharpenBefore");
    beforeLay->addWidget(beforeView_);

    auto* afterBox = new QGroupBox("After (sharpened + tone preview)", this);
    auto* afterLay = new QVBoxLayout(afterBox);
    afterLay->setContentsMargins(4, 4, 4, 4);
    afterView_ = makePreviewView(afterBox, "resharpenAfter");
    afterLay->addWidget(afterView_);

    previewCol->addWidget(beforeBox, 1);
    previewCol->addWidget(afterBox, 1);
    mainRow->addLayout(previewCol, 3);

    // Right: controls
    auto* ctrlCol = new QVBoxLayout();

    auto* profileBox = new QGroupBox("Saved profiles", this);
    auto* profLay = new QHBoxLayout(profileBox);
    profileCombo_ = new QComboBox(profileBox);
    profileCombo_->setObjectName("resharpenProfileCombo");
    saveProfileBtn_ = new QPushButton("Save…", profileBox);
    deleteProfileBtn_ = new QPushButton("Delete", profileBox);
    profLay->addWidget(profileCombo_, 1);
    profLay->addWidget(saveProfileBtn_);
    profLay->addWidget(deleteProfileBtn_);
    ctrlCol->addWidget(profileBox);

    auto* sharpBox = new QGroupBox("Linear sharpening (FITS + video)", this);
    auto* sharpForm = new QFormLayout(sharpBox);
    radius_ = new QDoubleSpinBox(sharpBox);
    radius_->setObjectName("resharpenRadius");
    radius_->setRange(0.3, 15.0);
    radius_->setDecimals(2);
    radius_->setSingleStep(0.1);
    radius_->setValue(1.4);
    amount_ = new QDoubleSpinBox(sharpBox);
    amount_->setObjectName("resharpenAmount");
    amount_->setRange(0.0, 6.0);
    amount_->setDecimals(2);
    amount_->setSingleStep(0.1);
    amount_->setValue(0.6);
    threshold_ = new QDoubleSpinBox(sharpBox);
    threshold_->setObjectName("resharpenThreshold");
    threshold_->setRange(0.0, 0.25);
    threshold_->setDecimals(4);
    threshold_->setSingleStep(0.001);
    threshold_->setValue(0.004);
    denoise_ = new QDoubleSpinBox(sharpBox);
    denoise_->setObjectName("resharpenDenoise");
    denoise_->setRange(0.0, 1.0);
    denoise_->setDecimals(2);
    denoise_->setSingleStep(0.05);
    denoise_->setValue(0.0);
    sharpForm->addRow("USM radius (px)", makeSliderRow(sharpBox, radius_, radiusSlider_, "resharpenRadiusSlider", 0.30, 6.00, 100.0));
    sharpForm->addRow("USM amount", makeSliderRow(sharpBox, amount_, amountSlider_, "resharpenAmountSlider", 0.00, 4.00, 100.0));
    sharpForm->addRow("Noise threshold", makeSliderRow(sharpBox, threshold_, thresholdSlider_, "resharpenThresholdSlider", 0.0000, 0.0500, 10000.0));
    sharpForm->addRow("Pre-denoise", makeSliderRow(sharpBox, denoise_, denoiseSlider_, "resharpenDenoiseSlider", 0.00, 1.00, 100.0));
    ctrlCol->addWidget(sharpBox);

    auto* videoBox = new QGroupBox("Video tone-mapping && export (MP4/GIF only)", this);
    auto* videoForm = new QFormLayout(videoBox);
    autoStretch_ = new QCheckBox("Auto stretch black/white", videoBox);
    autoStretch_->setObjectName("resharpenAutoStretch");
    autoStretch_->setChecked(true);
    blackLevel_ = new QDoubleSpinBox(videoBox);
    blackLevel_->setRange(0.0, 0.98);
    blackLevel_->setDecimals(3);
    blackLevel_->setSingleStep(0.01);
    blackLevel_->setValue(0.0);
    whiteLevel_ = new QDoubleSpinBox(videoBox);
    whiteLevel_->setRange(0.02, 1.0);
    whiteLevel_->setDecimals(3);
    whiteLevel_->setSingleStep(0.01);
    whiteLevel_->setValue(1.0);
    gamma_ = new QDoubleSpinBox(videoBox);
    gamma_->setObjectName("resharpenGamma");
    gamma_->setRange(0.2, 3.5);
    gamma_->setDecimals(2);
    gamma_->setSingleStep(0.05);
    gamma_->setValue(1.0);
    brightnessLevel_ = new QComboBox(videoBox);
    brightnessLevel_->setObjectName("resharpenBrightnessLevel");
    brightnessLevel_->addItem("Off", static_cast<int>(BrightnessLevellingMode::Off));
    brightnessLevel_->addItem("Match brightness (global)", static_cast<int>(BrightnessLevellingMode::Global));
    brightnessLevel_->addItem("Match brightness + patchy clouds (Moon)", static_cast<int>(BrightnessLevellingMode::GlobalAndPatchy));
    brightnessLevel_->setCurrentIndex(1);
    brightnessLevel_->setToolTip("Equalizes brightness and haze across stacks against the sharpest clear stack. "
        "Global matches overall brightness and haze while preserving moving lunar shadows and rotating planets; "
        "Patchy clouds also smooths uneven clouds crossing part of an aligned Moon.");
    minQualityPct_ = new QDoubleSpinBox(videoBox);
    minQualityPct_->setRange(0.0, 95.0);
    minQualityPct_->setDecimals(0);
    minQualityPct_->setSuffix(" % of median");
    minQualityPct_->setSpecialValueText("Keep all stacks");
    cropOverlap_ = new QCheckBox("Crop to valid overlap", videoBox);
    cropOverlap_->setObjectName("resharpenCropOverlap");
    cropOverlap_->setChecked(true);
    fps_ = new QSpinBox(videoBox);
    fps_->setObjectName("resharpenFps");
    fps_->setRange(1, 60);
    fps_->setValue(12);
    fps_->setSuffix(" fps");
    auto* fmtRow = new QHBoxLayout();
    writeMp4_ = new QCheckBox("MP4", videoBox);
    writeMp4_->setChecked(true);
    writeGif_ = new QCheckBox("GIF", videoBox);
    fmtRow->addWidget(writeMp4_);
    fmtRow->addWidget(writeGif_);
    rebuildVideoOnly_ = new QCheckBox("Rebuild video only (reuse sharpened/)", videoBox);
    rebuildVideoOnly_->setObjectName("resharpenRebuildOnly");

    videoForm->addRow(autoStretch_);
    videoForm->addRow("Black level", makeSliderRow(videoBox, blackLevel_, blackSlider_, "resharpenBlackSlider", 0.00, 0.98, 1000.0));
    videoForm->addRow("White level", makeSliderRow(videoBox, whiteLevel_, whiteSlider_, "resharpenWhiteSlider", 0.02, 1.00, 1000.0));
    videoForm->addRow("Gamma", makeSliderRow(videoBox, gamma_, gammaSlider_, "resharpenGammaSlider", 0.20, 2.50, 100.0));
    videoForm->addRow("Brightness levelling", brightnessLevel_);
    videoForm->addRow("Drop below quality", minQualityPct_);
    videoForm->addRow(cropOverlap_);
    videoForm->addRow("Frame rate", fps_);
    videoForm->addRow("Formats", fmtRow);
    videoForm->addRow(rebuildVideoOnly_);
    ctrlCol->addWidget(videoBox);

    ctrlCol->addStretch(1);
    mainRow->addLayout(ctrlCol, 2);
    root->addLayout(mainRow, 1);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    root->addWidget(progressBar_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("resharpenStatus");
    root->addWidget(statusLabel_);

    auto* btnRow = new QHBoxLayout();
    applyBtn_ = new QPushButton("Apply to all", this);
    applyBtn_->setObjectName("resharpenApply");
    cancelBtn_ = new QPushButton("Cancel", this);
    cancelBtn_->setEnabled(false);
    openDirBtn_ = new QPushButton("Open output folder", this);
    openDirBtn_->setEnabled(false);
    auto* closeBtn = new QPushButton("Close", this);
    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(cancelBtn_);
    btnRow->addWidget(openDirBtn_);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(zoomFitBtn_, &QToolButton::clicked, this, [this] { beforeView_->setFitToWindow(); });
    connect(zoom100Btn_, &QToolButton::clicked, this, [this] { beforeView_->setZoom(1.0); });
    connect(zoom200Btn_, &QToolButton::clicked, this, [this] { beforeView_->setZoom(2.0); });
    connect(zoom400Btn_, &QToolButton::clicked, this, [this] { beforeView_->setZoom(4.0); });
    connect(beforeView_, &PreviewWidget::viewChanged, this, [this](bool fit, double zoom, QPointF pan) {
        syncPreviewView(afterView_, fit, zoom, pan);
    });
    connect(afterView_, &PreviewWidget::viewChanged, this, [this](bool fit, double zoom, QPointF pan) {
        syncPreviewView(beforeView_, fit, zoom, pan);
    });
    connect(beforeView_, &PreviewWidget::zoomChanged, this, &ResharpenDialog::updateZoomControls);

    connect(browseBtn_, &QPushButton::clicked, this, [this] {
        const QString start = sessionDir_.isEmpty() ? timelapsesRoot_ : sessionDir_;
        const QString chosen = QFileDialog::getExistingDirectory(this, "Open time-lapse session folder", start);
        if (!chosen.isEmpty()) openSession(chosen);
    });
    connect(stackCombo_, &QComboBox::currentIndexChanged, this, [this] { loadSelectedStack(); });

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    connect(previewTimer_, &QTimer::timeout, this, &ResharpenDialog::updatePreview);

    for (auto* sb : {radius_, amount_, threshold_, denoise_, blackLevel_, whiteLevel_, gamma_})
        connect(sb, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) schedulePreviewUpdate(); });
    connect(autoStretch_, &QCheckBox::toggled, this, [this](bool on) {
        blackLevel_->setEnabled(!on);
        blackSlider_->setEnabled(!on);
        whiteLevel_->setEnabled(!on);
        whiteSlider_->setEnabled(!on);
        if (!updating_) updatePreview();
    });
    connect(rebuildVideoOnly_, &QCheckBox::toggled, this, [this](bool only) {
        for (QWidget* w : std::initializer_list<QWidget*>{
                 radius_, radiusSlider_, amount_, amountSlider_,
                 threshold_, thresholdSlider_, denoise_, denoiseSlider_})
        {
            w->setEnabled(!only);
        }
        if (!updating_) updatePreview();
    });
    connect(cropOverlap_, &QCheckBox::toggled, this, [this] { if (!updating_) updatePreview(); });
    connect(brightnessLevel_, &QComboBox::currentIndexChanged, this, [this] { if (!updating_) updatePreview(); });

    connect(profileCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (updating_ || idx < 0) return;
        const QString name = profileCombo_->currentData().toString();
        for (const auto& p : savedProfiles_)
        {
            if (p.name == name)
            {
                setControlsFromSettings(p.sharpening, p.video);
                updatePreview();
                break;
            }
        }
    });
    connect(saveProfileBtn_, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save sharpening profile",
            "Profile name (for example \"Moon - strong\"):", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        SavedSharpeningProfile prof;
        prof.name = name;
        prof.sharpening = currentSharpening();
        prof.sharpening.profileName = name;
        prof.video = currentTone();
        saveNamedSharpeningProfile(prof);
        refreshSavedProfiles();
        const int idx = profileCombo_->findData(name);
        if (idx >= 0) profileCombo_->setCurrentIndex(idx);
        emit profilesChanged();
    });
    connect(deleteProfileBtn_, &QPushButton::clicked, this, [this] {
        const QString name = profileCombo_->currentData().toString();
        if (name.isEmpty()) return;
        removeNamedSharpeningProfile(name);
        refreshSavedProfiles();
        emit profilesChanged();
    });

    connect(applyBtn_, &QPushButton::clicked, this, &ResharpenDialog::onApplyAll);
    connect(cancelBtn_, &QPushButton::clicked, this, [this] { cancelFlag_.store(true); });
    connect(openDirBtn_, &QPushButton::clicked, this, [this] {
        const QString target = lastOutputDir_.isEmpty() ? sessionDir_ : lastOutputDir_;
        if (!target.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    });
    connect(closeBtn, &QPushButton::clicked, this, [this] {
        cancelFlag_.store(true);
        accept();
    });

    refreshSavedProfiles();
    blackLevel_->setEnabled(!autoStretch_->isChecked());
    blackSlider_->setEnabled(!autoStretch_->isChecked());
    whiteLevel_->setEnabled(!autoStretch_->isChecked());
    whiteSlider_->setEnabled(!autoStretch_->isChecked());

    QString startDir = initialSessionDir;
    if (startDir.isEmpty() && !timelapsesRoot_.isEmpty())
    {
        const QDir rootDir(timelapsesRoot_);
        const QStringList sessions = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (!sessions.isEmpty()) startDir = rootDir.filePath(sessions.back());
    }
    if (!startDir.isEmpty()) openSession(startDir);
}

ResharpenDialog::~ResharpenDialog()
{
    cancelFlag_.store(true);
    if (worker_.joinable()) worker_.join();
}

void ResharpenDialog::refreshSavedProfiles()
{
    const QSignalBlocker b(profileCombo_);
    const QString prev = profileCombo_->currentData().toString();
    profileCombo_->clear();
    profileCombo_->addItem("(Choose saved profile…)", QString());
    savedProfiles_ = loadSavedSharpeningProfiles();
    for (const auto& p : savedProfiles_)
        profileCombo_->addItem(p.name, p.name);
    const int idx = profileCombo_->findData(prev);
    profileCombo_->setCurrentIndex(std::max(0, idx));
}

void ResharpenDialog::setControlsFromSettings(const SharpeningSettings& sharp, const VideoToneSettings& tone)
{
    updating_ = true;
    radius_->setValue(sharp.radius);
    amount_->setValue(sharp.amount);
    threshold_->setValue(sharp.threshold);
    denoise_->setValue(sharp.denoise);
    autoStretch_->setChecked(tone.autoStretch);
    blackLevel_->setValue(tone.blackLevel);
    whiteLevel_->setValue(tone.whiteLevel);
    blackLevel_->setEnabled(!tone.autoStretch);
    blackSlider_->setEnabled(!tone.autoStretch);
    whiteLevel_->setEnabled(!tone.autoStretch);
    whiteSlider_->setEnabled(!tone.autoStretch);
    gamma_->setValue(tone.gamma);
    const int bIdx = brightnessLevel_->findData(static_cast<int>(tone.brightnessMode));
    brightnessLevel_->setCurrentIndex(std::max(0, bIdx));
    minQualityPct_->setValue(tone.minQualityRatio * 100.0);
    cropOverlap_->setChecked(tone.cropToOverlap);
    fps_->setValue(tone.fps);
    writeMp4_->setChecked(tone.writeMp4);
    writeGif_->setChecked(tone.writeGif);
    updating_ = false;
}

void ResharpenDialog::openSession(const QString& sessionDir)
{
    sessionDir_ = QDir::cleanPath(sessionDir);
    lastOutputDir_ = sessionDir_;
    sessionLabel_->setText(QDir::toNativeSeparators(sessionDir_));
    openDirBtn_->setEnabled(QDir(sessionDir_).exists());

    sessionOverlap_.reset();
    const QString profFile = QDir(sessionDir_).filePath("sharpening_profile.json");
    if (QFileInfo::exists(profFile))
    {
        SharpeningSettings s;
        VideoToneSettings v;
        QString err;
        if (loadProfileJson(profFile, s, v, err, &sessionOverlap_))
            setControlsFromSettings(s, v);
    }

    stackFiles_.clear();
    bestStackIndex_ = -1;
    bestRaw_.reset();
    bestSharpened_.reset();
    cachedBestApplied_.reset();
    cachedBestAppliedValid_ = false;
    const QSignalBlocker b(stackCombo_);
    stackCombo_->clear();
    const QDir stacksDir(QDir(sessionDir_).filePath("stacks"));
    const QStringList names = stacksDir.entryList({"*.fits", "*.fit"}, QDir::Files | QDir::Readable, QDir::Name);
    std::vector<double> qualities;
    std::vector<double> spans;
    qualities.reserve(names.size());
    spans.reserve(names.size());
    const int sampleStride = std::max(1, static_cast<int>(names.size()) / 120);
    const QRect statsCrop = (sessionOverlap_ && sessionOverlap_->isValid() && !sessionOverlap_->isEmpty())
        ? *sessionOverlap_ : QRect{};
    for (int i = 0; i < names.size(); ++i)
    {
        const QString full = stacksDir.filePath(names[i]);
        stackFiles_.push_back(full);
        FitsMetadata meta;
        int w = 0, h = 0, ch = 0;
        QString err;
        double q = 0.0;
        if (readFitsMetadata(full, meta, w, h, ch, err)) q = meta.qualityScore;
        qualities.push_back(q);
        double span = 0.0;
        if (i % sampleStride == 0)
        {
            FitsImage probe;
            QString pErr;
            if (readFitsFloat(full, probe, pErr))
                span = measureStackBrightnessStats(probe, statsCrop).contrastSpan();
        }
        spans.push_back(span);
        stackCombo_->addItem(QString("%1 (Q=%2)").arg(names[i]).arg(q, 0, 'g', 3));
    }
    if (!stackFiles_.empty())
    {
        const int bestIndex = std::max(0, selectClearReferenceStack(qualities, spans));
        bestStackIndex_ = bestIndex;
        FitsImage bestImg;
        QString bestErr;
        if (readFitsFloat(stackFiles_[static_cast<size_t>(bestIndex)], bestImg, bestErr))
            bestRaw_ = std::move(bestImg);
        const QString bestSharpPath = QDir(QDir(sessionDir_).filePath("sharpened"))
            .filePath(QFileInfo(stackFiles_[static_cast<size_t>(bestIndex)]).fileName());
        if (QFileInfo::exists(bestSharpPath))
        {
            FitsImage bs;
            QString bsErr;
            if (readFitsFloat(bestSharpPath, bs, bsErr)) bestSharpened_ = std::move(bs);
        }

        stackCombo_->setCurrentIndex(bestIndex);
        loadSelectedStack();
        statusLabel_->setText(QString("Loaded %1 stacks (defaulting preview to sharpest clear stack).").arg(stackFiles_.size()));
    }
    else
    {
        loadedRaw_.reset();
        loadedSharpened_.reset();
        beforeView_->clear();
        afterView_->clear();
        statusLabel_->setText("No stacks/ FITS files found in this folder.");
    }
    applyBtn_->setEnabled(!stackFiles_.empty());
}

void ResharpenDialog::loadSelectedStack()
{
    const int idx = stackCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(stackFiles_.size())) return;
    const QString& path = stackFiles_[static_cast<size_t>(idx)];
    if (idx == bestStackIndex_ && bestRaw_)
    {
        loadedRaw_ = bestRaw_;
    }
    else
    {
        FitsImage img;
        QString err;
        if (!readFitsFloat(path, img, err))
        {
            statusLabel_->setText(err);
            return;
        }
        loadedRaw_ = std::move(img);
    }

    loadedSharpened_.reset();
    if (idx == bestStackIndex_ && bestSharpened_)
    {
        loadedSharpened_ = bestSharpened_;
    }
    else
    {
        const QString sharpPath = QDir(QDir(sessionDir_).filePath("sharpened"))
            .filePath(QFileInfo(path).fileName());
        if (QFileInfo::exists(sharpPath))
        {
            FitsImage sImg;
            QString sErr;
            if (readFitsFloat(sharpPath, sImg, sErr))
                loadedSharpened_ = std::move(sImg);
        }
    }

    cachedBeforeImg_ = {};
    updatePreview();
}

SharpeningSettings ResharpenDialog::currentSharpening() const
{
    SharpeningSettings s;
    s.preset = amount_->value() <= 0.0 ? SharpeningPreset::Off : SharpeningPreset::Custom;
    s.radius = radius_->value();
    s.amount = amount_->value();
    s.threshold = threshold_->value();
    s.denoise = denoise_->value();
    const QString sel = profileCombo_->currentData().toString();
    s.profileName = sel.isEmpty() ? QString("Custom") : sel;
    return s;
}

VideoToneSettings ResharpenDialog::currentTone() const
{
    VideoToneSettings v;
    v.autoStretch = autoStretch_->isChecked();
    v.blackLevel = blackLevel_->value();
    v.whiteLevel = std::max(v.blackLevel + 0.01, whiteLevel_->value());
    v.gamma = gamma_->value();
    v.brightnessMode = static_cast<BrightnessLevellingMode>(brightnessLevel_->currentData().toInt());
    v.minQualityRatio = minQualityPct_->value() / 100.0;
    v.cropToOverlap = cropOverlap_->isChecked();
    v.fps = fps_->value();
    v.writeMp4 = writeMp4_->isChecked();
    v.writeGif = writeGif_->isChecked();
    return v;
}

void ResharpenDialog::schedulePreviewUpdate()
{
    if (previewTimer_) previewTimer_->start(25);
}

void ResharpenDialog::updatePreview()
{
    if (!loadedRaw_) return;
    if (previewTimer_ && previewTimer_->isActive()) previewTimer_->stop();

    const VideoToneSettings tone = currentTone();
    const SharpeningSettings sharp = currentSharpening();
    const bool rebuildOnly = rebuildVideoOnly_->isChecked();

    // Match exportTimelapseFromFits overlap cropping (even dimensions).
    QRect crop;
    if (tone.cropToOverlap && sessionOverlap_ && sessionOverlap_->isValid() && !sessionOverlap_->isEmpty())
    {
        const QRect clipped = sessionOverlap_->intersected(QRect(0, 0, loadedRaw_->width, loadedRaw_->height));
        if (clipped.width() >= 2 && clipped.height() >= 2)
        {
            crop = clipped;
            crop.setWidth(std::max(2, crop.width() & ~1));
            crop.setHeight(std::max(2, crop.height() & ~1));
        }
    }

    // Build the exact linear After stack (reusing sharpened/ when Rebuild video only is checked).
    FitsImage computedAfter;
    const FitsImage* afterFits = nullptr;
    if (rebuildOnly)
    {
        afterFits = loadedSharpened_ ? &*loadedSharpened_ : &*loadedRaw_;
    }
    else
    {
        computedAfter = applyLinearSharpening(*loadedRaw_, sharp);
        afterFits = &computedAfter;
    }

    // Use the sharpened highest-quality stack as the reference for auto-stretch and
    // brightness levelling, matching exportTimelapseFromFits.
    const bool viewingBest = (stackCombo_->currentIndex() == bestStackIndex_) || !bestRaw_;
    const FitsImage* refFits = afterFits;
    if (!viewingBest)
    {
        if (rebuildOnly)
        {
            refFits = bestSharpened_ ? &*bestSharpened_ : &*bestRaw_;
        }
        else
        {
            if (!cachedBestAppliedValid_ || cachedBestSharpSettings_ != sharp)
            {
                cachedBestApplied_ = applyLinearSharpening(*bestRaw_, sharp);
                cachedBestSharpSettings_ = sharp;
                cachedBestAppliedValid_ = true;
            }
            refFits = &*cachedBestApplied_;
        }
    }

    double black = tone.blackLevel;
    double white = tone.whiteLevel;
    if (tone.autoStretch)
        computeAutoStretchLevels(*refFits, black, white, crop);

    const bool alignedPixels = refFits->meta.targetPreset != "planet";

    if (cachedBeforeImg_.isNull() || cachedBeforeBlack_ != black
        || cachedBeforeWhite_ != white || cachedBeforeGamma_ != tone.gamma
        || cachedBeforeCrop_ != crop)
    {
        cachedBeforeImg_ = toneMapFitsToImage(*loadedRaw_, black, white, tone.gamma, 1.0, crop, 0);
        cachedBeforeBlack_ = black;
        cachedBeforeWhite_ = white;
        cachedBeforeGamma_ = tone.gamma;
        cachedBeforeCrop_ = crop;
        beforeView_->setImage(cachedBeforeImg_);
    }

    const QImage after = toneMapFitsToImage(*afterFits, black, white, tone.gamma, 1.0, crop, 0,
        refFits, tone.brightnessMode, alignedPixels);
    afterView_->setImage(after);
}

void ResharpenDialog::syncPreviewView(PreviewWidget* target, bool fitToWindow, double zoom, QPointF pan)
{
    if (syncingView_) return;
    syncingView_ = true;
    if (fitToWindow)
        target->setFitToWindow();
    else
        target->setZoomAndPan(zoom, pan);
    updateZoomControls(zoom, fitToWindow);
    syncingView_ = false;
}

void ResharpenDialog::updateZoomControls(double effectiveZoom, bool fitToWindow)
{
    if (!zoomFitBtn_ || !zoom100Btn_ || !zoom200Btn_ || !zoom400Btn_ || !zoomReadout_) return;
    const QSignalBlocker bFit(zoomFitBtn_);
    const QSignalBlocker b100(zoom100Btn_);
    const QSignalBlocker b200(zoom200Btn_);
    const QSignalBlocker b400(zoom400Btn_);
    zoomFitBtn_->setChecked(fitToWindow);
    zoom100Btn_->setChecked(!fitToWindow && std::abs(effectiveZoom - 1.0) < 0.01);
    zoom200Btn_->setChecked(!fitToWindow && std::abs(effectiveZoom - 2.0) < 0.01);
    zoom400Btn_->setChecked(!fitToWindow && std::abs(effectiveZoom - 4.0) < 0.01);
    const int pct = std::max(1, static_cast<int>(std::lround(effectiveZoom * 100.0)));
    zoomReadout_->setText(fitToWindow ? QString("Fit (%1%)").arg(pct) : QString("%1%").arg(pct));
}

void ResharpenDialog::setBusy(bool busy)
{
    applyBtn_->setEnabled(!busy && !stackFiles_.empty());
    browseBtn_->setEnabled(!busy);
    stackCombo_->setEnabled(!busy);
    profileCombo_->setEnabled(!busy);
    saveProfileBtn_->setEnabled(!busy);
    deleteProfileBtn_->setEnabled(!busy);
    cancelBtn_->setEnabled(busy);
}

void ResharpenDialog::onApplyAll()
{
    if (sessionDir_.isEmpty() || stackFiles_.empty()) return;
    if (worker_.joinable()) worker_.join();

    cancelFlag_.store(false);
    setBusy(true);
    progressBar_->setValue(0);
    statusLabel_->setText("Starting…");

    const QString dir = sessionDir_;
    const SharpeningSettings sharp = currentSharpening();
    const VideoToneSettings tone = currentTone();
    const bool rebuildOnly = rebuildVideoOnly_->isChecked();

    worker_ = std::jthread([this, dir, sharp, tone, rebuildOnly] {
        QString outDir;
        TimelapseExportResult res;
        // This thread's exception boundary: a failure ends this run with a
        // message instead of terminating the program.
        try
        {
            res = resharpenTimelapseSession(
                dir, {}, sharp, tone, rebuildOnly, outDir,
                [this](int done, int total, const QString& stage) {
                    QMetaObject::invokeMethod(this, [this, done, total, stage] {
                        progressBar_->setRange(0, std::max(1, total));
                        progressBar_->setValue(done);
                        statusLabel_->setText(QString("%1 (%2/%3)").arg(stage).arg(done).arg(total));
                    }, Qt::QueuedConnection);
                },
                &cancelFlag_);
        }
        catch (const std::exception& ex)
        {
            res = {};
            res.error = QString("internal error: %1").arg(QString::fromUtf8(ex.what()).trimmed());
        }
        catch (...)
        {
            res = {};
            res.error = "internal error";
        }

        QMetaObject::invokeMethod(this, [this, res, outDir] {
            setBusy(false);
            if (!outDir.isEmpty()) lastOutputDir_ = outDir;
            openDirBtn_->setEnabled(!lastOutputDir_.isEmpty());
            if (res.ok)
            {
                progressBar_->setValue(progressBar_->maximum());
                statusLabel_->setText(QString("Done: %1 stacks encoded to %2.")
                    .arg(res.stacksUsed).arg(QDir::toNativeSeparators(lastOutputDir_)));
            }
            else
            {
                statusLabel_->setText(QString("Failed: %1").arg(res.error));
            }
        }, Qt::QueuedConnection);
    });
}
