#include "ui/widgets/timelapse_stack_strip.h"

#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

namespace {
constexpr int kThumbW = 72;
constexpr int kThumbH = 48;
constexpr int kCardW = 84;
constexpr int kCardH = 72;
constexpr const char* kSettingsKey = "ui/sections/timelapseStrip";
}

TimelapseStackStrip::TimelapseStackStrip(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("timelapseStackStrip");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 2, 0, 0);
    root->setSpacing(2);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    toggle_ = new QToolButton(this);
    toggle_->setObjectName("timelapseStripToggle");
    toggle_->setCheckable(true);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setAutoRaise(true);
    toggle_->setStyleSheet("QToolButton { font-weight: bold; text-align: left; padding: 1px 4px; }");
    toggle_->setToolTip("Show or hide the horizontal strip of completed time-lapse stacks below the preview.");

    hintLabel_ = new QLabel("Click a stack to inspect in the preview", this);
    hintLabel_->setObjectName("timelapseStripHint");
    hintLabel_->setStyleSheet("color: #888888; font-size: 11px;");

    header->addWidget(toggle_);
    header->addWidget(hintLabel_);
    header->addStretch(1);
    root->addLayout(header);

    scroll_ = new QScrollArea(this);
    scroll_->setObjectName("timelapseStripScroll");
    scroll_->setFrameShape(QFrame::StyledPanel);
    scroll_->setWidgetResizable(true);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_->setFixedHeight(kCardH + 14);

    stripContent_ = new QWidget(scroll_);
    stripLayout_ = new QHBoxLayout(stripContent_);
    stripLayout_->setContentsMargins(6, 4, 6, 4);
    stripLayout_->setSpacing(6);

    emptyLabel_ = new QLabel("Completed stacks will appear here during a time-lapse.", stripContent_);
    emptyLabel_->setObjectName("timelapseStripEmpty");
    emptyLabel_->setStyleSheet("color: #888888;");
    stripLayout_->addWidget(emptyLabel_);
    stripLayout_->addStretch(1);

    scroll_->setWidget(stripContent_);
    root->addWidget(scroll_);

    const bool open = QSettings().value(kSettingsKey, true).toBool();
    setExpanded(open);

    connect(toggle_, &QToolButton::toggled, this, [this](bool on) {
        setExpanded(on);
        QSettings().setValue(kSettingsKey, on);
    });
    updateHeader();
}

void TimelapseStackStrip::addStack(int stackNumber, const QImage& image, const QString& tip)
{
    if (image.isNull()) return;
    if (entries_.size() >= kMaxStacks)
    {
        Entry oldest = std::move(entries_.front());
        entries_.pop_front();
        if (oldest.button)
        {
            stripLayout_->removeWidget(oldest.button);
            oldest.button->deleteLater();
        }
    }

    emptyLabel_->hide();
    auto* btn = new QToolButton(stripContent_);
    btn->setObjectName(QString("timelapseStackCard_%1").arg(stackNumber));
    btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    btn->setFixedSize(kCardW, kCardH);
    btn->setIconSize(QSize(kThumbW, kThumbH));
    const QPixmap pm = QPixmap::fromImage(image).scaled(kThumbW, kThumbH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    btn->setIcon(QIcon(pm));
    btn->setText(QString("#%1").arg(stackNumber));
    btn->setToolTip(tip.isEmpty() ? QString("Inspect stack #%1 in the main preview.").arg(stackNumber) : tip);
    btn->setWhatsThis(btn->toolTip());
    btn->setCheckable(true);
    connect(btn, &QToolButton::clicked, this, [this, stackNumber] { emit stackSelected(stackNumber); });

    // Insert before the trailing stretch.
    const int insertAt = std::max(0, stripLayout_->count() - 1);
    stripLayout_->insertWidget(insertAt, btn);
    entries_.push_back({stackNumber, image, btn});
    updateHeader();
    updateCardStyles();

    // Auto-scroll smoothly to keep the newest stack visible.
    QTimer::singleShot(0, scroll_, [this] {
        if (QScrollBar* hb = scroll_->horizontalScrollBar())
            hb->setValue(hb->maximum());
    });
}

void TimelapseStackStrip::clear()
{
    for (Entry& e : entries_)
    {
        if (e.button)
        {
            stripLayout_->removeWidget(e.button);
            e.button->deleteLater();
        }
    }
    entries_.clear();
    highlightedNumber_ = -1;
    emptyLabel_->show();
    updateHeader();
}

void TimelapseStackStrip::setHighlightedStack(int stackNumber)
{
    highlightedNumber_ = stackNumber;
    updateCardStyles();
}

void TimelapseStackStrip::setExpanded(bool expanded)
{
    const QSignalBlocker blocked(toggle_);
    toggle_->setChecked(expanded);
    toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    scroll_->setVisible(expanded);
    hintLabel_->setVisible(expanded);
    updateGeometry();
}

bool TimelapseStackStrip::isExpanded() const
{
    return toggle_->isChecked();
}

QImage TimelapseStackStrip::stackImage(int stackNumber) const
{
    for (const Entry& e : entries_)
        if (e.number == stackNumber) return e.image;
    return {};
}

QImage TimelapseStackStrip::stackImageAtIndex(int index) const
{
    if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return {};
    return entries_[static_cast<size_t>(index)].image;
}

int TimelapseStackStrip::stackNumberAtIndex(int index) const
{
    if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return 0;
    return entries_[static_cast<size_t>(index)].number;
}

void TimelapseStackStrip::updateHeader()
{
    const int n = static_cast<int>(entries_.size());
    toggle_->setText(n > 0 ? QString("Latest stacks (%1)").arg(n) : QString("Latest stacks"));
}

void TimelapseStackStrip::updateCardStyles()
{
    for (const Entry& e : entries_)
    {
        if (!e.button) continue;
        const QSignalBlocker blocked(e.button);
        e.button->setChecked(e.number == highlightedNumber_);
    }
}
