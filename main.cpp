#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
#ifdef Q_OS_ANDROID
    w.showMaximized();   // en el teléfono la ventana ocupa toda la pantalla
#else
    w.resize(420, 820);  // en escritorio se simula un teléfono vertical
    w.show();
#endif
    return a.exec();
}
