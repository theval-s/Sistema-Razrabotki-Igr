#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>

#include "tree_panel.hpp"

namespace {

const QString UnsavedCheckId = QStringLiteral("behavior-tree");

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
