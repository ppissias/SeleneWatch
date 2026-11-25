#include "ui/widgets/collapsible_section.h"

#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QFrame>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

CollapsibleSection::CollapsibleSection(const QString& title, QString settingsKey,
    bool defaultExpanded, QWidget* parent)
    : QWidget(parent), settingsKey_(std::move(settingsKey))
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 4, 0, 0);
    root->setSpacing(4);

    auto* rule = new QFrame(this);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    root->addWidget(rule);

    toggle_ = new QToolButton(this);
    toggle_->setText(title);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setCheckable(true);
    toggle_->setAutoRaise(true);
    toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toggle_->setStyleSheet("QToolButton { text-align: left; font-weight: bold; border: none; padding: 2px 4px; }");
    root->addWidget(toggle_);

    content_ = new QWidget(this);
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(6, 2, 0, 4);
    contentLayout_->setSpacing(4);
    root->addWidget(content_);

    const bool initial = settingsKey_.isEmpty()
        ? defaultExpanded
        : QSettings().value(settingsKey_, defaultExpanded).toBool();
    setExpanded(initial);

    connect(toggle_, &QToolButton::toggled, this, [this](bool on) {
        toggle_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        content_->setVisible(on);
        if (!settingsKey_.isEmpty()) QSettings().setValue(settingsKey_, on);
        emit toggled(on);
    });
}

bool CollapsibleSection::isExpanded() const
{
    return toggle_->isChecked();
}

void CollapsibleSection::setExpanded(bool expanded)
{
    const QSignalBlocker blocked(toggle_);
    toggle_->setChecked(expanded);
    toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    content_->setVisible(expanded);
}
