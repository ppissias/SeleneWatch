#include "tools/camera_probe.h"
#include <QtGui/QGuiApplication>

// Runs the ASI diagnostic with an offscreen Qt context for clip preview images.
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    return runCameraProbe(app, app.arguments(), makeZwoApi());
}
