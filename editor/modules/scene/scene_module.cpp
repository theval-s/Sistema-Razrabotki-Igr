#include <QAction>
#include <QDropEvent>
#include <QEvent>
#include <QMenu>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SimpleMath.h>

#include <engine/scene/Scene.hpp>

#include <editor/edit_actions.hpp>
#include <editor/editor_camera.hpp>
#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>
#include <editor/menu_button.hpp>
#include <editor/play_session.hpp>
#include <editor/selection.hpp>

#include "component_inspectors.hpp"
#include "inspector_panel.hpp"

namespace {

/// Lists the scene's entities as a tree, rebuilt whenever the scene's revision changes.
/// Its selection mirrors the editor's Selection both ways; the current item is the primary.
/// Dragging the selection onto an item asks for it to become that item's children; between
/// items, their siblings; onto empty space, roots. The tree never moves items itself.
class HierarchyTree : public QTreeWidget {
public:
    /// Called with the new parent (entt::null for the root) when the selection is dropped.
    using ReparentHandler = std::function<void(entt::entity parent)>;

    HierarchyTree(engine::Scene& scene, editor::Selection& selection, ReparentHandler reparent)
        : scene_(scene), selection_(selection), reparent_(std::move(reparent)) {
        setHeaderHidden(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setDragDropMode(QAbstractItemView::InternalMove);
        // A click changes the selection before the current item, so both push.
        connect(this, &QTreeWidget::itemSelectionChanged, this, &HierarchyTree::pushSelection);
        connect(this, &QTreeWidget::currentItemChanged, this, &HierarchyTree::pushSelection);
        connect(&selection_, &editor::Selection::changed, this, &HierarchyTree::showSelection);
        // Scene has no change signal, only a revision counter: poll it.
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &HierarchyTree::refresh);
        timer->start(200);
        rebuild();
    }

protected:
    void dropEvent(QDropEvent* event) override {
        if (event->source() != this) {
            event->ignore();
            return;
        }
        entt::entity parent = entt::null;
        if (const QTreeWidgetItem* target = itemAt(event->position().toPoint())) {
            switch (dropIndicatorPosition()) {
            case OnItem: parent = entityOf(target); break;
            case AboveItem:
            case BelowItem: parent = entityOf(target->parent()); break;
            case OnViewport: break;
            }
        }
        // Not a move: after a MoveAction Qt would remove the dragged rows itself. The scene
        // changes instead, and the tree follows it.
        event->setDropAction(Qt::CopyAction);
        event->accept();
        reparent_(parent);
        QTimer::singleShot(0, this, &HierarchyTree::refresh);
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
    ReparentHandler reparent_;
    /// Every item of the tree, by entity. Owned by the tree.
    std::unordered_map<entt::entity, QTreeWidgetItem*> items_;
    std::uint64_t shown_revision_ = ~std::uint64_t{0};
    /// Set while the tree's own selection is being pushed, so it isn't shown back.
    bool pushing_ = false;
};

/// The hierarchy tree, with an Add button floating in its bottom-right corner. Right-clicking the
/// tree opens the same menu, plus Focus, Duplicate, Unparent and Delete when it was on an entity. New entities go in
/// front of the viewport camera, at the root, and become the selection. Reparenting keeps entities where they are in
/// the world.
class HierarchyPanel : public QWidget {
public:
    explicit HierarchyPanel(editor::EditorContext& context) : context_(context) {
        tree_ = new HierarchyTree(context.scene(), context.selection(),
                                  [this](const entt::entity parent) { reparentSelected(parent); });
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(tree_);

        auto* menu = new QMenu(this);
        menu->addAction(QObject::tr("Empty Entity"), this, [this] { addEmpty(); });
        menu->addAction(QObject::tr("Cube"), this, [this] { addCube(); });

        const editor::EditActions& edit = context.editActions();
        edit.attach(this);

        // Pressing the right button on an entity has already selected it, as with the left one.
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree_, &QWidget::customContextMenuRequested, this, [this, menu, &edit](const QPoint& position) {
            QMenu context_menu;
            if (tree_->itemAt(position) != nullptr) {
                context_menu.addAction(edit.focusAction());
                context_menu.addAction(edit.duplicateAction());
                if (std::ranges::any_of(movableSelection(), [this](const entt::entity entity) {
                        return context_.scene().GetParent(entity) != entt::null;
                    })) {
                    context_menu.addAction(QObject::tr("Unparent"), this, [this] { reparentSelected(entt::null); });
                }
                context_menu.addAction(edit.deleteAction());
                context_menu.addSeparator();
            }
            context_menu.addActions(menu->actions());
            context_menu.exec(tree_->viewport()->mapToGlobal(position));
        });

        // On the tree but not its viewport, which would scroll it along with the items.
        add_ = editor::makeMenuButton(QObject::tr("Add"), menu);
        add_->setParent(tree_);
        add_->adjustSize();
        add_->raise();
        // The viewport shrinks when a scroll bar shows up, without the panel resizing.
        tree_->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == tree_->viewport() && event->type() == QEvent::Resize) {
            placeAddButton();
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    entt::entity createAtSpawnPoint(const std::string_view name) {
        engine::Scene& scene = context_.scene();
        const entt::entity entity = scene.CreateEntity(name);
        if (const editor::EditorCameraSubsystem* camera = context_.camera()) {
            scene.GetTransform(entity).position = camera->GetSpawnPoint();
        }
        return entity;
    }

    void addEmpty() {
        context_.selection().select(createAtSpawnPoint("Entity"));
    }

    void addCube() {
        const entt::entity cube = createAtSpawnPoint("Cube");
        // The built-in cube is one unit wide around its origin: lift it onto the ground.
        context_.scene().GetTransform(cube).position += DirectX::SimpleMath::Vector3{0.0f, 0.5f, 0.0f};
        // Asynchronous: the cube shows up once the mesh is loaded (immediately after the first one).
        addMeshRenderer(context_, cube);
        context_.selection().select(cube);
    }

    /// The selected entities that aren't under another selected one: those move, the rest come along.
    std::vector<entt::entity> movableSelection() {
        const engine::Scene& scene = context_.scene();
        const editor::Selection& selection = context_.selection();
        std::vector<entt::entity> movable;
        for (const entt::entity entity : selection.entities()) {
            if (!scene.IsValid(entity)) continue;
            bool under_selected = false;
            for (entt::entity ancestor = scene.GetParent(entity); ancestor != entt::null && !under_selected;
                 ancestor = scene.GetParent(ancestor)) {
                under_selected = selection.contains(ancestor);
            }
            if (!under_selected) movable.push_back(entity);
        }
        return movable;
    }

    /// Moves the selection under `parent` (entt::null: to the root), keeping it in place in the world.
    void reparentSelected(const entt::entity parent) {
        engine::Scene& scene = context_.scene();
        for (const entt::entity entity : movableSelection()) {
            // Refused when `parent` is the entity itself or one of its children: nothing to do.
            scene.SetParentKeepingWorld(entity, parent);
        }
    }

    void placeAddButton() {
        constexpr int Margin = 6;
        const QRect area = tree_->viewport()->geometry();
        add_->move(area.x() + area.width() - add_->width() - Margin, area.y() + area.height() - add_->height() - Margin);
    }

    editor::EditorContext& context_;
    HierarchyTree* tree_ = nullptr;
    QToolButton* add_ = nullptr;
};

class SceneModule final : public editor::IEditorModule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "Scene";
    }

    void startup(editor::EditorContext& context) override {
        auto* hierarchy = new HierarchyPanel(context);
        context.addPanel(QStringLiteral("HierarchyPanel"), QObject::tr("Hierarchy"), hierarchy, Qt::LeftDockWidgetArea);

        auto* inspector = new InspectorPanel(context);
        context.addPanel(QStringLiteral("InspectorPanel"), QObject::tr("Inspector"), inspector, Qt::RightDockWidgetArea);

        // Edits during play would land in the running copy, which Stop throws away.
        const editor::PlaySession* session = &context.playSession();
        QObject::connect(session, &editor::PlaySession::playingChanged, hierarchy, &QWidget::setDisabled);
        QObject::connect(session, &editor::PlaySession::playingChanged, inspector, &QWidget::setDisabled);
    }
};

}  // namespace

SRI_EDITOR_MODULE(SceneModule)
