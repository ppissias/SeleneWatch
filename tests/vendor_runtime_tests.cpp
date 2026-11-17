#include "cameras/vendor_runtime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {
void touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not a DLL");
}
QString native(const QString& path) { return QDir::toNativeSeparators(path); }
// Sets an environment variable for one scope.
class ScopedVariable
{
public:
    ScopedVariable(const char* name, const QString& value) : name_(name), had_(qEnvironmentVariableIsSet(name)), old_(qgetenv(name))
    {
        qputenv(name, value.toLocal8Bit());
    }
    ~ScopedVariable() { if (had_) qputenv(name_, old_); else qunsetenv(name_); }
private:
    const char* name_;
    bool had_;
    QByteArray old_;
};
} // namespace

// Where a vendor DLL the observer installs is looked for, and how the loader
// picks one. No vendor code is loaded: the files are fakes or Windows' own.
class VendorRuntimeTest : public QObject
{
    Q_OBJECT
private slots:
    void otherProgramsCopiesAreFound();
    void theNewestVersionComesFirst();
    void theLoaderSkipsWhatItCannotUse();
};

void VendorRuntimeTest::otherProgramsCopiesAreFound()
{
    QTemporaryDir programFiles;
    const QDir root(programFiles.path());
    const QString nina = "N.I.N.A. - Nighttime Imaging 'N' Astronomy/External/x64/";
    touch(root.filePath(nina + "ToupTek/toupcam.dll"));
    touch(root.filePath(nina + "PlayerOne/PlayerOneCamera.dll"));
    touch(root.filePath("SharpCap 4.1 (64 bit)/toupcam.dll"));
    touch(root.filePath("SharpCap 4.2 (64 bit)/toupcam.dll"));
    touch(root.filePath("SharpCap 4.2 (64 bit)/PlayerOneCamera.dll"));
    touch(root.filePath("SharpCap 3.2/toupcam.dll")); // a 32-bit SharpCap: not usable by SeleneWatch
    const ScopedVariable w6432("ProgramW6432", programFiles.path());
    const ScopedVariable files("ProgramFiles", programFiles.path());

    QCOMPARE(copiesInCapturePrograms("toupcam.dll", "ToupTek"),
        (QStringList{native(root.filePath(nina + "ToupTek/toupcam.dll")),
            native(root.filePath("SharpCap 4.2 (64 bit)/toupcam.dll")),
            native(root.filePath("SharpCap 4.1 (64 bit)/toupcam.dll"))}));
    QCOMPARE(copiesInCapturePrograms("PlayerOneCamera.dll", "PlayerOne"),
        (QStringList{native(root.filePath(nina + "PlayerOne/PlayerOneCamera.dll")),
            native(root.filePath("SharpCap 4.2 (64 bit)/PlayerOneCamera.dll"))}));
    QVERIFY(copiesInCapturePrograms("missing.dll", "Nobody").isEmpty());
}

void VendorRuntimeTest::theNewestVersionComesFirst()
{
    QTemporaryDir directory;
    const QString first = directory.filePath("first.dll");
    const QString second = directory.filePath("second.dll");
    touch(first);
    touch(second);
    const QString windows = QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/kernel32.dll");
    QVERIFY(QFileInfo(windows).isFile());
    // A file with a version comes before those without, which keep their order; a missing file is left out.
    QCOMPARE(newestFirst({first, directory.filePath("missing.dll"), windows, second}), (QStringList{windows, first, second}));
}

void VendorRuntimeTest::theLoaderSkipsWhatItCannotUse()
{
    QTemporaryDir directory;
    const QString fake = directory.filePath("vendor.dll");
    touch(fake);
    const QString windows = QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/version.dll");
    const VendorRuntime runtime{"TEST", "vendor.dll", "Install the TEST runtime."};
    QString loaded;

    // Not a DLL, then a DLL without the needed export: both refused, each with
    // its reason, and with Windows' error boxes off throughout (a box would
    // wait for a click, as one did under CTest before they were turned off).
    bool quiet = true;
    try
    {
        (void)loadVendorRuntime(runtime, {}, {fake, windows}, [&quiet](void* module) {
            quiet = quiet && (GetThreadErrorMode() & SEM_FAILCRITICALERRORS) != 0;
            (void)vendorExport(module, "NoSuchExport");
        }, loaded);
        QFAIL("expected the load to fail");
    }
    catch (const std::runtime_error& ex)
    {
        const QString message = QString::fromUtf8(ex.what());
        QVERIFY2(message.contains("Cannot load a compatible 64-bit TEST runtime"), qPrintable(message));
        QVERIFY(message.contains(native(fake)) && message.contains(native(windows)));
        QVERIFY(message.contains("missing SDK export NoSuchExport"));
        QVERIFY(message.endsWith("Install the TEST runtime."));
    }
    QVERIFY(quiet);
    QVERIFY(loaded.isEmpty());

    // The first usable candidate is taken.
    void* module = loadVendorRuntime(runtime, {}, {directory.filePath("missing.dll"), fake, windows},
        [](void* candidate) { (void)vendorExport(candidate, "GetFileVersionInfoW"); }, loaded);
    QVERIFY(module);
    QCOMPARE(loaded, native(windows));
    freeVendorRuntime(module);

    // A chosen path replaces the automatic ones and is named when missing.
    try
    {
        (void)loadVendorRuntime(runtime, directory.filePath("chosen.dll"), {windows}, [](void*) {}, loaded);
        QFAIL("expected the load to fail");
    }
    catch (const std::runtime_error& ex)
    {
        QVERIFY(QString::fromUtf8(ex.what()).contains("TEST runtime file unavailable at " + native(directory.filePath("chosen.dll"))));
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    VendorRuntimeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "vendor_runtime_tests.moc"
