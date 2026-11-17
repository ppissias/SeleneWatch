#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

// A camera vendor's SDK library that the observer installs, loaded at run
// time by absolute path, never from the DLL search path; its dependencies
// are looked for beside it and in System32. QHY, Player One and ToupTek
// use it: their DLLs are not shipped with SeleneWatch (THIRD_PARTY_LICENSES.md).
struct VendorRuntime
{
    QString vendor;       // "QHY", for messages
    QString fileName;     // "qhyccd.dll"
    QString installHint;  // what to install, or which DLL to select
};

// Loads the first candidate that is a file, loads, and passes accept, which
// resolves the adapter's functions and throws if one is missing. Candidates
// are the observer's chosen DLL when there is one, otherwise the automatic
// locations. Returns the module (an HMODULE) and sets loadedPath; throws
// std::runtime_error naming every candidate that failed and why.
[[nodiscard]] void* loadVendorRuntime(const VendorRuntime& runtime, const QString& chosenPath,
    const QStringList& automaticPaths, const std::function<void(void* module)>& accept, QString& loadedPath);
void freeVendorRuntime(void* module) noexcept;

// One exported function; throws std::runtime_error when it is missing.
[[nodiscard]] void* vendorExport(void* module, const char* name);

// The runtime's file beside SeleneWatch.exe, the first automatic location.
[[nodiscard]] QString runtimeBesideProgram(const QString& fileName);
