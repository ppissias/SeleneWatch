#include "tools/qhy_probe.h"
#include <QtGui/QGuiApplication>
int main(int argc,char** argv)
{
    qputenv("QT_QPA_PLATFORM","offscreen");QGuiApplication app(argc,argv);
    return runQhyProbe(app,app.arguments(),makeQhyApi());
}
