#include <QApplication>
#include <QCoreApplication>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("kinmap");
    QCoreApplication::setApplicationVersion("0.2");
    QCoreApplication::setOrganizationName("we6jbo");

    MainWindow window;
    window.show();

    return app.exec();
}
