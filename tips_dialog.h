#pragma once

#include "startup_tips.h"

#include <QtWidgets/QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QPushButton;

// Non-modal "Did you know?" startup dialog modeled after SpacePixels' TipsDialog.
// Shows one tip at a time with a link to its section in MANUAL.md, advances the
// saved tip index across runs, and remembers "Show tips at startup" in QSettings.
class TipsDialog final : public QDialog
{
    Q_OBJECT
public:
    using OpenManualCallback = std::function<void(const QString& section)>;

    static constexpr const char* kShowAtStartupKey = "tips/showAtStartup";
    static constexpr const char* kNextTipKey = "tips/next";

    TipsDialog(QWidget* parent, OpenManualCallback openManualSection);

    // Brings the dialog up (non-modal) and focuses Next.
    void open();

    // Displays a specific tip index (wrapping around) and saves the following index.
    void showTip(int requestedIndex);

    [[nodiscard]] int currentIndex() const { return index_; }
    [[nodiscard]] int tipCount() const { return static_cast<int>(tips_.size()); }

    [[nodiscard]] static bool wantedAtStartup();
    static void setWantedAtStartup(bool show);
    [[nodiscard]] static int savedNextTip();

private:
    int measureTallestTipHeight() const;

    const std::vector<StartupTip>& tips_;
    OpenManualCallback openManualSection_;
    QLabel* heading_{nullptr};
    QLabel* body_{nullptr};
    QPushButton* manualLink_{nullptr};
    QCheckBox* showAtStartup_{nullptr};
    QLabel* position_{nullptr};
    QPushButton* previousButton_{nullptr};
    QPushButton* nextButton_{nullptr};
    QPushButton* closeButton_{nullptr};
    int index_{0};
};
