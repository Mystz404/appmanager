#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setWindowIcon(QIcon(QStringLiteral(":/resources/appmanager.png")));
    MainWindow w;
    w.show();
    return a.exec();
}
