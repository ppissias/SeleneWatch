#include "mainwindow.h"
#include "recording/nightly_report.h"
#include <QtCore/QDebug>
#include "recording/output_paths.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args[1] == "--nightly-report")
    {
        QString error;
        if (writeNightlyReport(args[2], error).isEmpty()) { qCritical().noquote() << error; return 1; }
        return 0;
    }
    // These names identify the persistent Qt settings for camera, detection and display preferences.
    // SELENEWATCH_SETTINGS_NAME gives a run settings of its own: the package
    // check starts the packaged program without the observer's settings, so
    // that, for example, Connect at startup does not open their camera.
    QCoreApplication::setOrganizationName("SeleneWatch");
    const QString settingsName = qEnvironmentVariable("SELENEWATCH_SETTINGS_NAME");
    QCoreApplication::setApplicationName(settingsName.isEmpty() ? QString("SeleneWatch") : settingsName);

    // Optional portable mode: a SeleneWatch.ini file beside the executable takes
    // priority over the per-user registry store when not running isolated tests.
    if (settingsName.isEmpty())
    {
        const QString portableIni = QDir(QCoreApplication::applicationDirPath()).filePath("SeleneWatch.ini");
        if (QFileInfo::exists(portableIni))
        {
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::applicationDirPath());
        }
    }
    migrateOutputSettings();

    // The window icon, at the sizes the title bar and the task switcher use.
    QIcon icon;
    for (const int size : {16, 32, 48, 256})
    {
        icon.addFile(QString(":/icons/selenewatch-%1.png").arg(size), QSize(size, size));
    }
    app.setWindowIcon(icon);

    MainWindow window;
    window.show();
    QTimer::singleShot(0, &window, &MainWindow::maybeShowStartupTips);
    // Package/GUI checks use an isolated settings name and do not contact GitHub.
    if (settingsName.isEmpty()) QTimer::singleShot(1000, &window, &MainWindow::checkForUpdates);
    return app.exec();
}
