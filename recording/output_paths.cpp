#include "recording/output_paths.h"
#include "recording/metadata_text.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryFile>

namespace {

bool hasCloudSegment(const QString& cleanPath)
{
    const QString lower = cleanPath.toLower();
    for (const char* marker : {"/onedrive", "/dropbox", "/google drive", "/googledrive", "/iclouddrive"})
    {
        if (lower.contains(marker))
        {
            return true;
        }
    }
    return false;
}

bool mountExistsFor(const QString& cleanPath)
{
    QString at = cleanPath;
    for (;;)
    {
        if (QFileInfo::exists(at))
        {
            return true;
        }
        const QString parent = QFileInfo(at).path();
        if (parent == at || parent.isEmpty())
        {
            return QFileInfo::exists(at);
        }
        at = parent;
    }
}

} // namespace

QString eventsFolder(const QString& root, Target target)
{
    return QDir(root).filePath(QString("events/%1").arg(targetName(target)));
}

QString recordingsFolder(const QString& root)
{
    return QDir(root).filePath("recordings");
}

QString timelapsesFolder(const QString& root)
{
    return QDir(root).filePath("timelapses");
}

QString defaultOutputFolder()
{
    QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (home.isEmpty())
    {
        home = QDir::homePath();
    }
    const QString candidate = QDir::cleanPath(QDir(home).filePath("SeleneWatch"));
    if (!isCloudSyncedPath(candidate))
    {
        return candidate;
    }
    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir::cleanPath(QDir(local.isEmpty() ? home : local).filePath("SeleneWatch"));
}

bool isCloudSyncedPath(const QString& path)
{
    if (path.isEmpty())
    {
        return false;
    }
    const QString clean = QDir::cleanPath(QDir(path).absolutePath());
    for (const char* env : {"OneDrive", "OneDriveCommercial", "OneDriveConsumer"})
    {
        const QString root = qEnvironmentVariable(env);
        if (!root.isEmpty())
        {
            const QString cleanRoot = QDir::cleanPath(QDir(root).absolutePath());
            if (clean.compare(cleanRoot, Qt::CaseInsensitive) == 0
                || clean.startsWith(cleanRoot + '/', Qt::CaseInsensitive))
            {
                return true;
            }
        }
    }
    return hasCloudSegment(clean);
}

OutputFolderCheck checkOutputFolder(const QString& root, uint64_t reserveBytes, bool createFolder,
    const FreeSpaceQuery& freeSpace)
{
    OutputFolderCheck result;
    const QString trimmed = root.trimmed();
    if (trimmed.isEmpty())
    {
        result.error = "No output folder is configured.";
        return result;
    }

    const QString clean = QDir::cleanPath(QDir(trimmed).absolutePath());
    if (!mountExistsFor(clean))
    {
        result.mountMissing = true;
        result.error = QString("The drive or location for %1 is not available.").arg(QDir::toNativeSeparators(clean));
        return result;
    }

    const QFileInfo info(clean);
    if (!info.exists())
    {
        if (!createFolder)
        {
            result.folderMissing = true;
            result.error = QString("The output folder %1 does not exist.").arg(QDir::toNativeSeparators(clean));
            return result;
        }
        if (!QDir().mkpath(clean))
        {
            result.notWritable = true;
            result.error = QString("SeleneWatch could not create %1.").arg(QDir::toNativeSeparators(clean));
            return result;
        }
    }
    else if (!info.isDir())
    {
        result.notWritable = true;
        result.error = QString("%1 is not a folder.").arg(QDir::toNativeSeparators(clean));
        return result;
    }

    QTemporaryFile probe(QDir(clean).filePath("selenewatch-write-check-XXXXXX"));
    if (!probe.open())
    {
        result.notWritable = true;
        result.error = QString("SeleneWatch cannot write in %1.").arg(QDir::toNativeSeparators(clean));
        return result;
    }
    probe.remove();

    const FreeSpaceQuery& query = freeSpace ? freeSpace : FreeSpaceQuery(freeBytesAt);
    result.freeBytes = query(clean);
    if (reserveBytes > 0 && result.freeBytes && *result.freeBytes < reserveBytes)
    {
        result.belowReserve = true;
    }
    result.ok = true;
    return result;
}

void migrateOutputSettings()
{
    QSettings saved;
    const int version = saved.value("settingsVersion", 0).toInt();
    if (version >= kCurrentSettingsVersion)
    {
        return;
    }
    // Preserve any legacy recordings folder note once, then consolidate onto
    // the single output/folder root.
    if (saved.contains("recording/folder"))
    {
        const QString legacyRecording = saved.value("recording/folder").toString();
        if (!legacyRecording.isEmpty() && !saved.contains("output/legacyRecordingFolder"))
        {
            saved.setValue("output/legacyRecordingFolder", legacyRecording);
        }
        saved.remove("recording/folder");
    }
    saved.setValue("settingsVersion", kCurrentSettingsVersion);
    saved.sync();
}
