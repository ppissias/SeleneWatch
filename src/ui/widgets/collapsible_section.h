#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QToolButton;
class QVBoxLayout;

// A lightweight disclosure section with a persistent expanded/collapsed state
// (saved under an optional QSettings key). Keeps secondary or diagnostic
// controls out of the way until opened, while letting observers who prefer
// them open keep them expanded across launches.
class CollapsibleSection : public QWidget
{
    Q_OBJECT

public:
    explicit CollapsibleSection(const QString& title, QString settingsKey = {},
        bool defaultExpanded = false, QWidget* parent = nullptr);

    [[nodiscard]] QWidget* content() const { return content_; }
    [[nodiscard]] QVBoxLayout* contentLayout() const { return contentLayout_; }
    [[nodiscard]] QToolButton* toggleButton() const { return toggle_; }
    [[nodiscard]] bool isExpanded() const;
    void setExpanded(bool expanded);

signals:
    void toggled(bool expanded);

private:
    QString settingsKey_;
    QToolButton* toggle_{nullptr};
    QWidget* content_{nullptr};
    QVBoxLayout* contentLayout_{nullptr};
};
