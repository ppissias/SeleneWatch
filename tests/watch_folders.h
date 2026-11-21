#pragma once
#include <QtCore/QDir>

// Resolve the actual independently named watches created by a Session in a test.
inline QString latestWatch(const QString& targetFolder)
{
    const QDir parent(targetFolder);
    const auto names = parent.entryList({"session-*"}, QDir::Dirs, QDir::Name);
    return parent.filePath(names.isEmpty() ? "missing-session" : names.back());
}

inline QStringList watchClips(const QString& targetFolder)
{
    QStringList result;
    for (const auto& watch : QDir(targetFolder).entryList({"session-*"}, QDir::Dirs, QDir::Name))
        for (const auto& clip : QDir(targetFolder + "/" + watch + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            result << watch + "/clips/" + clip;
    return result;
}
