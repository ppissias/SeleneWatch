// Loads a camera family's SDK runtime, the DLL the observer installed, and
// reports where it was found, its version and the cameras it sees. Nothing is
// opened or streamed. Run by hand:
//
//   SeleneWatchRuntimeProbe qhy|playerone|touptek [path to the DLL]
//
// Without a path the family's automatic locations are tried, as in the window.
#include "cameras/playerone_api.h"
#include "cameras/qhy_api.h"
#include "cameras/touptek_api.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTextStream>

#include <stdexcept>

namespace {
template<class Api, class List> int report(const std::shared_ptr<Api>& api, const QString& path, List list)
{
    if (!api) throw std::runtime_error("This build has no SDK header for that camera family");
    api->setRuntimeOverride(path);
    QTextStream out(stdout);
    const auto version = api->version();
    out << "runtime: " << QDir::toNativeSeparators(api->loadedRuntimePath()) << "\n";
    out << "sdk_version: " << QString::fromStdString(version) << "\n";
    const QStringList cameras = list(*api);
    out << "cameras: " << cameras.size() << "\n";
    for (const auto& camera : cameras) out << "  " << camera << "\n";
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    QTextStream error(stderr);
    if (args.size() < 2 || args.size() > 3)
    {
        error << "usage: SeleneWatchRuntimeProbe qhy|playerone|touptek [path to the DLL]\n";
        return 2;
    }
    const QString path = args.size() == 3 ? QDir(args[2]).absolutePath() : QString();
    try
    {
        if (args[1] == "qhy")
            return report(makeQhyApi(), path, [](QhyApi& api) {
                QStringList names;
                for (const auto& d : api.devices()) names << QString::fromStdString(d.name + " (" + d.id + ")");
                return names;
            });
        if (args[1] == "playerone")
            return report(makePlayerOneApi(), path, [](PlayerOneApi& api) {
                QStringList names;
                for (const auto& c : api.cameras()) names << QString::fromStdString(c.name + " (serial " + c.serial + ")");
                return names;
            });
        if (args[1] == "touptek")
            return report(makeToupTekApi(), path, [](ToupTekApi& api) {
                QStringList names;
                for (const auto& d : api.devices()) names << QString::fromStdString(d.name + " (" + d.id + ")");
                return names;
            });
        error << "unknown camera family: " << args[1] << "\n";
        return 2;
    }
    catch (const std::exception& ex)
    {
        error << ex.what() << "\n";
        return 1;
    }
}
