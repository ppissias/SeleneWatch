#pragma once

#include "camera_family_panel.h"

#include <QtWidgets/QDialog>

#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;

// Session → Camera support: for every camera family, whether this version
// supports it, which SDK library it uses and its version, the connected
// cameras, and what to install. The observer can choose a family's DLL here.
// Checks run only when the window opens or the observer asks, and only while
// no stream runs: starting SeleneWatch never loads a vendor's code, and a
// running camera's SDK belongs to its source.
class CameraSupportDialog final : public QDialog
{
    Q_OBJECT
public:
    CameraSupportDialog(const std::vector<CameraFamilyPanel*>& families, bool streamRunning, QWidget* parent = nullptr);
    // Checks every family this version supports. Done when the window opens.
    void checkAll();

private:
    struct Row
    {
        QString key, name;
        CameraFamilyPanel* panel{nullptr};  // null when this version does not support the family
        CameraSupport support;
        bool checked{false};
    };
    void check(size_t row);
    void showRow(size_t row);
    void showSelection();
    [[nodiscard]] int selectedRow() const;
    std::vector<Row> rows_;
    bool running_;
    QTableWidget* table_;
    QLabel* details_;
    QPushButton *choose_, *automatic_, *checkAgain_;
};
