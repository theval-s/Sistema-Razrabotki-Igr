#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>
#include <editor/play_session.hpp>
#include <editor/selection.hpp>

#include <engine/ai/BehaviorTreeComponent.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>

#include "tree_panel.hpp"

namespace {

const QString UnsavedCheckId = QStringLiteral("behavior-tree");

/// The running game as the panel follows it.
behavior::TreePanel::Game playingGame(editor::EditorContext& context) {
    const auto* scripts = context.engine().GetSubsystem<engine::ScriptSubsystem>();
    if (scripts == nullptr) {
        return {};
    }
    return {[&context](const engine::AssetUuid& tree) {
                std::vector<behavior::TreePanel::Agent> agents;
                const engine::Scene& scene = context.scene();
                const entt::entity selected = context.selection().primary();
                scene.ForEach<engine::ai::BehaviorTreeComponent>(
                    [&](const entt::entity entity, const engine::ai::BehaviorTreeComponent& component) {
                        if (component.tree.id == tree) {
                            agents.push_back({entity, QString::fromStdString(scene.GetName(entity)), entity == selected});
                        }
                    });
                return agents;
            },
            [scripts](const entt::entity entity) { return scripts->GetRuntime().GetBehaviorAgentState(entity); }};
}

/// Editors for the engine's AI: the Behavior Tree panel, which opens .bt.lua assets from Content.
class AiModule final : public editor::IEditorModule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "AI";
    }

    void startup(editor::EditorContext& context) override {
        context_ = &context;
        auto* panel = new behavior::TreePanel(context.engine().GetAssets().GetCatalog());
        context.addPanel(QStringLiteral("BehaviorTreePanel"), QObject::tr("Behavior Tree"), panel,
                         Qt::BottomDockWidgetArea);
        context.setAssetEditor(engine::CatalogAssetKind::BehaviorTree,
                               [&context, panel](const std::filesystem::path& source) {
            context.showPanel(panel);
            panel->open(source);
        });
        context.setUnsavedCheck(UnsavedCheckId, [panel](QWidget* parent) { return panel->maybeSave(parent); });
        QObject::connect(&context.playSession(), &editor::PlaySession::playingChanged, panel,
                         [&context, panel](const bool playing) {
                             panel->setGame(playing ? playingGame(context) : behavior::TreePanel::Game());
                         });
    }

    void shutdown() override {
        context_->setAssetEditor(engine::CatalogAssetKind::BehaviorTree, {});
        context_->setUnsavedCheck(UnsavedCheckId, {});
    }

private:
    editor::EditorContext* context_ = nullptr;
};

}  // namespace

SRI_EDITOR_MODULE(AiModule)
