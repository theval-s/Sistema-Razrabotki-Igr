#include <QApplication>

#include <editor/main_window.hpp>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("PEN_I_SOSNA"));
    QCoreApplication::setApplicationName(QStringLiteral("SRI Editor"));

    editor::MainWindow window;
    window.show();

    return QApplication::exec();
}
