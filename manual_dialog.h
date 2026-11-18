#pragma once

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtWidgets/QDialog>

class QPushButton;
class QTextBrowser;

// GitHub-compatible heading anchor generator (lowercase, spaces -> '-', strips
// punctuation while preserving existing hyphens and underscores).
[[nodiscard]] QString githubHeadingSlug(const QString& headingText);

// Locates MANUAL.md beside the running executable or in nearby source/build parents.
[[nodiscard]] QString findPackagedManualPath();

// Non-modal viewer showing the packaged MANUAL.md (with its docs/images/
// screenshots) using Qt's built-in GitHub Markdown support, plus direct jumps
// to any heading section and a button to open the same manual on GitHub.
class ManualDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit ManualDialog(QWidget* parent = nullptr, const QString& manualPathOverride = {});

    // Shows/raises the viewer and scrolls to the given heading anchor (empty = top).
    void openSection(const QString& section = {});

    // Scrolls immediately to the requested heading anchor; returns true if found.
    bool scrollToSection(const QString& section);

    [[nodiscard]] QString loadedManualPath() const { return manualPath_; }
    [[nodiscard]] bool hasAnchor(const QString& section) const;
    [[nodiscard]] QString currentSection() const { return currentSection_; }

private:
    void loadManual(const QString& manualPathOverride);
    void indexHeadings();

    QTextBrowser* browser_{nullptr};
    QPushButton* topButton_{nullptr};
    QPushButton* githubButton_{nullptr};
    QString manualPath_;
    QString currentSection_;
    QHash<QString, int> anchorPositions_;
};
