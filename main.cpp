#include "mainwindow.h"

#include <QtGui/QIcon>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // The window icon, at the sizes the title bar and the task switcher use.
    QIcon icon;
    for (const int size : {16, 32, 48, 256})
    {
        icon.addFile(QString(":/icons/selenewatch-%1.png").arg(size), QSize(size, size));
    }
    app.setWindowIcon(icon);

    MainWindow window;
    window.show();
    return app.exec();
}
