#pragma once

#include <QtCore/QString>

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
