#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName("ffrog");
  QCoreApplication::setApplicationVersion("1.7");
  QApplication::setApplicationDisplayName("ffrog v1.7 - The Frogmat utility");
  MainWindow w;
  w.show();
  return app.exec();
}
