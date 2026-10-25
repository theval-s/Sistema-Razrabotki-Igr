#include <QApplication>
#include <QDir>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <editor/main_window.hpp>

namespace {

void addFileLog() {
    //logging in the exe path instead of cwd
    const QString path = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/logs/editor.log");
    try {
        // spdlog opens narrow paths with the ANSI code page on Windows, hence local 8-bit.
        spdlog::default_logger()->sinks().push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path.toLocal8Bit().toStdString(), 5 * 1024 * 1024, 3));
        spdlog::flush_on(spdlog::level::warn);
    } catch (const spdlog::spdlog_ex& e) {
        spdlog::warn("Could not open log file '{}': {}", path.toStdString(), e.what());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    // The viewport is a native window; without this Qt makes all of its siblings native
    // too, which the docking system's splitters and drag overlays handle badly.
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("PEN_I_SOSNA"));
    QCoreApplication::setApplicationName(QStringLiteral("SRI Editor"));

    // Before the window starts the engine and its threads, which log to the same logger.
    addFileLog();

    editor::MainWindow window;
    window.show();

    return QApplication::exec();
}
