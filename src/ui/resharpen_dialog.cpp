#include "ui/resharpen_dialog.h"

#include "timelapse/stack_processor.h"
#include "ui/preview_widget.h"
#include "ui/widgets/control_help.h"

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
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
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
    outSlider->setToolTip(spin->toolTip());
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
    stackCombo_->setToolTip("Choose which saved stack to preview. Defaults to the sharpest clear stack (used as the "
        "reference for brightness levelling).");
    browseBtn_->setToolTip("Open a different finished time-lapse session folder.");
    auto* previewStackLabel = new QLabel("Preview stack:", this);
    previewStackLabel->setBuddy(stackCombo_);
    topRow->addWidget(sessionLabel_, 1);
    topRow->addWidget(previewStackLabel);
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

    stackListBox_ = new QGroupBox("Stack quality filter inspector (click a stack to preview; tick/untick to override)", this);
    stackListBox_->setObjectName("resharpenStackListBox");
    auto* stackListLay = new QVBoxLayout(stackListBox_);
    stackListLay->setContentsMargins(6, 6, 6, 6);
    stackTable_ = new QTableWidget(0, 5, stackListBox_);
    stackTable_->setObjectName("resharpenStackTable");
    stackTable_->setHorizontalHeaderLabels({"Keep", "Stack", "Quality", "Reference", "Status"});
    stackTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stackTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stackTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stackTable_->verticalHeader()->setVisible(false);
    stackTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    stackTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    stackTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    stackTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    stackTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    stackTable_->setMaximumHeight(175);
    stackTable_->setToolTip("Every stack in the session with its sharpness score and keep/drop status. Click any row "
        "to inspect that frame in the Before/After preview, or toggle Keep to manually override the quality filter.");
    stackListLay->addWidget(stackTable_);
    stackListBox_->hide();
    previewCol->addWidget(stackListBox_);
    mainRow->addLayout(previewCol, 3);

    // Right: controls
    auto* ctrlCol = new QVBoxLayout();

    auto* profileBox = new QGroupBox("Saved profiles", this);
    auto* profLay = new QHBoxLayout(profileBox);
    profileCombo_ = new QComboBox(profileBox);
    profileCombo_->setObjectName("resharpenProfileCombo");
    profileCombo_->setToolTip("Load a saved sharpening and video tone-mapping profile.");
    saveProfileBtn_ = new QPushButton("Save…", profileBox);
    saveProfileBtn_->setToolTip("Save the current sharpening and video tone-mapping settings as a reusable profile.");
    deleteProfileBtn_ = new QPushButton("Delete", profileBox);
    deleteProfileBtn_->setToolTip("Delete the currently selected saved profile.");
    profLay->addWidget(profileCombo_, 1);
    profLay->addWidget(saveProfileBtn_);
    profLay->addWidget(deleteProfileBtn_);
    ctrlCol->addWidget(profileBox);

    auto* sharpBox = new QGroupBox("Linear sharpening (FITS + video)", this);
    auto* sharpForm = new QFormLayout(sharpBox);
    radius_ = new QDoubleSpinBox(sharpBox);
    radius_->setObjectName("resharpenRadius");
    radius_->setToolTip("Size of details to sharpen (Gaussian blur radius in pixels). Small values (1–2 px) crisp "
        "fine crater edges; larger values (2.5–5 px) boost broader lunar or planetary structure.");
    radius_->setRange(0.3, 15.0);
    radius_->setDecimals(2);
    radius_->setSingleStep(0.1);
    radius_->setValue(1.4);
    amount_ = new QDoubleSpinBox(sharpBox);
    amount_->setObjectName("resharpenAmount");
    amount_->setToolTip("Strength of unsharp-mask sharpening (0 = off). Higher values increase edge contrast, while "
        "too much creates bright/dark halos and amplifies grain. Zoom to 100% or 200% to judge accurately.");
    amount_->setRange(0.0, 6.0);
    amount_->setDecimals(2);
    amount_->setSingleStep(0.1);
    amount_->setValue(0.6);
    threshold_ = new QDoubleSpinBox(sharpBox);
    threshold_->setObjectName("resharpenThreshold");
    threshold_->setToolTip("Ignores tiny brightness differences below this level (on a 0–1 scale) so sharpening "
        "enhances real features instead of background noise. Set to 0 to sharpen all detail; values above ~0.01 "
        "suppress most fine texture.");
    threshold_->setRange(0.0, 0.25);
    threshold_->setDecimals(4);
    threshold_->setSingleStep(0.001);
    threshold_->setValue(0.004);
    denoise_ = new QDoubleSpinBox(sharpBox);
    denoise_->setObjectName("resharpenDenoise");
    denoise_->setToolTip("Gentle edge-preserving smoothing applied before sharpening to reduce grain without blurring "
        "sharp crater rims (0 = off).");
    denoise_->setRange(0.0, 1.0);
    denoise_->setDecimals(2);
    denoise_->setSingleStep(0.05);
    denoise_->setValue(0.0);
    sharpForm->addRow("USM radius (px)", makeSliderRow(sharpBox, radius_, radiusSlider_, "resharpenRadiusSlider", 0.30, 6.00, 100.0));
    sharpForm->addRow("USM amount", makeSliderRow(sharpBox, amount_, amountSlider_, "resharpenAmountSlider", 0.00, 4.00, 100.0));
    sharpForm->addRow("Noise threshold", makeSliderRow(sharpBox, threshold_, thresholdSlider_, "resharpenThresholdSlider", 0.0000, 0.0500, 10000.0));
    sharpForm->addRow("Pre-denoise", makeSliderRow(sharpBox, denoise_, denoiseSlider_, "resharpenDenoiseSlider", 0.00, 1.00, 100.0));
    ctrlCol->addWidget(sharpBox);

    auto checkboxRowWithHelp = [](QCheckBox* cb, QWidget* parent) {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        row->addWidget(cb);
        row->addWidget(makeHelpIcon(cb, parent));
        row->addStretch(1);
        return row;
    };

    auto* videoBox = new QGroupBox("Video tone-mapping && export (MP4/GIF only)", this);
    auto* videoForm = new QFormLayout(videoBox);
    autoStretch_ = new QCheckBox("Auto stretch black/white", videoBox);
    autoStretch_->setObjectName("resharpenAutoStretch");
    autoStretch_->setToolTip("Automatically chooses black and white display levels for the video from the sharpest "
        "clear stack. Uncheck to adjust Black level and White level manually.");
    autoStretch_->setChecked(true);
    blackLevel_ = new QDoubleSpinBox(videoBox);
    blackLevel_->setToolTip("Normalized pixel brightness mapped to pure black in the exported video (used when Auto "
        "stretch is off).");
    blackLevel_->setRange(0.0, 0.98);
    blackLevel_->setDecimals(3);
    blackLevel_->setSingleStep(0.01);
    blackLevel_->setValue(0.0);
    whiteLevel_ = new QDoubleSpinBox(videoBox);
    whiteLevel_->setToolTip("Normalized pixel brightness mapped to pure white in the exported video (used when Auto "
        "stretch is off).");
    whiteLevel_->setRange(0.02, 1.0);
    whiteLevel_->setDecimals(3);
    whiteLevel_->setSingleStep(0.01);
    whiteLevel_->setValue(1.0);
    gamma_ = new QDoubleSpinBox(videoBox);
    gamma_->setObjectName("resharpenGamma");
    gamma_->setToolTip("Midtone brightness curve for the exported video (1.0 = linear; above 1.0 brightens shadows "
        "and midtones, below 1.0 darkens them).");
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
        "Patchy clouds also smooths uneven clouds crossing part of an aligned Moon. Pick a non-reference stack in "
        "Preview stack to see its effect here.");
    qualityMode_ = new QComboBox(videoBox);
    qualityMode_->setObjectName("resharpenQualityMode");
    qualityMode_->addItem("Off (keep all stacks)", -1);
    qualityMode_->addItem("Local rolling median (best for long runs)", static_cast<int>(QualityFilterMode::LocalMedian));
    qualityMode_->addItem("Session median (global)", static_cast<int>(QualityFilterMode::GlobalMedian));
    qualityMode_->addItem("Keep sharpest %", static_cast<int>(QualityFilterMode::KeepBestPct));
    qualityMode_->setToolTip("How blurry or cloudy stacks are excluded before building the video. Local rolling "
        "median compares each stack to its surrounding neighbors (avoids dropping good frames when altitude or lunar "
        "shadows drift slowly over hours). Session median compares to the whole night. Keep sharpest % keeps the top "
        "fraction overall. Use Inspect stacks or Dropped buttons below to view every dropped frame before exporting.");
    minQualityPct_ = new QDoubleSpinBox(videoBox);
    minQualityPct_->setObjectName("resharpenMinQuality");
    minQualityPct_->setToolTip("Drops stacks whose sharpness score is below this percentage of the local or session "
        "median quality.");
    minQualityPct_->setRange(1.0, 95.0);
    minQualityPct_->setDecimals(0);
    minQualityPct_->setValue(75.0);
    minQualityPct_->setSuffix(" % of nearby median");
    keepBestPct_ = new QDoubleSpinBox(videoBox);
    keepBestPct_->setObjectName("resharpenKeepBestPct");
    keepBestPct_->setToolTip("Keeps only this percentage of the sharpest stacks in the session.");
    keepBestPct_->setRange(5.0, 100.0);
    keepBestPct_->setDecimals(0);
    keepBestPct_->setValue(80.0);
    keepBestPct_->setSuffix(" % sharpest");
    auto* qualThreshRow = new QHBoxLayout();
    qualThreshRow->setContentsMargins(0, 0, 0, 0);
    qualThreshRow->addWidget(minQualityPct_);
    qualThreshRow->addWidget(keepBestPct_);
    qualThreshRow->addStretch(1);

    auto* qualInspectCol = new QVBoxLayout();
    qualInspectCol->setContentsMargins(0, 0, 0, 0);
    qualInspectCol->setSpacing(4);
    qualitySummary_ = new QLabel("No stacks loaded", videoBox);
    qualitySummary_->setObjectName("resharpenQualitySummary");
    auto* qualBtnRow = new QHBoxLayout();
    qualBtnRow->setContentsMargins(0, 0, 0, 0);
    qualBtnRow->setSpacing(4);
    prevDroppedBtn_ = new QToolButton(videoBox);
    prevDroppedBtn_->setObjectName("resharpenPrevDropped");
    prevDroppedBtn_->setText("◀ Dropped");
    prevDroppedBtn_->setToolTip("Jump the Before/After preview to the previous dropped stack.");
    prevDroppedBtn_->setEnabled(false);
    nextDroppedBtn_ = new QToolButton(videoBox);
    nextDroppedBtn_->setObjectName("resharpenNextDropped");
    nextDroppedBtn_->setText("Dropped ▶");
    nextDroppedBtn_->setToolTip("Jump the Before/After preview to the next dropped stack.");
    nextDroppedBtn_->setEnabled(false);
    inspectStacksBtn_ = new QToolButton(videoBox);
    inspectStacksBtn_->setObjectName("resharpenInspectStacks");
    inspectStacksBtn_->setText("Inspect stacks");
    inspectStacksBtn_->setCheckable(true);
    inspectStacksBtn_->setToolTip("Show the full stack quality list so you can inspect dropped frames and manually "
        "keep or drop individual stacks.");
    resetOverridesBtn_ = new QToolButton(videoBox);
    resetOverridesBtn_->setObjectName("resharpenResetOverrides");
    resetOverridesBtn_->setText("Reset");
    resetOverridesBtn_->setToolTip("Clear any manual per-stack keep/drop overrides.");
    resetOverridesBtn_->setEnabled(false);
    qualBtnRow->addWidget(prevDroppedBtn_);
    qualBtnRow->addWidget(nextDroppedBtn_);
    qualBtnRow->addWidget(inspectStacksBtn_);
    qualBtnRow->addWidget(resetOverridesBtn_);
    qualBtnRow->addStretch(1);
    qualInspectCol->addWidget(qualitySummary_);
    qualInspectCol->addLayout(qualBtnRow);
    cropOverlap_ = new QCheckBox("Crop to valid overlap", videoBox);
    cropOverlap_->setObjectName("resharpenCropOverlap");
    cropOverlap_->setToolTip("Crops the video to the region covered by every stack so drifting black borders do not appear.");
    cropOverlap_->setChecked(true);
    fps_ = new QSpinBox(videoBox);
    fps_->setObjectName("resharpenFps");
    fps_->setToolTip("Playback frame rate of the exported MP4/GIF video (applies on export, not preview).");
    fps_->setRange(1, 60);
    fps_->setValue(12);
    fps_->setSuffix(" fps");
    auto* fmtRow = new QHBoxLayout();
    writeMp4_ = new QCheckBox("MP4", videoBox);
    writeMp4_->setObjectName("resharpenWriteMp4");
    writeMp4_->setToolTip("Export an H.264 MP4 video.");
    writeMp4_->setChecked(true);
    writeGif_ = new QCheckBox("GIF", videoBox);
    writeGif_->setObjectName("resharpenWriteGif");
    writeGif_->setToolTip("Also export a looping animated GIF.");
    writeSer_ = new QCheckBox("SER", videoBox);
    writeSer_->setObjectName("resharpenWriteSer");
    writeSer_->setToolTip("Also export timelapse.ser (one lossless frame per kept stack with UTC timestamps) for "
        "SER Player or further processing.");
    fmtRow->addWidget(writeMp4_);
    fmtRow->addWidget(writeGif_);
    fmtRow->addWidget(writeSer_);
    stretchSerRow_ = new QWidget(videoBox);
    auto* stretchSerLay = checkboxRowWithHelp(
        (stretchSer_ = new QCheckBox("Stretch SER (match MP4)", stretchSerRow_)), stretchSerRow_);
    stretchSer_->setObjectName("resharpenStretchSer");
    stretchSer_->setToolTip("Off (default) writes unclipped linear 16-bit mono / RGB stacks to timelapse.ser for "
        "further processing in SER Player. On applies the same black/white stretch and gamma as timelapse.mp4.");
    stretchSerRow_->setLayout(stretchSerLay);
    stretchSerRow_->hide();
    rebuildVideoOnly_ = new QCheckBox("Rebuild video only (reuse sharpened/)", videoBox);
    rebuildVideoOnly_->setObjectName("resharpenRebuildOnly");
    rebuildVideoOnly_->setToolTip("Re-encodes the video directly from the existing sharpened/ FITS stacks without "
        "re-running sharpening.");

    videoForm->addRow(checkboxRowWithHelp(autoStretch_, videoBox));
    videoForm->addRow("Black level", makeSliderRow(videoBox, blackLevel_, blackSlider_, "resharpenBlackSlider", 0.00, 0.98, 1000.0));
    videoForm->addRow("White level", makeSliderRow(videoBox, whiteLevel_, whiteSlider_, "resharpenWhiteSlider", 0.02, 1.00, 1000.0));
    videoForm->addRow("Gamma", makeSliderRow(videoBox, gamma_, gammaSlider_, "resharpenGammaSlider", 0.20, 2.50, 100.0));
    videoForm->addRow("Brightness levelling", brightnessLevel_);
    videoForm->addRow("Quality filter", qualityMode_);
    videoForm->addRow("Filter threshold", qualThreshRow);
    videoForm->addRow(qualInspectCol);
    videoForm->addRow(checkboxRowWithHelp(cropOverlap_, videoBox));
    videoForm->addRow("Frame rate", fps_);
    videoForm->addRow("Formats", fmtRow);
    videoForm->addRow(stretchSerRow_);
    videoForm->addRow(checkboxRowWithHelp(rebuildVideoOnly_, videoBox));
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
    connect(writeSer_, &QCheckBox::toggled, this, [this](bool on) { stretchSerRow_->setVisible(on); });
    connect(brightnessLevel_, &QComboBox::currentIndexChanged, this, [this] { if (!updating_) updatePreview(); });
    connect(qualityMode_, &QComboBox::currentIndexChanged, this, [this] {
        updateQualityFilterControls();
        if (!updating_) refreshQualityFilter();
    });
    connect(minQualityPct_, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) refreshQualityFilter(); });
    connect(keepBestPct_, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) refreshQualityFilter(); });
    connect(prevDroppedBtn_, &QToolButton::clicked, this, [this] { stepDroppedStack(-1); });
    connect(nextDroppedBtn_, &QToolButton::clicked, this, [this] { stepDroppedStack(1); });
    connect(inspectStacksBtn_, &QToolButton::toggled, this, [this](bool on) { stackListBox_->setVisible(on); });
    connect(resetOverridesBtn_, &QToolButton::clicked, this, [this] {
        std::fill(stackManualOverrides_.begin(), stackManualOverrides_.end(), std::nullopt);
        refreshQualityFilter();
    });
    connect(stackTable_, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row >= 0 && row < stackCombo_->count() && stackCombo_->currentIndex() != row)
            stackCombo_->setCurrentIndex(row);
    });
    connect(stackTable_, &QTableWidget::itemChanged, this, &ResharpenDialog::onStackTableItemChanged);

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
    updateQualityFilterControls();
    installControlHelp(this);

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
    const bool filterActive = (tone.qualityFilterMode == QualityFilterMode::KeepBestPct)
        ? (tone.keepBestPercent < 100.0)
        : (tone.minQualityRatio > 0.0);
    if (!filterActive)
    {
        qualityMode_->setCurrentIndex(0);
    }
    else
    {
        const int qIdx = qualityMode_->findData(static_cast<int>(tone.qualityFilterMode));
        qualityMode_->setCurrentIndex(std::max(0, qIdx));
    }
    if (tone.minQualityRatio > 0.0) minQualityPct_->setValue(tone.minQualityRatio * 100.0);
    if (tone.keepBestPercent < 100.0) keepBestPct_->setValue(tone.keepBestPercent);
    updateQualityFilterControls();
    cropOverlap_->setChecked(tone.cropToOverlap);
    fps_->setValue(tone.fps);
    writeMp4_->setChecked(tone.writeMp4);
    writeGif_->setChecked(tone.writeGif);
    writeSer_->setChecked(tone.writeSer);
    stretchSer_->setChecked(tone.stretchSer);
    stretchSerRow_->setVisible(tone.writeSer);
    updating_ = false;
    refreshQualityFilter();
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
    stackQualities_.clear();
    stackManualOverrides_.clear();
    filterDecisions_.clear();
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
    stackQualities_ = qualities;
    stackManualOverrides_.assign(stackFiles_.size(), std::nullopt);
    refreshQualityFilter();
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
    if (stackTable_ && idx < stackTable_->rowCount())
    {
        const QSignalBlocker bt(stackTable_);
        stackTable_->selectRow(idx);
    }
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
    const int qMode = qualityMode_->currentData().toInt();
    if (qMode < 0)
    {
        v.qualityFilterMode = QualityFilterMode::LocalMedian;
        v.minQualityRatio = 0.0;
        v.keepBestPercent = 100.0;
    }
    else
    {
        v.qualityFilterMode = static_cast<QualityFilterMode>(qMode);
        if (v.qualityFilterMode == QualityFilterMode::KeepBestPct)
        {
            v.minQualityRatio = 0.0;
            v.keepBestPercent = keepBestPct_->value();
        }
        else
        {
            v.minQualityRatio = minQualityPct_->value() / 100.0;
            v.keepBestPercent = 100.0;
        }
    }
    v.cropToOverlap = cropOverlap_->isChecked();
    v.fps = fps_->value();
    v.writeMp4 = writeMp4_->isChecked();
    v.writeGif = writeGif_->isChecked();
    v.writeSer = writeSer_->isChecked();
    v.stretchSer = stretchSer_->isChecked();
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

void ResharpenDialog::updateQualityFilterControls()
{
    if (!qualityMode_ || !minQualityPct_ || !keepBestPct_) return;
    const int mode = qualityMode_->currentData().toInt();
    const bool isOff = (mode < 0);
    const bool isKeepBest = (!isOff && static_cast<QualityFilterMode>(mode) == QualityFilterMode::KeepBestPct);
    const bool isMedian = (!isOff && !isKeepBest);
    minQualityPct_->setVisible(isMedian);
    minQualityPct_->setEnabled(isMedian);
    if (isMedian)
    {
        minQualityPct_->setSuffix(static_cast<QualityFilterMode>(mode) == QualityFilterMode::LocalMedian
            ? " % of nearby median" : " % of session median");
    }
    keepBestPct_->setVisible(isKeepBest);
    keepBestPct_->setEnabled(isKeepBest);
}

void ResharpenDialog::refreshQualityFilter()
{
    if (!qualitySummary_ || !stackTable_ || !stackCombo_) return;
    const size_t n = stackFiles_.size();
    if (n == 0)
    {
        filterDecisions_.clear();
        qualitySummary_->setText("No stacks loaded");
        prevDroppedBtn_->setEnabled(false);
        nextDroppedBtn_->setEnabled(false);
        resetOverridesBtn_->setEnabled(false);
        updatingTable_ = true;
        stackTable_->setRowCount(0);
        updatingTable_ = false;
        return;
    }

    filterDecisions_ = evaluateStackQualityFilter(stackQualities_, currentTone(), stackManualOverrides_);
    int keptCount = 0;
    int droppedCount = 0;
    int overrideCount = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (filterDecisions_[i].kept) ++keptCount;
        else ++droppedCount;
        if (stackManualOverrides_[i].has_value()) ++overrideCount;
    }

    QString summary = droppedCount == 0
        ? QString("All %1 stacks kept").arg(n)
        : QString("Keeps %1 of %2 stacks · drops %3").arg(keptCount).arg(n).arg(droppedCount);
    if (overrideCount > 0)
        summary += QString(" (%1 manual)").arg(overrideCount);
    qualitySummary_->setText(summary);
    prevDroppedBtn_->setEnabled(droppedCount > 0);
    nextDroppedBtn_->setEnabled(droppedCount > 0);
    resetOverridesBtn_->setEnabled(overrideCount > 0);

    // Update combo box prefixes so dropped stacks stand out even when the table is hidden.
    {
        const QSignalBlocker bc(stackCombo_);
        for (size_t i = 0; i < n && static_cast<int>(i) < stackCombo_->count(); ++i)
        {
            const QString base = QFileInfo(stackFiles_[i]).fileName();
            const QString prefix = filterDecisions_[i].kept ? QString() : QString("[DROP] ");
            stackCombo_->setItemText(static_cast<int>(i),
                QString("%1%2 (Q=%3)").arg(prefix, base).arg(stackQualities_[i], 0, 'g', 3));
        }
    }

    // Rebuild / update the stack inspector table.
    updatingTable_ = true;
    stackTable_->setRowCount(static_cast<int>(n));
    const QBrush droppedBg(QColor(130, 38, 38, 70));
    const QBrush clearBg;
    for (size_t i = 0; i < n; ++i)
    {
        const int row = static_cast<int>(i);
        const StackFilterDecision& d = filterDecisions_[i];
        auto* checkItem = stackTable_->item(row, 0);
        if (!checkItem)
        {
            checkItem = new QTableWidgetItem();
            checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            stackTable_->setItem(row, 0, checkItem);
        }
        checkItem->setCheckState(d.kept ? Qt::Checked : Qt::Unchecked);

        auto setTextCell = [&](int col, const QString& text) {
            auto* item = stackTable_->item(row, col);
            if (!item)
            {
                item = new QTableWidgetItem();
                stackTable_->setItem(row, col, item);
            }
            item->setText(text);
            item->setBackground(d.kept ? clearBg : droppedBg);
        };
        checkItem->setBackground(d.kept ? clearBg : droppedBg);
        setTextCell(1, QFileInfo(stackFiles_[i]).fileName());
        setTextCell(2, QString::number(stackQualities_[i], 'g', 4));
        setTextCell(3, d.referenceQuality > 0.0 ? QString::number(d.referenceQuality, 'g', 4) : QString("--"));
        const QString status = d.overridden
            ? (d.kept ? "Kept (manual)" : "Dropped (manual)")
            : (d.kept ? "Kept" : "Dropped");
        setTextCell(4, status);
    }
    const int cur = stackCombo_->currentIndex();
    if (cur >= 0 && cur < stackTable_->rowCount()) stackTable_->selectRow(cur);
    updatingTable_ = false;
}

void ResharpenDialog::stepDroppedStack(int direction)
{
    const int n = static_cast<int>(filterDecisions_.size());
    if (n == 0) return;
    const int start = std::clamp(stackCombo_->currentIndex(), 0, n - 1);
    for (int step = 1; step <= n; ++step)
    {
        const int idx = (start + direction * step + n) % n;
        if (!filterDecisions_[static_cast<size_t>(idx)].kept)
        {
            stackCombo_->setCurrentIndex(idx);
            return;
        }
    }
}

void ResharpenDialog::onStackTableItemChanged(QTableWidgetItem* item)
{
    if (updatingTable_ || !item || item->column() != 0) return;
    const int row = item->row();
    if (row < 0 || row >= static_cast<int>(stackManualOverrides_.size())) return;
    const bool wantKeep = (item->checkState() == Qt::Checked);
    stackManualOverrides_[static_cast<size_t>(row)] = wantKeep;
    refreshQualityFilter();
}

std::map<QString, bool> ResharpenDialog::currentStackOverrides() const
{
    std::map<QString, bool> out;
    for (size_t i = 0; i < std::min(stackFiles_.size(), stackManualOverrides_.size()); ++i)
        if (stackManualOverrides_[i].has_value())
            out[QFileInfo(stackFiles_[i]).fileName()] = *stackManualOverrides_[i];
    return out;
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
    const std::map<QString, bool> overrides = currentStackOverrides();

    worker_ = std::jthread([this, dir, sharp, tone, rebuildOnly, overrides] {
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
                &cancelFlag_, overrides);
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
                const QString droppedNote = res.stacksDropped > 0
                    ? QString(" (%1 dropped)").arg(res.stacksDropped) : QString();
                statusLabel_->setText(QString("Done: %1 stacks encoded%2 to %3.")
                    .arg(res.stacksUsed).arg(droppedNote, QDir::toNativeSeparators(lastOutputDir_)));
            }
            else
            {
                statusLabel_->setText(QString("Failed: %1").arg(res.error));
            }
        }, Qt::QueuedConnection);
    });
}
