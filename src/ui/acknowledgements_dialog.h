#pragma once

#include <QtWidgets/QDialog>

#include <functional>

class QEvent;
class QTabWidget;
class QTextBrowser;

// Modeless tabbed credits dialog opened from Help → Acknowledgements (and from
// AboutDialog). Renders theme-aware HTML with embedded AI assistant logos and
// clickable references for libraries, algorithms, formats and standards.
class AcknowledgementsDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit AcknowledgementsDialog(QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;

private:
    void populateTabs();
    [[nodiscard]] QString wrapHtml(const QString& body) const;
    [[nodiscard]] QString logoCard(const QString& resourcePath, int width, int height, const QString& contentHtml) const;
    [[nodiscard]] QString softwareUtilitiesHtml() const;
    [[nodiscard]] QString scientificReferencesHtml() const;
    [[nodiscard]] QString formatsStandardsHtml() const;

    QTabWidget* tabs_{nullptr};
    QTextBrowser* softwareBrowser_{nullptr};
    QTextBrowser* referencesBrowser_{nullptr};
    QTextBrowser* formatsBrowser_{nullptr};
};

// Modeless About dialog opened from Help → About SeleneWatch. Displays the
// application icon, version, BSD 2-Clause LICENSE text, third-party notice
// pointer, and direct buttons to Acknowledgements and About Qt.
class AboutDialog final : public QDialog
{
    Q_OBJECT
public:
    AboutDialog(QWidget* parent, const QString& release, const QString& revision,
        std::function<void()> openAcknowledgements, std::function<void()> openAboutQt);

    // Embedded LICENSE text from the repository, or a brief pointer if missing.
    [[nodiscard]] static QString licenseText();
};
