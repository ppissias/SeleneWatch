#include "recording/report_links.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace {

const QStringList kTypes = {"ser", "avi", "png", "gif", "jpg", "jpeg", "mp4", "txt"};
// A clip's files are two levels below its watch folder.
constexpr int kLevelsUp = 3;

} // namespace

std::optional<ReportLinkTarget> reportLinkTarget(const QString& text, QString& error)
{
    const QUrl link(text);
    if (!link.isValid() || link.scheme().compare("selenewatch", Qt::CaseInsensitive) != 0)
    {
        error = "This is not a link from a SeleneWatch report.";
        return std::nullopt;
    }
    ReportLinkTarget target;
    if (link.path() == "open") target.action = ReportLinkAction::Open;
    else if (link.path() == "show") target.action = ReportLinkAction::Show;
    else
    {
        error = QString("A report link cannot ask for \"%1\".").arg(link.path());
        return std::nullopt;
    }
    const QUrl file(QUrlQuery(link).queryItemValue("file", QUrl::FullyDecoded));
    if (!file.isLocalFile())
    {
        error = "The link does not name a file on this computer.";
        return std::nullopt;
    }
    const QFileInfo info(file.toLocalFile());
    if (!info.isFile())
    {
        error = "The file was not found: " + QDir::toNativeSeparators(info.absoluteFilePath());
        return std::nullopt;
    }
    target.path = info.canonicalFilePath();
    const QFileInfo canonical(target.path);
    if (!kTypes.contains(canonical.suffix().toLower()))
    {
        error = "From a report, SeleneWatch opens only SER, AVI, PNG, GIF, JPEG, MP4 and text files, not "
            + QDir::toNativeSeparators(target.path);
        return std::nullopt;
    }
    QDir folder = canonical.dir();
    for (int up = 0; up <= kLevelsUp; ++up)
    {
        if (QFileInfo(folder.filePath("candidates.txt")).isFile()) return target;
        if (!folder.cdUp()) break;
    }
    error = "From a report, SeleneWatch opens only files inside a monitoring session folder, not "
        + QDir::toNativeSeparators(target.path);
    return std::nullopt;
}

bool registerReportLinks(const QString& program, QString& error)
{
#ifdef Q_OS_WIN
    // "Default" is a registry key's unnamed value.
    QSettings classes("HKEY_CURRENT_USER\\Software\\Classes\\selenewatch", QSettings::NativeFormat);
    const QString exe = QDir::toNativeSeparators(program);
    const QString command = QString("\"%1\" --open-link \"%2\"").arg(exe, "%1");
    if (classes.value("shell/open/command/Default").toString() == command) return true;
    classes.setValue("Default", "URL:SeleneWatch report link");
    classes.setValue("URL Protocol", "");
    classes.setValue("DefaultIcon/Default", "\"" + exe + "\",0");
    classes.setValue("shell/open/command/Default", command);
    classes.sync();
    if (classes.status() != QSettings::NoError)
    {
        error = "Report links could not be registered for this user.";
        return false;
    }
#else
    Q_UNUSED(program);
    Q_UNUSED(error);
#endif
    return true;
}
