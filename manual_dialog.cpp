#include "manual_dialog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QImage>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>

namespace {
constexpr auto kGitHubManualUrl = "https://github.com/ppissias/SeleneWatch/blob/main/MANUAL.md";
constexpr auto kGitHubBlobBase = "https://github.com/ppissias/SeleneWatch/blob/main/";
constexpr int kMaxManualImageWidth = 860;

// Renders MANUAL.md and scales wide screenshots from docs/images/ to fit the window.
class ManualTextBrowser final : public QTextBrowser
{
public:
    explicit ManualTextBrowser(QWidget* parent = nullptr) : QTextBrowser(parent) {}

    void setManualDirectory(const QDir& dir) { manualDir_ = dir; }

protected:
    QVariant loadResource(int type, const QUrl& name) override
    {
        if (type == QTextDocument::ImageResource && !manualDir_.path().isEmpty())
        {
            QString localPath;
            if (name.isLocalFile()) localPath = name.toLocalFile();
            else if (name.scheme().isEmpty()) localPath = QDir::cleanPath(manualDir_.filePath(name.toString()));

            if (!localPath.isEmpty() && QFileInfo::exists(localPath))
            {
                QImage image(localPath);
                if (!image.isNull())
                {
                    if (image.width() > kMaxManualImageWidth)
                        image = image.scaledToWidth(kMaxManualImageWidth, Qt::SmoothTransformation);
                    return image;
                }
            }
        }
        return QTextBrowser::loadResource(type, name);
    }

private:
    QDir manualDir_;
};

QString stripLeadingHash(const QString& section)
{
    return section.startsWith(QLatin1Char('#')) ? section.mid(1) : section;
}
}

QString githubHeadingSlug(const QString& headingText)
{
    QString out;
    out.reserve(headingText.size());
    for (const QChar ch : headingText.trimmed().toLower())
    {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-'))
            out.append(ch);
        else if (ch == QLatin1Char(' '))
            out.append(QLatin1Char('-'));
    }
    return out;
}

QString findPackagedManualPath()
{
    QStringList candidateDirs;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
    {
        QDir dir(appDir);
        candidateDirs << dir.absolutePath();
        for (int up = 0; up < 3 && dir.cdUp(); ++up)
            candidateDirs << dir.absolutePath();
    }
    candidateDirs << QDir::currentPath();

    for (const QString& dirPath : candidateDirs)
    {
        const QString candidate = QDir(dirPath).filePath("MANUAL.md");
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

ManualDialog::ManualDialog(QWidget* parent, const QString& manualPathOverride) : QDialog(parent)
{
    setObjectName("manualDialog");
    setWindowTitle("SeleneWatch User Manual");
    setModal(false);
    resize(960, 740);

    auto* layout = new QVBoxLayout(this);
    auto* textBrowser = new ManualTextBrowser(this);
    textBrowser->setObjectName("manualTextBrowser");
    textBrowser->setOpenLinks(false);
    textBrowser->setReadOnly(true);
    browser_ = textBrowser;
    browser_->document()->setDocumentMargin(16);
    layout->addWidget(browser_, 1);

    auto* bottomRow = new QHBoxLayout;
    topButton_ = new QPushButton("Top", this);
    topButton_->setObjectName("manualTopButton");
    topButton_->setToolTip("Jump back to the start of the manual.");
    githubButton_ = new QPushButton("Open on GitHub", this);
    githubButton_->setObjectName("manualOpenGitHubButton");
    githubButton_->setToolTip("Open this manual (or current section) on GitHub in your browser.");
    auto* closeBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    if (auto* closeBtn = closeBox->button(QDialogButtonBox::Close)) closeBtn->setDefault(true);
    bottomRow->addWidget(topButton_);
    bottomRow->addWidget(githubButton_);
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeBox);
    layout->addLayout(bottomRow);

    connect(topButton_, &QPushButton::clicked, this, [this] { scrollToSection({}); });
    connect(githubButton_, &QPushButton::clicked, this, [this] {
        const QString url = currentSection_.isEmpty()
            ? QString::fromLatin1(kGitHubManualUrl)
            : QString::fromLatin1(kGitHubManualUrl) + QLatin1Char('#') + currentSection_;
        QDesktopServices::openUrl(QUrl(url));
    });
    connect(closeBox, &QDialogButtonBox::rejected, this, &QDialog::hide);
    connect(browser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.path().isEmpty() && url.hasFragment())
        {
            scrollToSection(url.fragment());
            return;
        }
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        {
            QDesktopServices::openUrl(url);
            return;
        }
        const QString rel = url.toString(QUrl::RemoveFragment);
        if (!manualPath_.isEmpty() && !rel.isEmpty())
        {
            const QString local = QFileInfo(manualPath_).dir().filePath(rel);
            if (QFileInfo::exists(local))
            {
                QDesktopServices::openUrl(QUrl::fromLocalFile(local));
                return;
            }
        }
        if (!rel.isEmpty())
        {
            const QString fragment = url.hasFragment() ? QLatin1Char('#') + url.fragment() : QString();
            QDesktopServices::openUrl(QUrl(QString::fromLatin1(kGitHubBlobBase) + rel + fragment));
        }
    });

    loadManual(manualPathOverride);
}

void ManualDialog::loadManual(const QString& manualPathOverride)
{
    manualPath_ = manualPathOverride.isEmpty() ? findPackagedManualPath() : manualPathOverride;
    auto* textBrowser = static_cast<ManualTextBrowser*>(browser_);

    if (!manualPath_.isEmpty())
    {
        QFile file(manualPath_);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            const QDir dir = QFileInfo(manualPath_).dir();
            textBrowser->setManualDirectory(dir);
            browser_->setSearchPaths({dir.absolutePath()});
            browser_->document()->setMarkdown(QString::fromUtf8(file.readAll()), QTextDocument::MarkdownDialectGitHub);
            indexHeadings();
            return;
        }
    }

    textBrowser->setManualDirectory(QDir());
    anchorPositions_.clear();
    browser_->document()->setMarkdown(
        "# SeleneWatch User Manual\n\n"
        "The packaged `MANUAL.md` file was not found beside `SeleneWatch.exe`.\n\n"
        "Use **Open on GitHub** below to view the full manual online:\n\n"
        "- [SeleneWatch User Manual on GitHub](https://github.com/ppissias/SeleneWatch/blob/main/MANUAL.md)\n",
        QTextDocument::MarkdownDialectGitHub);
}

void ManualDialog::indexHeadings()
{
    anchorPositions_.clear();
    QHash<QString, int> seen;
    for (QTextBlock block = browser_->document()->begin(); block.isValid(); block = block.next())
    {
        if (block.blockFormat().headingLevel() <= 0) continue;
        const QString base = githubHeadingSlug(block.text());
        if (base.isEmpty()) continue;
        const int count = seen.value(base, 0);
        seen.insert(base, count + 1);
        const QString slug = count == 0 ? base : QString("%1-%2").arg(base).arg(count);
        anchorPositions_.insert(slug, block.position());
    }
}

bool ManualDialog::hasAnchor(const QString& section) const
{
    const QString clean = stripLeadingHash(section);
    return !clean.isEmpty() && anchorPositions_.contains(clean);
}

void ManualDialog::openSection(const QString& section)
{
    show();
    raise();
    activateWindow();
    const QString clean = stripLeadingHash(section);
    if (!clean.isEmpty())
    {
        // Wait one event-loop turn so QTextDocument layout reflects the shown viewport size.
        scrollToSection(clean);
        QTimer::singleShot(0, this, [this, clean] { scrollToSection(clean); });
    }
    else
    {
        scrollToSection({});
    }
}

bool ManualDialog::scrollToSection(const QString& section)
{
    const QString clean = stripLeadingHash(section);
    if (clean.isEmpty())
    {
        currentSection_.clear();
        QTextCursor cursor(browser_->document());
        cursor.movePosition(QTextCursor::Start);
        browser_->setTextCursor(cursor);
        browser_->verticalScrollBar()->setValue(0);
        return true;
    }

    const auto it = anchorPositions_.constFind(clean);
    if (it == anchorPositions_.constEnd()) return false;

    currentSection_ = clean;
    QTextCursor cursor(browser_->document());
    cursor.setPosition(*it);
    browser_->setTextCursor(cursor);
    const QRect rect = browser_->cursorRect(cursor);
    browser_->verticalScrollBar()->setValue(browser_->verticalScrollBar()->value() + rect.top() - 12);
    return true;
}
