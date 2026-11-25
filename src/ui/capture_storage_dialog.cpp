#include "ui/capture_storage_dialog.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

CaptureStorageDialog::CaptureStorageDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName("captureStorageDialog");
    setWindowTitle("Capture & storage");
    setMinimumWidth(380);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto* note = new QLabel("Clip timing, RAM ceiling, and disk reserve apply at the next Start (they are locked while streaming).", this);
    note->setWordWrap(true);
    root->addWidget(note);

    auto* clipBox = new QGroupBox("Event clip timing (next Start)", this);
    auto* clipForm = new QFormLayout(clipBox);
    history_ = new QDoubleSpinBox(clipBox);
    history_->setObjectName("historySeconds");
    history_->setRange(0, 600);
    history_->setDecimals(2);
    history_->setSuffix(" s");
    history_->setToolTip("Seconds of source time kept before a trigger, including during fast playback. The effective history is capped by the RAM ceiling.");

    post_ = new QDoubleSpinBox(clipBox);
    post_->setObjectName("postSeconds");
    post_->setRange(0.05, 3600);
    post_->setDecimals(2);
    post_->setSuffix(" s");
    post_->setToolTip("A clip continues until the event has been quiet this long.");

    maxPost_ = new QDoubleSpinBox(clipBox);
    maxPost_->setObjectName("maxPostSeconds");
    maxPost_->setRange(0.05, 3600);
    maxPost_->setDecimals(2);
    maxPost_->setSuffix(" s");
    maxPost_->setToolTip("A clip stops this long after the trigger even if the event is still active.");

    clipForm->addRow("Pre-event history", history_);
    clipForm->addRow("Quiet tail", post_);
    clipForm->addRow("Maximum after trigger", maxPost_);
    root->addWidget(clipBox);

    auto* storageBox = new QGroupBox("Memory & disk (next Start)", this);
    storageForm_ = new QFormLayout(storageBox);
    memory_ = new QSpinBox(storageBox);
    memory_->setObjectName("memoryMiB");
    memory_->setRange(16, 65536);
    memory_->setSuffix(" MiB");
    memory_->setToolTip("Most RAM used for frames: the pre-event history, clips being collected, and time-lapse bursts share it. History is shortened to fit.");

    reserve_ = new QDoubleSpinBox(storageBox);
    reserve_->setObjectName("diskReserveGiB");
    reserve_->setRange(0, 100000);
    reserve_->setDecimals(0);
    reserve_->setSuffix(" GiB");
    reserve_->setSpecialValueText("None");
    reserve_->setToolTip("A clip is not written when it would leave less than this free on the output drive. Its candidates are still logged.");

    format_ = new QComboBox(storageBox);
    format_->setObjectName("syntheticFormat");
    format_->setToolTip("Pixel format of the synthetic test source (testing tools).");
    format_->addItems({"Mono8", "Mono12, low bits", "Mono12, high bits", "Mono16", "Bayer RGGB, 12 high"});

    usage_ = new QLabel(storageBox);
    usage_->setWordWrap(true);
    usage_->setObjectName("memoryUsage");
    usage_->setToolTip("Image and analysis allocations, including shared frames, maps and previews. Process metadata, Qt caches and codec workspaces are additional.");

    storageForm_->addRow("RAM ceiling", memory_);
    storageForm_->addRow("Disk reserve", reserve_);
    storageForm_->addRow("Synthetic pixels", format_);
    storageForm_->addRow(usage_);
    storageForm_->setRowVisible(format_, false);
    root->addWidget(storageBox);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    closeButton_ = new QPushButton("Close", this);
    closeButton_->setObjectName("captureStorageClose");
    closeButton_->setToolTip("Closes the Capture & storage window. Settings are saved automatically.");
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(closeButton_);
    root->addLayout(buttons);

    connect(post_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (value > maxPost_->value()) maxPost_->setValue(value);
        if (!updating_) emit settingsChanged();
    });
    connect(maxPost_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (value < post_->value()) post_->setValue(value);
        if (!updating_) emit settingsChanged();
    });
    connect(history_, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) emit settingsChanged(); });
    connect(memory_, &QSpinBox::valueChanged, this, [this] { if (!updating_) emit settingsChanged(); });
    connect(reserve_, &QDoubleSpinBox::valueChanged, this, [this] { if (!updating_) emit settingsChanged(); });

    setCaptureSettings({});
}

CaptureSettings CaptureStorageDialog::captureSettings() const
{
    return {history_->value(), post_->value(), maxPost_->value(),
        static_cast<size_t>(memory_->value()) * 1024 * 1024,
        target_, static_cast<uint64_t>(reserve_->value()) * 1024 * 1024 * 1024};
}

int CaptureStorageDialog::syntheticFormat() const
{
    return format_->currentIndex();
}

void CaptureStorageDialog::setCaptureSettings(CaptureSettings value)
{
    if (!value.valid()) return;
    updating_ = true;
    history_->setValue(value.historySeconds);
    maxPost_->setValue(value.maxPostSeconds);
    post_->setValue(value.postSeconds);
    memory_->setValue(static_cast<int>(value.memoryBytes / (1024 * 1024)));
    reserve_->setValue(static_cast<double>(value.diskReserveBytes / (uint64_t{1024} * 1024 * 1024)));
    target_ = value.target;
    updating_ = false;
}

void CaptureStorageDialog::setRunning(bool running, bool recording)
{
    for (auto* widget : {static_cast<QWidget*>(history_), static_cast<QWidget*>(post_),
            static_cast<QWidget*>(maxPost_), static_cast<QWidget*>(memory_), static_cast<QWidget*>(reserve_)})
        widget->setEnabled(!running);
    format_->setEnabled(!running && !recording);
}

void CaptureStorageDialog::setTestingTools(bool on)
{
    storageForm_->setRowVisible(format_, on);
}

void CaptureStorageDialog::showUsage(MemoryUsage memory, RingUsage history, WriterUsage writer)
{
    usage_->setText(QString("RAM %1 / %2 MiB (peak %3).\nHistory %4 s, %5 frames%6.\nWriter: %7 clips, %8 MiB.\nBuffered SER rate: %9 MB/s.\nAllocation refusals: %10.")
        .arg(memory.used / 1048576.0, 0, 'f', 1).arg(memory.limit / 1048576.0, 0, 'f', 0).arg(memory.peak / 1048576.0, 0, 'f', 1)
        .arg(history.seconds, 0, 'f', 2).arg(history.frames).arg(history.memoryClamped ? " (RAM-clamped)" : "")
        .arg(writer.outstandingClips).arg(writer.retainedBytes / 1048576.0, 0, 'f', 1)
        .arg(writer.writeSeconds > 0 ? writer.writtenBytes / writer.writeSeconds / 1e6 : 0, 0, 'f', 1).arg(memory.refused));
}
