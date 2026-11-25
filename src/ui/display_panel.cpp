#include "ui/display_panel.h"
#include "ui/widgets/elided_label.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

namespace {
QString levelsText(double black, double white)
{
    return QString("Display: %1–%2 ADU (screen only)").arg(black, 0, 'f', 1).arg(white, 0, 'f', 1);
}
}

DisplayPanel::DisplayPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName("displayPanel");
    setMinimumWidth(280);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* form = new QFormLayout(content);

    mode_ = new QComboBox(content);
    mode_->setObjectName("displayMode");
    mode_->setToolTip("How frames are shown on the screen: the camera's bits as they are, between black and white levels you set, or stretched automatically. Recorded and detected data are never changed.");
    mode_->addItems({"Linear / high bits", "Black / white levels", "Auto-stretch"});
    form->addRow("Display", mode_);

    black_ = new QDoubleSpinBox(content);
    black_->setObjectName("displayBlack");
    black_->setToolTip("Pixel value (ADU) shown as black when Display is Black / white levels.");
    white_ = new QDoubleSpinBox(content);
    white_->setObjectName("displayWhite");
    white_->setToolTip("Pixel value (ADU) shown as white when Display is Black / white levels.");
    for (auto* spin : {black_, white_}) { spin->setDecimals(0); spin->setSuffix(" ADU"); }

    blackSlider_ = new QSlider(Qt::Horizontal, content);
    blackSlider_->setObjectName("blackSlider");
    blackSlider_->setToolTip("Black level of the display.");
    whiteSlider_ = new QSlider(Qt::Horizontal, content);
    whiteSlider_->setObjectName("whiteSlider");
    whiteSlider_->setToolTip("White level of the display.");
    form->addRow("Black", black_);
    form->addRow(blackSlider_);
    form->addRow("White", white_);
    form->addRow(whiteSlider_);

    invert_ = new QCheckBox("Invert brightness", content);
    invert_->setObjectName("displayInvert");
    invert_->setToolTip("Shows the preview as a negative: dark detail can be easier to see. Display only.");
    debayer_ = new QCheckBox("Show Bayer colour", content);
    debayer_->setObjectName("displayDebayer");
    debayer_->setToolTip("Shows a colour camera's Bayer frames in colour. Display only: recordings keep the raw pixels.");
    form->addRow(invert_);
    form->addRow(debayer_);

    levels_ = new ElidedLabel("Display only; raw evidence and detection are unchanged.", Qt::ElideRight, content);
    levels_->setToolTip("Display stretch never alters raw SER evidence or detection.");
    levels_->setObjectName("displayLevels");
    form->addRow(levels_);

    scroll->setWidget(content);
    outer->addWidget(scroll);

    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        if (!updating_) { display_.mode = static_cast<DisplayMode>(mode_->currentIndex()); setDisplaySettings(display_); emit displayChanged(); }
    });
    connect(black_, &QDoubleSpinBox::valueChanged, this, [this] { levelsChanged(false); });
    connect(white_, &QDoubleSpinBox::valueChanged, this, [this] { levelsChanged(true); });
    connect(blackSlider_, &QSlider::valueChanged, this, [this](int n) { if (!updating_) black_->setValue(n); });
    connect(whiteSlider_, &QSlider::valueChanged, this, [this](int n) { if (!updating_) white_->setValue(n); });
    connect(invert_, &QCheckBox::toggled, this, [this](bool value) { if (!updating_) { display_.invert = value; emit displayChanged(); } });
    connect(debayer_, &QCheckBox::toggled, this, [this](bool value) { if (!updating_) { display_.debayer = value; emit displayChanged(); } });
    setNativeMaximum(255);
}

DisplaySettings DisplayPanel::displaySettings() const { return display_; }

void DisplayPanel::setDisplaySettings(DisplaySettings value)
{
    if (!value.valid()) return;
    updating_ = true;
    display_ = value;
    mode_->setCurrentIndex(static_cast<int>(value.mode));
    black_->setValue(value.black * maximum_);
    white_->setValue(value.white * maximum_);
    blackSlider_->setValue(static_cast<int>(black_->value()));
    whiteSlider_->setValue(static_cast<int>(white_->value()));
    invert_->setChecked(value.invert);
    debayer_->setChecked(value.debayer);
    for (auto* widget : {static_cast<QWidget*>(black_), static_cast<QWidget*>(white_), static_cast<QWidget*>(blackSlider_), static_cast<QWidget*>(whiteSlider_)})
        widget->setEnabled(value.mode == DisplayMode::Window);
    updating_ = false;
}

// Map the stored level fractions onto the incoming frame's native ADU range.
void DisplayPanel::setNativeMaximum(int maximum)
{
    updating_ = true;
    maximum_ = maximum;
    black_->setRange(0, maximum - 1);
    white_->setRange(1, maximum);
    blackSlider_->setRange(0, maximum - 1);
    whiteSlider_->setRange(1, maximum);
    setDisplaySettings(display_);
}

// Keep black below white as the observer moves either end of the window.
void DisplayPanel::levelsChanged(bool whiteChanged)
{
    if (updating_) return;
    updating_ = true;
    if (black_->value() >= white_->value())
    {
        if (whiteChanged) black_->setValue(white_->value() - 1);
        else white_->setValue(black_->value() + 1);
    }
    display_.black = black_->value() / maximum_;
    display_.white = white_->value() / maximum_;
    blackSlider_->setValue(static_cast<int>(black_->value()));
    whiteSlider_->setValue(static_cast<int>(white_->value()));
    updating_ = false;
    emit displayChanged();
}

void DisplayPanel::showLevels(DisplayLevels value)
{
    levels_->setText(levelsText(value.black, value.white));
}
