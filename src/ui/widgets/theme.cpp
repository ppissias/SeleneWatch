#include "ui/widgets/theme.h"

#include <QtCore/QSettings>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleFactory>

namespace {

UiTheme gCurrentTheme = UiTheme::System;
bool gSavedOriginal = false;
QString gOriginalStyleName;
QPalette gOriginalPalette;

void captureOriginalIfNeeded()
{
    if (gSavedOriginal || !qApp) return;
    gSavedOriginal = true;
    if (QStyle* style = qApp->style()) gOriginalStyleName = style->objectName();
    gOriginalPalette = qApp->palette();
}

QPalette makeDarkPalette()
{
    QPalette p;
    const QColor window("#1b1f27");
    const QColor base("#13161d");
    const QColor altBase("#212630");
    const QColor button("#262c38");
    const QColor text("#e5e9f0");
    const QColor disabledText("#737c8e");
    const QColor highlight("#2f78c4");
    const QColor link("#66b0ff");

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, altBase);
    p.setColor(QPalette::ToolTipBase, QColor("#252b36"));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, disabledText);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, QColor("#3a4252"));
    p.setColor(QPalette::Midlight, QColor("#303746"));
    p.setColor(QPalette::Mid, QColor("#222731"));
    p.setColor(QPalette::Dark, QColor("#12151b"));
    p.setColor(QPalette::Shadow, QColor("#090b0f"));

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor("#323946"));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    return p;
}

QPalette makeNightRedPalette()
{
    // Pure zero-blue, zero-green palette to preserve dark adaptation at the telescope.
    QPalette p;
    const QColor window("#120303");
    const QColor base("#0a0101");
    const QColor altBase("#1a0505");
    const QColor button("#240707");
    const QColor text("#e03c3c");
    const QColor disabledText("#6b2020");
    const QColor highlight("#8c1414");
    const QColor link("#f05454");

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, altBase);
    p.setColor(QPalette::ToolTipBase, QColor("#1f0606"));
    p.setColor(QPalette::ToolTipText, QColor("#f05a5a"));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, disabledText);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor("#ff7070"));
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, QColor("#ffaaaa"));
    p.setColor(QPalette::Light, QColor("#4a1212"));
    p.setColor(QPalette::Midlight, QColor("#380d0d"));
    p.setColor(QPalette::Mid, QColor("#210606"));
    p.setColor(QPalette::Dark, QColor("#0d0202"));
    p.setColor(QPalette::Shadow, QColor("#050000"));

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor("#3b0c0c"));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    return p;
}

QString darkStyleSheet()
{
    return QStringLiteral(
        "QToolBar { background: #181c23; border: none; border-bottom: 1px solid #2e3542; padding: 2px 4px; }"
        "QDockWidget::title { background: #222732; padding: 4px 8px; border-bottom: 1px solid #2f3644; font-weight: bold; }"
        "QTabBar::tab { background: #1f242e; color: #aeb6c4; padding: 4px 8px; border: 1px solid #2e3542; border-bottom: none; }"
        "QTabBar::tab:selected { background: #293140; color: #f0f4fa; border-color: #3f4b5e; }"
        "QCheckBox::indicator:unchecked { border: 1px solid #5c6880; border-radius: 2px; background: #14171e; }"
        "QCheckBox::indicator:unchecked:disabled { border-color: #363e4f; background: #191d25; }"
        "QToolButton { border: 1px solid #353e4e; border-radius: 3px; padding: 2px 7px; background: #252b37; }"
        "QToolButton:hover { background: #2f3746; }"
        "QToolButton:checked { background: #2b5b8c; color: #ffffff; border-color: #4a89c7; }"
        "QToolButton:disabled { color: #677082; border-color: #262b35; background: #1c2028; }"
        "QToolButton:checked:disabled { background: #233a54; color: #a7c2df; border-color: #35587d; }"
        "QToolButton[toolWindowButton=\"true\"] { background: #1b3b42; color: #b8eef7; border: 1px solid #2f6c78; font-weight: 600; padding: 2px 9px; }"
        "QToolButton[toolWindowButton=\"true\"]:hover { background: #234d56; color: #dcf8fc; border-color: #4292a2; }"
        "QToolButton[toolWindowButton=\"true\"]:checked { background: #1d7484; color: #ffffff; border: 1px solid #52c3d6; font-weight: bold; }"
        "QGroupBox { border: 1px solid #303746; border-radius: 4px; margin-top: 8px; padding-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #c5cddb; }"
        "QStatusBar { background: #161920; border-top: 1px solid #2c3340; }"
        "QStatusBar::item { border: none; }"
        "QToolTip { background: #252b36; color: #eef2f7; border: 1px solid #444f63; padding: 4px 6px; }");
}

QString nightRedStyleSheet()
{
    return QStringLiteral(
        "QToolBar { background: #100202; border: none; border-bottom: 1px solid #3d0f0f; padding: 2px 4px; }"
        "QDockWidget::title { background: #1d0505; padding: 4px 8px; border-bottom: 1px solid #421010; font-weight: bold; color: #e84848; }"
        "QTabBar::tab { background: #170404; color: #a62b2b; padding: 4px 8px; border: 1px solid #3d0f0f; border-bottom: none; }"
        "QTabBar::tab:selected { background: #2c0808; color: #ff6060; border-color: #6b1a1a; }"
        "QCheckBox::indicator:unchecked { border: 1px solid #7a2222; border-radius: 2px; background: #0c0202; }"
        "QCheckBox::indicator:unchecked:disabled { border-color: #3f1212; background: #140303; }"
        "QToolButton { border: 1px solid #4a1212; border-radius: 3px; padding: 2px 7px; background: #220606; color: #e03c3c; }"
        "QToolButton:hover { background: #330a0a; }"
        "QToolButton:checked { background: #6e1212; color: #ff9999; border-color: #a62424; }"
        "QToolButton:disabled { color: #5c1b1b; border-color: #290909; background: #140303; }"
        "QToolButton:checked:disabled { background: #470e0e; color: #c46868; border-color: #6b1a1a; }"
        "QToolButton[toolWindowButton=\"true\"] { background: #360909; color: #f56868; border: 1px solid #781a1a; font-weight: 600; padding: 2px 9px; }"
        "QToolButton[toolWindowButton=\"true\"]:hover { background: #4a0e0e; color: #ff8888; border-color: #9e2424; }"
        "QToolButton[toolWindowButton=\"true\"]:checked { background: #871717; color: #ffcccc; border: 1px solid #cc3333; font-weight: bold; }"
        "QGroupBox { border: 1px solid #421010; border-radius: 4px; margin-top: 8px; padding-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #d63a3a; }"
        "QStatusBar { background: #0e0202; border-top: 1px solid #3b0e0e; }"
        "QStatusBar::item { border: none; }"
        "QToolTip { background: #1f0505; color: #f55c5c; border: 1px solid #661818; padding: 4px 6px; }");
}

} // namespace

UiTheme loadUiTheme()
{
    const QString value = QSettings().value("ui/theme", "dark").toString().trimmed().toLower();
    if (value == "system" || value == "light") return UiTheme::System;
    if (value == "night_red" || value == "red") return UiTheme::NightRed;
    return UiTheme::Dark;
}

void saveUiTheme(UiTheme theme)
{
    QSettings saved;
    switch (theme)
    {
    case UiTheme::System: saved.setValue("ui/theme", "system"); break;
    case UiTheme::Dark: saved.setValue("ui/theme", "dark"); break;
    case UiTheme::NightRed: saved.setValue("ui/theme", "night_red"); break;
    }
    saved.sync();
}

void applyUiTheme(UiTheme theme)
{
    gCurrentTheme = theme;
    if (!qApp) return;
    captureOriginalIfNeeded();

    if (theme == UiTheme::System)
    {
        if (!gOriginalStyleName.isEmpty())
        {
            if (QStyle* style = QStyleFactory::create(gOriginalStyleName))
                qApp->setStyle(style);
        }
        qApp->setPalette(gOriginalPalette);
        qApp->setStyleSheet(QStringLiteral(
            "QToolBar { border: none; border-bottom: 1px solid palette(mid); padding: 2px 4px; }"
            "QToolButton { border: 1px solid palette(mid); border-radius: 3px; padding: 2px 7px; }"
            "QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
            "QToolButton[toolWindowButton=\"true\"] { background: #d8eef2; color: #124852; border: 1px solid #6ab0bd; font-weight: 600; padding: 2px 9px; }"
            "QToolButton[toolWindowButton=\"true\"]:hover { background: #c4e4ea; color: #0c3942; border-color: #4696a6; }"
            "QToolButton[toolWindowButton=\"true\"]:checked { background: #1a6d7c; color: #ffffff; border: 1px solid #13535e; font-weight: bold; }"
            "QStatusBar::item { border: none; }"));
        return;
    }

    if (QStyle* fusion = QStyleFactory::create("Fusion"))
        qApp->setStyle(fusion);
    if (theme == UiTheme::Dark)
    {
        qApp->setPalette(makeDarkPalette());
        qApp->setStyleSheet(darkStyleSheet());
    }
    else
    {
        qApp->setPalette(makeNightRedPalette());
        qApp->setStyleSheet(nightRedStyleSheet());
    }
}

UiTheme currentUiTheme()
{
    return gCurrentTheme;
}

QString modePillStyle(bool monitoring, bool timeLapsing, bool running, UiTheme theme)
{
    if (theme == UiTheme::NightRed)
    {
        if (monitoring)
            return "background:#781414; color:#ffaaaa; border:1px solid #b52a2a; border-radius:4px; padding:3px 9px; font-weight:bold;";
        if (timeLapsing)
            return "background:#571010; color:#ff9494; border:1px solid #912020; border-radius:4px; padding:3px 9px; font-weight:bold;";
        if (running)
            return "background:#3d0b0b; color:#f06565; border:1px solid #701a1a; border-radius:4px; padding:3px 9px; font-weight:bold;";
        return "background:#220707; color:#b33b3b; border:1px solid #4a1414; border-radius:4px; padding:3px 9px; font-weight:bold;";
    }
    if (monitoring)
        return "background:#165943; color:white; border-radius:4px; padding:3px 9px; font-weight:bold;";
    if (timeLapsing)
        return "background:#51337a; color:white; border-radius:4px; padding:3px 9px; font-weight:bold;";
    if (running)
        return "background:#244872; color:white; border-radius:4px; padding:3px 9px; font-weight:bold;";
    return theme == UiTheme::Dark
        ? "background:#2d3442; color:#c8d0de; border-radius:4px; padding:3px 9px; font-weight:bold;"
        : "background:#4a5568; color:white; border-radius:4px; padding:3px 9px; font-weight:bold;";
}

QString primaryActionStyle(UiTheme theme)
{
    if (theme == UiTheme::NightRed)
        return "QPushButton { background:#6e1212; color:#ff9e9e; border:1px solid #a62424; border-radius:3px; padding:3px 10px; font-weight:bold; }"
               "QPushButton:hover { background:#851717; }"
               "QPushButton:disabled { background:#220808; color:#662020; border-color:#3a0f0f; font-weight:normal; }";
    return "QPushButton { background:#2a6db0; color:white; border:1px solid #3c82c9; border-radius:3px; padding:3px 10px; font-weight:bold; }"
           "QPushButton:hover { background:#347ec8; }"
           "QPushButton:disabled { background:palette(button); color:palette(mid); border:1px solid palette(mid); font-weight:normal; }";
}

QString stopActionStyle(UiTheme theme)
{
    if (theme == UiTheme::NightRed)
        return "QPushButton { background:#4f0d0d; color:#ff8080; border:1px solid #8f1d1d; border-radius:3px; padding:3px 10px; font-weight:bold; }"
               "QPushButton:hover { background:#661212; }";
    return "QPushButton { background:#8f2a2a; color:white; border:1px solid #b03a3a; border-radius:3px; padding:3px 10px; font-weight:bold; }"
           "QPushButton:hover { background:#a83333; }";
}

QString dirtyApplyStyle(UiTheme theme)
{
    if (theme == UiTheme::NightRed)
        return "QPushButton { background:#6e1212; color:#ffaaaa; border:1px solid #b82c2c; border-radius:3px; padding:3px 8px; font-weight:bold; }"
               "QPushButton:hover { background:#851818; }";
    return "QPushButton { background:#2a6db0; color:white; border:1px solid #4990d8; border-radius:3px; padding:3px 8px; font-weight:bold; }"
           "QPushButton:hover { background:#347ec8; }";
}

QString autoTuneButtonStyle(UiTheme theme)
{
    if (theme == UiTheme::NightRed)
        return "QPushButton { font-weight: bold; background: #3d0b0b; color: #e88080; border: 1px solid #701a1a; border-radius: 3px; padding: 3px 10px; }"
               "QPushButton:hover { background: #521010; }"
               "QPushButton:disabled { background: #1a0505; color: #662020; border-color: #360e0e; }";
    if (theme == UiTheme::System)
        return "QPushButton { font-weight: bold; background: #dce8f4; color: #1c3d5c; border: 1px solid #8eb1d4; border-radius: 3px; padding: 3px 10px; }"
               "QPushButton:hover { background: #cde0f2; }"
               "QPushButton:disabled { background: palette(button); color: palette(mid); border: 1px solid palette(mid); }";
    return "QPushButton { font-weight: bold; background: #223548; color: #d8e6f5; border: 1px solid #3a5c7d; border-radius: 3px; padding: 3px 10px; }"
           "QPushButton:hover { background: #2b445c; }"
           "QPushButton:disabled { background: #1f242e; color: #737c8e; border: 1px solid #2c3340; }";
}

QString timeLapseBannerStyle(bool active, UiTheme theme)
{
    if (theme == UiTheme::NightRed)
        return active
            ? "background:#6b1313; color:#ffaaaa; font-weight:bold; padding:5px 8px; border-radius:4px;"
            : "background:#2b0909; color:#d94a4a; font-weight:bold; padding:5px 8px; border-radius:4px;";
    return active
        ? "background:#164b3c; color:white; font-weight:bold; padding:5px 8px; border-radius:4px;"
        : "background:#3b3f4a; color:white; font-weight:bold; padding:5px 8px; border-radius:4px;";
}
