#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QToolBar>

#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>
#include <editor/lua_definitions.hpp>
#include <editor/play_session.hpp>

#include "autoload_dialog.hpp"

namespace {

QString toQString(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// Play mode controls, the project's autoloads and the LuaLS definitions for its scripts.
class ScriptingModule final : public editor::IEditorModule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "Scripting";
    }

    void startup(editor::EditorContext& context) override {
        context_ = &context;
        buildPlayActions();
        buildProjectActions();
        if (std::string error; !editor::refreshLuaDefinitions(context.project(), error)) {
            spdlog::warn("{}", error);
        }
    }

    void shutdown() override {
        // The menus and toolbar outlive the module, and these actions call into its code.
        for (QAction* action : {play_, pause_, stop_, autoload_, refresh_definitions_}) {
            delete action;
        }
    }

private:
    void buildPlayActions() {
        editor::PlaySession& session = context_->playSession();
        QMenu* menu = context_->menu(QObject::tr("&Engine"));
        play_ = menu->addAction(QObject::tr("Play Current Scene"));
        play_->setShortcut(QKeySequence(Qt::Key_F5));
        pause_ = menu->addAction(QObject::tr("Pause"));
        pause_->setCheckable(true);
        stop_ = menu->addAction(QObject::tr("Stop"));
        stop_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
        for (QAction* action : {play_, pause_, stop_}) {
            context_->toolBar()->addAction(action);
        }

        QObject::connect(play_, &QAction::triggered, play_, [this] { play(); });
        QObject::connect(stop_, &QAction::triggered, stop_, [&session] { session.stop(); });
        QObject::connect(pause_, &QAction::toggled, pause_, [&session](const bool paused) { session.setPaused(paused); });
        QObject::connect(&session, &editor::PlaySession::playingChanged, play_, [this] { refreshPlayActions(); });
        QObject::connect(&session, &editor::PlaySession::pausedChanged, pause_, [this] { refreshPlayActions(); });
        refreshPlayActions();
    }

    void buildProjectActions() {
        QMenu* menu = context_->menu(QObject::tr("&Project"));
        autoload_ = menu->addAction(QObject::tr("&Autoload..."));
        refresh_definitions_ = menu->addAction(QObject::tr("Refresh Lua Definitions"));
        QObject::connect(autoload_, &QAction::triggered, autoload_, [this] { editAutoloads(); });
        // Play runs with the autoloads it started with.
        QObject::connect(&context_->playSession(), &editor::PlaySession::playingChanged, autoload_,
                         &QAction::setDisabled);
        QObject::connect(refresh_definitions_, &QAction::triggered, refresh_definitions_, [this] {
            std::string error;
            if (!editor::refreshLuaDefinitions(context_->project(), error)) {
                QMessageBox::warning(context_->mainWindow(), QObject::tr("Lua completion"), toQString(error));
                return;
            }
            context_->showStatus(
                QObject::tr("Lua definitions updated. Open the project folder in VS Code for completion."), 6000);
        });
    }

    void refreshPlayActions() {
        const bool playing = context_->playSession().isPlaying();
        play_->setEnabled(!playing);
        stop_->setEnabled(playing);
        pause_->setEnabled(playing);
        const QSignalBlocker blocker(pause_);
        pause_->setChecked(context_->playSession().isPaused());
    }

    void play() {
        if (std::string error; !context_->playSession().play(error)) {
            QMessageBox::warning(context_->mainWindow(), QObject::tr("Play"), toQString(error));
        }
    }

    void editAutoloads() {
        engine::Engine& engine = context_->engine();
        if (!engine.IsInitialized()) {
            QMessageBox::warning(context_->mainWindow(), QObject::tr("Autoload"),
                                 QObject::tr("The engine is not running, see the Log."));
            return;
        }
        if (context_->playSession().isPlaying()) return;
        editor::AutoloadDialog dialog(engine, context_->project(), context_->mainWindow());
        while (dialog.exec() == QDialog::Accepted) {
            if (context_->playSession().isPlaying()) return;
            // Work from the current project: a scene operation may have adopted a startup scene.
            engine::Project updated = context_->project();
            updated.autoloads = dialog.definitions();
            std::string error;
            if (!context_->updateProject(std::move(updated), error)) {
                QMessageBox::warning(context_->mainWindow(), QObject::tr("Autoload"), toQString(error));
                continue;
            }
            if (!editor::refreshLuaDefinitions(context_->project(), error)) {
                spdlog::warn("{}", error);
                QMessageBox::warning(context_->mainWindow(), QObject::tr("Lua completion"), toQString(error));
            }
            return;
        }
    }

    editor::EditorContext* context_ = nullptr;
    QAction* play_ = nullptr;
    QAction* pause_ = nullptr;
    QAction* stop_ = nullptr;
    QAction* autoload_ = nullptr;
    QAction* refresh_definitions_ = nullptr;
};

}  // namespace

SRI_EDITOR_MODULE(ScriptingModule)
