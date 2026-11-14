#include "display_panel.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>

#include <algorithm>
#include <cmath>

DisplayPanel::DisplayPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("displayPanel");
    setMinimumWidth(350);
    auto* form = new QFormLayout(this);
    mode_ = new QComboBox(this); mode_->setObjectName("displayMode");
    mode_->addItems({"Linear / high bits", "Black / white levels", "Auto-stretch"});
    form->addRow("Display", mode_);
    black_ = new QDoubleSpinBox(this); black_->setObjectName("displayBlack");
    white_ = new QDoubleSpinBox(this); white_->setObjectName("displayWhite");
    for (auto* spin : {black_, white_}) { spin->setDecimals(0); spin->setSuffix(" ADU"); }
    blackSlider_ = new QSlider(Qt::Horizontal, this); blackSlider_->setObjectName("blackSlider");
    whiteSlider_ = new QSlider(Qt::Horizontal, this); whiteSlider_->setObjectName("whiteSlider");
    form->addRow("Black", black_); form->addRow(blackSlider_);
    form->addRow("White", white_); form->addRow(whiteSlider_);
    invert_ = new QCheckBox("Invert brightness", this); invert_->setObjectName("displayInvert");
    debayer_ = new QCheckBox("Show Bayer colour", this); debayer_->setObjectName("displayDebayer");
    form->addRow(invert_); form->addRow(debayer_);
    levels_ = new QLabel("Display only; raw evidence and detection are unchanged.", this);
    levels_->setWordWrap(true); levels_->setObjectName("displayLevels"); form->addRow(levels_);
    auto* heading = new QLabel("Capture settings (next Start)", this); form->addRow(heading);
    history_ = new QDoubleSpinBox(this); history_->setObjectName("historySeconds");
    post_ = new QDoubleSpinBox(this); post_->setObjectName("postSeconds");
    maxPost_ = new QDoubleSpinBox(this); maxPost_->setObjectName("maxPostSeconds");
    for (auto* spin : {history_, post_, maxPost_}) { spin->setRange(0.05, 3600); spin->setDecimals(2); spin->setSuffix(" s"); }
    history_->setRange(0, 600);
    history_->setToolTip("Seconds of source time, including during fast playback. The effective history is capped by the RAM allowance.");
    memory_ = new QSpinBox(this); memory_->setObjectName("memoryMiB"); memory_->setRange(16, 65536); memory_->setSuffix(" MiB");
    form->addRow("Pre-event history", history_); form->addRow("Quiet tail", post_);
    form->addRow("Maximum after trigger", maxPost_); form->addRow("RAM ceiling", memory_);
    format_ = new QComboBox(this); format_->setObjectName("syntheticFormat");
    format_->addItems({"Mono8", "Mono12, low bits", "Mono12, high bits", "Mono16", "Bayer RGGB, 12 high"});
    form->addRow("Synthetic pixels", format_);
    usage_ = new QLabel(this); usage_->setWordWrap(true); usage_->setObjectName("memoryUsage");
    usage_->setToolTip("Image and analysis allocations, including shared frames, maps and previews. Process metadata, Qt caches and codec workspaces are additional.");
    form->addRow(usage_);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_) { display_.mode = static_cast<DisplayMode>(mode_->currentIndex()); setDisplaySettings(display_); emit displayChanged(); }
    });
    connect(black_, &QDoubleSpinBox::valueChanged, this, [this] { levelsChanged(false); });
    connect(white_, &QDoubleSpinBox::valueChanged, this, [this] { levelsChanged(true); });
    connect(blackSlider_, &QSlider::valueChanged, this, [this](int n) { if (!updating_) black_->setValue(n); });
    connect(whiteSlider_, &QSlider::valueChanged, this, [this](int n) { if (!updating_) white_->setValue(n); });
    connect(invert_, &QCheckBox::toggled, this, [this](bool value) { if (!updating_) { display_.invert = value; emit displayChanged(); } });
    connect(debayer_, &QCheckBox::toggled, this, [this](bool value) { if (!updating_) { display_.debayer = value; emit displayChanged(); } });
    connect(post_, &QDoubleSpinBox::valueChanged, this, [this](double value) { if (value > maxPost_->value()) maxPost_->setValue(value); });
    connect(maxPost_, &QDoubleSpinBox::valueChanged, this, [this](double value) { if (value < post_->value()) post_->setValue(value); });
    setNativeMaximum(255); setCaptureSettings({});
}

DisplaySettings DisplayPanel::displaySettings() const { return display_; }
CaptureSettings DisplayPanel::captureSettings() const
{
    return {history_->value(), post_->value(), maxPost_->value(), static_cast<size_t>(memory_->value()) * 1024 * 1024};
}
int DisplayPanel::syntheticFormat() const { return format_->currentIndex(); }

void DisplayPanel::setDisplaySettings(DisplaySettings value)
{
    if (!value.valid()) return;
    updating_ = true; display_ = value;
    mode_->setCurrentIndex(static_cast<int>(value.mode));
    black_->setValue(value.black * maximum_); white_->setValue(value.white * maximum_);
    blackSlider_->setValue(static_cast<int>(black_->value())); whiteSlider_->setValue(static_cast<int>(white_->value()));
    invert_->setChecked(value.invert); debayer_->setChecked(value.debayer);
    for (auto* widget : {static_cast<QWidget*>(black_), static_cast<QWidget*>(white_), static_cast<QWidget*>(blackSlider_), static_cast<QWidget*>(whiteSlider_)})
        widget->setEnabled(value.mode == DisplayMode::Window);
    updating_ = false;
}

void DisplayPanel::setCaptureSettings(CaptureSettings value)
{
    if (!value.valid()) return;
    history_->setValue(value.historySeconds); maxPost_->setValue(value.maxPostSeconds); post_->setValue(value.postSeconds);
    memory_->setValue(static_cast<int>(value.memoryBytes / (1024 * 1024)));
}

void DisplayPanel::setNativeMaximum(int maximum)
{
    updating_ = true; maximum_ = maximum;
    black_->setRange(0, maximum - 1); white_->setRange(1, maximum);
    blackSlider_->setRange(0, maximum - 1); whiteSlider_->setRange(1, maximum);
    setDisplaySettings(display_);
}

void DisplayPanel::levelsChanged(bool whiteChanged)
{
    if (updating_) return;
    updating_ = true;
    if (black_->value() >= white_->value())
    {
        if (whiteChanged) black_->setValue(white_->value() - 1);
        else white_->setValue(black_->value() + 1);
    }
    display_.black = black_->value() / maximum_; display_.white = white_->value() / maximum_;
    blackSlider_->setValue(static_cast<int>(black_->value())); whiteSlider_->setValue(static_cast<int>(white_->value()));
    updating_ = false; emit displayChanged();
}

void DisplayPanel::setRunning(bool running, bool recording)
{
    for (auto* widget : {static_cast<QWidget*>(history_), static_cast<QWidget*>(post_), static_cast<QWidget*>(maxPost_), static_cast<QWidget*>(memory_)}) widget->setEnabled(!running);
    format_->setEnabled(!running && !recording);
}

void DisplayPanel::showLevels(DisplayLevels value)
{
    levels_->setText(QString("Display: %1 to %2 ADU.\nDisplay changes never alter raw evidence or detection.")
        .arg(value.black, 0, 'f', 1).arg(value.white, 0, 'f', 1));
}

void DisplayPanel::showUsage(MemoryUsage memory, RingUsage history, WriterUsage writer)
{
    usage_->setText(QString("RAM %1 / %2 MiB (peak %3).\nHistory %4 s, %5 frames%6.\nWriter: %7 clips, %8 MiB.\nBuffered SER rate: %9 MB/s.\nAllocation refusals: %10.")
        .arg(memory.used / 1048576.0, 0, 'f', 1).arg(memory.limit / 1048576.0, 0, 'f', 0).arg(memory.peak / 1048576.0, 0, 'f', 1)
        .arg(history.seconds, 0, 'f', 2).arg(history.frames).arg(history.memoryClamped ? " (RAM-clamped)" : "")
        .arg(writer.outstandingClips).arg(writer.retainedBytes / 1048576.0, 0, 'f', 1)
        .arg(writer.writeSeconds > 0 ? writer.writtenBytes / writer.writeSeconds / 1e6 : 0, 0, 'f', 1).arg(memory.refused));
}
