#pragma once

#include <QtCore/QString>

#include <optional>

// Feature-46: links in a nightly report that open a clip's files with the
// observer's own programs. A browser does not hand a local file to a desktop
// program; it copies it into Downloads instead, which for a SER is hundreds
// of megabytes. The report therefore links to
//     selenewatch:open?file=<the file's file:/// URL>
// (and selenewatch:show?file=... to show the file in Explorer). After the
// browser asks, Windows hands the link to `SeleneWatch.exe --open-link
// <link>`, which opens the file with its associated program and exits.
//
// Any web page can make such a link, so only what a monitoring session holds
// is opened: an existing file of a few known types (SER, AVI, PNG, GIF, JPEG,
// MP4, text) inside a watch folder, that is one with a candidates.txt at most
// three levels up. Nothing is run and no argument is passed on.

enum class ReportLinkAction { Open, Show };

struct ReportLinkTarget
{
    ReportLinkAction action{ReportLinkAction::Open};
    // The file's canonical path.
    QString path;
};

// What a selenewatch: link asks for, or nothing, with why, when refused.
[[nodiscard]] std::optional<ReportLinkTarget> reportLinkTarget(const QString& link, QString& error);

// Makes selenewatch: links run this program for the current Windows user,
// without administrator rights. Does nothing on other systems. False, with
// why, when the registry cannot be written.
bool registerReportLinks(const QString& program, QString& error);
