#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cameras/vendor_runtime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>

namespace {
QString libraryError(DWORD error)
{
    if (error == ERROR_BAD_EXE_FORMAT) return "expected an x64 Windows DLL";
    if (error == ERROR_MOD_NOT_FOUND) return "Windows could not load the DLL or a required component";
    return QString("Windows error %1").arg(error);
}
} // namespace

// While trying candidates, Windows reports a bad or unloadable DLL as an error
// code instead of a modal "Bad Image" box that would wait for someone to
// click it. Restores this thread's previous error mode when done.
class QuietLoaderErrors
{
public:
    QuietLoaderErrors() { SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_); }
    ~QuietLoaderErrors() { SetThreadErrorMode(previous_, nullptr); }
    QuietLoaderErrors(const QuietLoaderErrors&) = delete;
    QuietLoaderErrors& operator=(const QuietLoaderErrors&) = delete;
private:
    DWORD previous_{0};
};

void* loadVendorRuntime(const VendorRuntime& runtime, const QString& chosenPath, const QStringList& automaticPaths,
    const std::function<void(void*)>& accept, QString& loadedPath)
{
    const QuietLoaderErrors quiet;
    const QStringList paths = chosenPath.isEmpty() ? automaticPaths : QStringList{chosenPath};
    QStringList failedLoads;
    for (const auto& path : paths)
    {
        if (!QDir::isAbsolutePath(path) || !QFileInfo(path).isFile()) continue;
        const auto nativePath = QDir::toNativeSeparators(path);
        const HMODULE module = LoadLibraryExW(nativePath.toStdWString().c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
        {
            failedLoads << QString("%1 (%2)").arg(nativePath, libraryError(GetLastError()));
            continue;
        }
        try
        {
            accept(module);
        }
        catch (const std::exception& ex)
        {
            FreeLibrary(module);
            failedLoads << QString("%1 (%2)").arg(nativePath, QString::fromUtf8(ex.what()));
            continue;
        }
        loadedPath = nativePath;
        return module;
    }

    QString error;
    if (!failedLoads.isEmpty())
        error = QString("Cannot load a compatible 64-bit %1 runtime.\n").arg(runtime.vendor) + failedLoads.join('\n');
    else if (!chosenPath.isEmpty())
        error = QString("%1 runtime file unavailable at %2.").arg(runtime.vendor, QDir::toNativeSeparators(chosenPath));
    else
        error = QString("%1 runtime (64-bit %2) was not found.").arg(runtime.vendor, runtime.fileName);
    error += "\n" + runtime.installHint;
    throw std::runtime_error(error.toStdString());
}

void freeVendorRuntime(void* module) noexcept
{
    if (module) FreeLibrary(static_cast<HMODULE>(module));
}

void* vendorExport(void* module, const char* name)
{
    const auto pointer = GetProcAddress(static_cast<HMODULE>(module), name);
    if (!pointer) throw std::runtime_error(std::string("missing SDK export ") + name);
    return reinterpret_cast<void*>(pointer);
}
#else
void* loadVendorRuntime(const VendorRuntime& runtime, const QString&, const QStringList&,
    const std::function<void(void*)>&, QString&)
{
    throw std::runtime_error((runtime.vendor + " runtimes are loaded on Windows only").toStdString());
}
void freeVendorRuntime(void*) noexcept {}
void* vendorExport(void*, const char* name) { throw std::runtime_error(std::string("missing SDK export ") + name); }
#endif

QString runtimeBesideProgram(const QString& fileName)
{
    const auto directory = QCoreApplication::applicationDirPath();
    return directory.isEmpty() ? QString() : QDir(directory).filePath(fileName);
}

QStringList copiesInCapturePrograms(const QString& fileName, const QString& ninaFolder)
{
    // The 64-bit Program Files folder, by either name a 64-bit process sees.
    QStringList programFiles;
    for (const auto* variable : {"ProgramW6432", "ProgramFiles"})
        if (const auto folder = qEnvironmentVariable(variable); !folder.isEmpty()) programFiles << QDir::cleanPath(folder);
    programFiles.removeDuplicates();

    QStringList paths;
    for (const auto& folder : programFiles)
    {
        const QDir root(folder);
        paths << root.filePath(QString("N.I.N.A. - Nighttime Imaging 'N' Astronomy/External/x64/%1/%2").arg(ninaFolder, fileName));
        for (const auto& sharpCap : root.entryList({"SharpCap*"}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
            if (sharpCap.contains("(64 bit)", Qt::CaseInsensitive)) paths << root.filePath(sharpCap + "/" + fileName);
    }
    QStringList found;
    for (const auto& path : paths)
        if (QFileInfo(path).isFile()) found << QDir::toNativeSeparators(path);
    return found;
}

namespace {
// A file's version as one number, major part first; zero when it has none.
uint64_t fileVersion(const QString& path)
{
#ifdef _WIN32
    const std::wstring name = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(name.c_str(), &ignored);
    if (size == 0) return 0;
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(name.c_str(), 0, size, data.data())) return 0;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info || length < sizeof *info) return 0;
    return (static_cast<uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
#else
    (void)path;
    return 0;
#endif
}
} // namespace

QStringList newestFirst(const QStringList& paths)
{
    std::vector<std::pair<uint64_t, QString>> files;
    for (const auto& path : paths)
        if (QFileInfo(path).isFile()) files.emplace_back(fileVersion(path), path);
    std::stable_sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    QStringList result;
    for (const auto& file : files) result << file.second;
    return result;
}
