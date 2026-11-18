#include "tips_dialog.h"

#include <QtCore/QSettings>
#include <QtGui/QFont>
#include <QtGui/QPalette>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace {
constexpr int kTipTextWidth = 480;
}

TipsDialog::TipsDialog(QWidget* parent, OpenManualCallback openManualSection)
    : QDialog(parent), tips_(allStartupTips()), openManualSection_(std::move(openManualSection))
{
    setObjectName("tipsDialog");
    setWindowTitle("SeleneWatch tips");
    setModal(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);

    heading_ = new QLabel("Did you know?", this);
    heading_->setObjectName("tipsHeading");
    QFont headingFont = heading_->font();
    headingFont.setBold(true);
    if (headingFont.pointSizeF() > 0) headingFont.setPointSizeF(headingFont.pointSizeF() * 1.25);
    heading_->setFont(headingFont);
    const bool darkBg = palette().color(QPalette::Window).lightness() < 128;
    heading_->setStyleSheet(QString("color: %1;").arg(darkBg ? "#65d5bd" : "#00756e"));
    root->addWidget(heading_);

    body_ = new QLabel(this);
    body_->setObjectName("tipsBodyLabel");
    body_->setTextFormat(Qt::RichText);
    body_->setWordWrap(true);
    body_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    body_->setFixedWidth(kTipTextWidth);
    body_->setMinimumHeight(measureTallestTipHeight() + 8);
    root->addWidget(body_);

    auto* manualRow = new QHBoxLayout;
    manualRow->setContentsMargins(0, 0, 0, 4);
    manualLink_ = new QPushButton("Read more in the manual", this);
    manualLink_->setObjectName("tipsManualLink");
    manualLink_->setFlat(true);
    manualLink_->setCursor(Qt::PointingHandCursor);
    manualLink_->setToolTip("Open the matching section of the packaged User Manual.");
    QFont linkFont = manualLink_->font();
    linkFont.setUnderline(true);
    manualLink_->setFont(linkFont);
    manualLink_->setStyleSheet(QString("QPushButton { color: %1; text-align: left; padding: 0; border: none; }")
        .arg(palette().color(QPalette::Link).name()));
    manualRow->addWidget(manualLink_);
    manualRow->addStretch(1);
    root->addLayout(manualRow);

    auto* bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(8);
    showAtStartup_ = new QCheckBox("Show tips at startup", this);
    showAtStartup_->setObjectName("showTipsAtStartupCheckBox");
    showAtStartup_->setChecked(wantedAtStartup());
    showAtStartup_->setToolTip("You can reopen these tips anytime from Help → Tips.");
    bottomRow->addWidget(showAtStartup_);
    bottomRow->addStretch(1);

    position_ = new QLabel(this);
    position_->setObjectName("tipsPositionLabel");
    bottomRow->addWidget(position_);

    previousButton_ = new QPushButton("< Previous", this);
    previousButton_->setObjectName("tipsPreviousButton");
    nextButton_ = new QPushButton("Next >", this);
    nextButton_->setObjectName("tipsNextButton");
    nextButton_->setDefault(true);
    closeButton_ = new QPushButton("Close", this);
    closeButton_->setObjectName("tipsCloseButton");
    bottomRow->addWidget(previousButton_);
    bottomRow->addWidget(nextButton_);
    bottomRow->addWidget(closeButton_);
    root->addLayout(bottomRow);

    connect(showAtStartup_, &QCheckBox::toggled, this, &TipsDialog::setWantedAtStartup);
    connect(previousButton_, &QPushButton::clicked, this, [this] { showTip(index_ - 1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { showTip(index_ + 1); });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::hide);
    connect(manualLink_, &QPushButton::clicked, this, [this] {
        if (openManualSection_ && index_ >= 0 && index_ < static_cast<int>(tips_.size()))
            openManualSection_(tips_[index_].manualSection);
    });

    showTip(savedNextTip());
    adjustSize();
}

int TipsDialog::measureTallestTipHeight() const
{
    QLabel scratch;
    scratch.setFont(body_->font());
    scratch.setTextFormat(Qt::RichText);
    scratch.setWordWrap(true);
    scratch.setFixedWidth(kTipTextWidth);
    int tallest = 0;
    for (const StartupTip& tip : tips_)
    {
        scratch.setText(tip.html);
        tallest = std::max(tallest, scratch.heightForWidth(kTipTextWidth));
    }
    return tallest;
}

void TipsDialog::open()
{
    if (parentWidget())
    {
        const QRect parentGeo = parentWidget()->frameGeometry();
        move(parentGeo.center() - rect().center());
    }
    nextButton_->setFocus();
    show();
    raise();
    activateWindow();
}

void TipsDialog::showTip(int requestedIndex)
{
    const int count = static_cast<int>(tips_.size());
    if (count == 0) return;
    index_ = wrapTipIndex(requestedIndex, count);
    body_->setText(tips_[index_].html);
    position_->setText(QString("%1 of %2").arg(index_ + 1).arg(count));
    QSettings saved;
    saved.setValue(kNextTipKey, wrapTipIndex(index_ + 1, count));
}

bool TipsDialog::wantedAtStartup()
{
    return QSettings().value(kShowAtStartupKey, true).toBool();
}

void TipsDialog::setWantedAtStartup(bool show)
{
    QSettings().setValue(kShowAtStartupKey, show);
}

int TipsDialog::savedNextTip()
{
    return wrapTipIndex(QSettings().value(kNextTipKey, 0).toInt(),
        static_cast<int>(allStartupTips().size()));
}
