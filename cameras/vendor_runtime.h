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
// std::runtime_error naming every candidate that failed and why. Windows'
// own error boxes are off meanwhile, so a bad DLL never waits on a click.
[[nodiscard]] void* loadVendorRuntime(const VendorRuntime& runtime, const QString& chosenPath,
    const QStringList& automaticPaths, const std::function<void(void* module)>& accept, QString& loadedPath);
void freeVendorRuntime(void* module) noexcept;

// One exported function; throws std::runtime_error when it is missing.
[[nodiscard]] void* vendorExport(void* module, const char* name);

// The runtime's file beside SeleneWatch.exe, the first automatic location.
[[nodiscard]] QString runtimeBesideProgram(const QString& fileName);

// The copies of a vendor DLL that 64-bit astronomy programs install for
// themselves, where they exist: N.I.N.A.'s External\x64\<ninaFolder>, and
// the folder of each 64-bit SharpCap ("SharpCap 4.1 (64 bit)"). Sources:
// Player One's manual on replacing its SDK DLL in other software (N.I.N.A.
// and SharpCap folders), and N.I.N.A.'s nina.external repository (its
// x64\ToupTek and x64\PlayerOne folders).
[[nodiscard]] QStringList copiesInCapturePrograms(const QString& fileName, const QString& ninaFolder);

// The given files that exist, the newest file version first; a file with no
// version information comes after those with one, in the given order.
[[nodiscard]] QStringList newestFirst(const QStringList& paths);
