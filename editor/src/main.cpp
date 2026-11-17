#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>

#include <optional>
#include <string>
#include <utility>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/project/Project.hpp>

#include <editor/main_window.hpp>
#include <editor/paths.hpp>
#include <editor/project_dialog.hpp>
#include <editor/session_log.hpp>

namespace {

/// `file` if it loads, otherwise whatever the user picks in the project dialog; empty if they quit.
std::optional<engine::Project> chooseProject(QString file) {
    for (;;) {
        if (file.isEmpty()) {
            editor::ProjectDialog dialog;
            if (dialog.exec() != QDialog::Accepted) {
                return std::nullopt;
            }
            file = dialog.projectFile();
        }
        std::string error;
        if (auto project = engine::Project::Load(editor::toPath(file), error)) {
            editor::addRecentProject(file);
            return project;
        }
        QMessageBox::warning(nullptr, QObject::tr("Open Project"), QString::fromStdString(error));
        file.clear();
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
    editor::initializeSessionLog();

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption project_option(QStringLiteral("project"), QObject::tr("Project file to open."),
                                            QStringLiteral("file"));
    parser.addOption(project_option);
    parser.process(app);

    std::optional<engine::Project> project = chooseProject(parser.value(project_option));
    if (!project) {
        return 0;
    }
    editor::logSession(editor::toQString(project->file));
    // Before the engine exists: nothing may resolve a game/... name until this is set.
    engine::AssetPathResolver::SetGameRoot(project->ContentDir());

    editor::MainWindow window(std::move(*project));
    window.show();

    return QApplication::exec();
}
