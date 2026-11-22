#pragma once

#include <QtCore/QString>

#include <vector>

// One tip shown in the non-modal "Did you know?" startup dialog, paired with
// a short topic title and the GitHub-style heading anchor of the matching
// section in MANUAL.md.
struct StartupTip
{
    QString title;
    QString html;
    QString manualSection;
};

// All startup and Help → Tips entries, in display order.
[[nodiscard]] const std::vector<StartupTip>& allStartupTips();

// Wraps any tip index into [0, count), handling negative steps cleanly.
[[nodiscard]] int wrapTipIndex(int index, int count);
