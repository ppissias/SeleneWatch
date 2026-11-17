#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "cameras/vendor_runtime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <stdexcept>
#include <string>

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

void* loadVendorRuntime(const VendorRuntime& runtime, const QString& chosenPath, const QStringList& automaticPaths,
    const std::function<void(void*)>& accept, QString& loadedPath)
{
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
