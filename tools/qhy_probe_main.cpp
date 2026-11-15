#include "tools/qhy_probe.h"
#include <QtGui/QGuiApplication>
// Runs the QHY diagnostic with an offscreen Qt context for clip preview images.
int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");QGuiApplication app(argc,argv);
    return runQhyProbe(app,app.arguments(),makeQhyApi());
}
