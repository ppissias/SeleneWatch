#pragma once

#include <QtCore/QString>

// Application-wide visual themes (System, Dark, and dark-adaptation-safe
// Red night vision) persisted in QSettings, plus shared themed stylesheet
// helpers for mode pills, action buttons, and banners.
enum class UiTheme
{
    System,
    Dark,
    NightRed,
};

[[nodiscard]] UiTheme loadUiTheme();
void saveUiTheme(UiTheme theme);
void applyUiTheme(UiTheme theme);
[[nodiscard]] UiTheme currentUiTheme();

[[nodiscard]] QString modePillStyle(bool monitoring, bool timeLapsing, bool running, UiTheme theme = currentUiTheme());
[[nodiscard]] QString primaryActionStyle(UiTheme theme = currentUiTheme());
[[nodiscard]] QString stopActionStyle(UiTheme theme = currentUiTheme());
[[nodiscard]] QString dirtyApplyStyle(UiTheme theme = currentUiTheme());
[[nodiscard]] QString autoTuneButtonStyle(UiTheme theme = currentUiTheme());
[[nodiscard]] QString timeLapseBannerStyle(bool active, UiTheme theme = currentUiTheme());
