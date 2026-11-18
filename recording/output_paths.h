#pragma once

#include "capture_settings.h"

#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <optional>

// Feature-17: single remembered output root and fixed subfolders underneath.
// Everything SeleneWatch writes lives under one root:
//   <root>/events/moon/
//   <root>/events/sky/
//   <root>/recordings/
//   <root>/timelapses/
inline constexpr int kCurrentSettingsVersion = 1;

[[nodiscard]] QString eventsFolder(const QString& root, Target target);
[[nodiscard]] QString recordingsFolder(const QString& root);
[[nodiscard]] QString timelapsesFolder(const QString& root);

// Non-cloud-synced local default for first run (for example %USERPROFILE%\SeleneWatch).
[[nodiscard]] QString defaultOutputFolder();

// True when path appears to sit inside OneDrive, Dropbox, Google Drive, or iCloud Drive.
[[nodiscard]] bool isCloudSyncedPath(const QString& path);

struct OutputFolderCheck
{
    bool ok{false};
    bool mountMissing{false};
    bool folderMissing{false};
    bool notWritable{false};
    bool belowReserve{false};
    std::optional<uint64_t> freeBytes;
    QString error;
};

using FreeSpaceQuery = std::function<std::optional<uint64_t>(const QString& folder)>;

// Validates that root exists (or creates it when createFolder is true), can be
// written, and checks free space against reserveBytes. Never silently falls
// back to another drive or directory.
[[nodiscard]] OutputFolderCheck checkOutputFolder(const QString& root, uint64_t reserveBytes,
    bool createFolder = false, const FreeSpaceQuery& freeSpace = {});

// Migrates QSettings keys to kCurrentSettingsVersion (e.g., removing legacy
// Feature-14 recording/folder overrides).
void migrateOutputSettings();
