#include <QAction>
#include <QMenu>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>

#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>
#include <editor/selection.hpp>

#include "inspector_panel.hpp"

namespace {

/// Lists the scene's entities as a tree, rebuilt whenever the scene's revision changes.
/// Its selection mirrors the editor's Selection both ways; the current item is the primary.
class HierarchyPanel : public QTreeWidget {
public:
    HierarchyPanel(engine::Scene& scene, editor::Selection& selection) : scene_(scene), selection_(selection) {
        setHeaderHidden(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        // A click changes the selection before the current item, so both push.
        connect(this, &QTreeWidget::itemSelectionChanged, this, &HierarchyPanel::pushSelection);
        connect(this, &QTreeWidget::currentItemChanged, this, &HierarchyPanel::pushSelection);
        connect(&selection_, &editor::Selection::changed, this, &HierarchyPanel::showSelection);
        // Scene has no change signal, only a revision counter: poll it.
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &HierarchyPanel::refresh);
        timer->start(200);
        rebuild();
    }

private:
    static entt::entity entityOf(const QTreeWidgetItem* item) {
        return item != nullptr ? static_cast<entt::entity>(item->data(0, Qt::UserRole).toUInt()) : entt::null;
    }

    void refresh() {
        if (scene_.GetRevision() != shown_revision_) {
            rebuild();
        }
    }

    void rebuild() {
        shown_revision_ = scene_.GetRevision();
        // Rebuilding isn't a selection change: the selection is restored, quietly.
        const QSignalBlocker blocker(this);
        clear();
        items_.clear();
        for (const entt::entity root : scene_.GetRootEntities()) {
            addTopLevelItem(makeItem(root));
        }
        expandAll();
        showSelection();
    }

    /// Tree -> Selection.
    void pushSelection() {
        std::vector<entt::entity> entities;
        QTreeWidgetItem* current = currentItem();
        for (const QTreeWidgetItem* item : selectedItems()) {
            if (item != current) {
                entities.push_back(entityOf(item));
            }
        }
        if (current != nullptr && current->isSelected()) {
            entities.push_back(entityOf(current));
        }
        pushing_ = true;
        selection_.setEntities(entities);
        pushing_ = false;
    }

    /// Selection -> tree.
    void showSelection() {
        if (pushing_) {
            return;  // came from the tree, which already shows it
        }
        if (scene_.GetRevision() != shown_revision_) {
            rebuild();  // the selection may hold entities the tree doesn't have yet
            return;
        }
        const QSignalBlocker blocker(this);
        clearSelection();
        for (const entt::entity entity : selection_.entities()) {
            if (QTreeWidgetItem* item = findItem(entity)) {
                item->setSelected(true);
            }
        }
        if (QTreeWidgetItem* primary = findItem(selection_.primary())) {
            setCurrentItem(primary, 0, QItemSelectionModel::NoUpdate);
            scrollToItem(primary);
        }
    }

    QTreeWidgetItem* makeItem(const entt::entity entity) {
        auto* item = new QTreeWidgetItem({QString::fromStdString(scene_.GetName(entity))});
        item->setData(0, Qt::UserRole, entt::to_integral(entity));
        items_.emplace(entity, item);
        for (const entt::entity child : scene_.GetChildren(entity)) {
            item->addChild(makeItem(child));
        }
        return item;
    }

    QTreeWidgetItem* findItem(const entt::entity entity) const {
        const auto it = items_.find(entity);
        return it != items_.end() ? it->second : nullptr;
    }

    engine::Scene& scene_;
    editor::Selection& selection_;
    /// Every item of the tree, by entity. Owned by the tree.
    std::unordered_map<entt::entity, QTreeWidgetItem*> items_;
    std::uint64_t shown_revision_ = ~std::uint64_t{0};
    /// Set while the tree's own selection is being pushed, so it isn't shown back.
    bool pushing_ = false;
};

class SceneModule final : public editor::IEditorModule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "Scene";
    }

    void startup(editor::EditorContext& context) override {
        context_ = &context;
        auto* hierarchy = new HierarchyPanel(context.scene(), context.selection());
        context.addPanel(QStringLiteral("HierarchyPanel"), QObject::tr("Hierarchy"), hierarchy, Qt::LeftDockWidgetArea);

        auto* inspector = new InspectorPanel(context);
        context.addPanel(QStringLiteral("InspectorPanel"), QObject::tr("Inspector"), inspector, Qt::RightDockWidgetArea);

        auto* add_cube = context.menu(QObject::tr("&Scene"))->addAction(QObject::tr("Add &Cube"));
        context.toolBar()->addAction(add_cube);
        QObject::connect(add_cube, &QAction::triggered, add_cube, [this] { addCube(); });
    }

private:
    void addCube() {
        engine::Scene& scene = context_->scene();
        const int index = cube_count_++;
        const entt::entity cube = scene.CreateEntity("Cube " + std::to_string(index + 1));
        // Rows of five in front of the camera, so new cubes don't overlap.
        scene.GetTransform(cube).position = DirectX::SimpleMath::Vector3{
            static_cast<float>(index % 5 - 2) * 1.5f, 0.5f, -static_cast<float>(index / 5) * 1.5f};
        // Asynchronous: the cube shows up once the mesh is loaded (immediately after the first one).
        scene.AddComponent<engine::MeshRenderer>(cube,
            context_->engine().GetSceneAssets().RequestMesh(engine::BuiltinAssets::CubeMesh),
            context_->engine().GetSceneAssets().RequestMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance));
        context_->selection().select(cube);
    }

    editor::EditorContext* context_ = nullptr;
    int cube_count_ = 0;
};

}  // namespace

SRI_EDITOR_MODULE(SceneModule)
