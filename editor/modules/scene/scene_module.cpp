#include <QAction>
#include <QDropEvent>
#include <QEvent>
#include <QMenu>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
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
#include <editor/scene_events.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>

#include "component_inspectors.hpp"
#include "inspector_panel.hpp"

namespace {

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

/// "A/B/C" -> "A/B"; empty for a top-level folder.
QString parentFolder(const QString& path) {
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

/// "A/B/C" -> "C".
QString folderName(const QString& path) {
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

/// `name` inside `parent` (empty: top level).
QString joinFolder(const QString& parent, const QString& name) {
    return parent.isEmpty() ? name : parent + QLatin1Char('/') + name;
}

/// Whether `path` is `folder` or inside it.
bool inFolder(const QString& path, const QString& folder) {
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

/// Every folder the hierarchy shows: the scene's list, the folders of root entities, and all
/// their parent folders. Sorted, so parents come before their children.
QStringList collectFolders(const engine::Scene& scene) {
    QStringList folders;
    const auto add = [&folders](QString path) {
        for (; !path.isEmpty(); path = parentFolder(path)) folders.append(path);
    };
    for (const std::string& folder : scene.GetEditorFolders()) add(toQString(folder));
    for (const entt::entity root : scene.GetRootEntities()) add(toQString(scene.GetEditorFolder(root)));
    folders.removeDuplicates();
    folders.sort(Qt::CaseInsensitive);
    return folders;
}

/// Lists the scene's folders and entities as a tree, rebuilt when its structure changes. What the
/// user collapsed stays collapsed across rebuilds; everything else is expanded.
/// Its entity selection mirrors the editor's Selection both ways; the current item is the primary.
/// Folders can be selected too, but only in the tree: they aren't entities.
/// Dropping the selection reports the item that should receive it: the item dropped on, the parent
/// of the items dropped between, or nullptr for the top level. The tree never moves items itself.
class HierarchyTree : public QTreeWidget {
public:
    using DropHandler = std::function<void(const QTreeWidgetItem* receiver)>;
    using RenameHandler = std::function<void(const QString& folder, const QString& name)>;

    HierarchyTree(engine::Scene& scene, editor::Selection& selection, editor::SceneEvents& events, DropHandler drop,
                  RenameHandler rename)
        : scene_(scene), selection_(selection), drop_(std::move(drop)), rename_(std::move(rename)) {
        setHeaderHidden(true);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setDragDropMode(QAbstractItemView::InternalMove);
        // Only folders are editable, by double-click or F2.
        setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        // A click changes the selection before the current item, so both push.
        connect(this, &QTreeWidget::itemSelectionChanged, this, &HierarchyTree::pushSelection);
        connect(this, &QTreeWidget::currentItemChanged, this, &HierarchyTree::pushSelection);
        connect(&selection_, &editor::Selection::changed, this, &HierarchyTree::showSelection);
        // Rebuilds block signals, so this is only the user renaming a folder.
        connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
            if (const QString folder = folderOf(item); !folder.isEmpty()) {
                rename_(folder, item->text(0).trimmed());
                QTimer::singleShot(0, this, &HierarchyTree::rebuild);  // also undoes a refused name
            }
        });
        connect(&events, &editor::SceneEvents::changed, this, &HierarchyTree::follow);
        // Rebuilds block signals too, so these are only the user's.
        connect(this, &QTreeWidget::itemCollapsed, this, [this](const QTreeWidgetItem* item) {
            collapsed_.insert(keyOf(item));
        });
        connect(this, &QTreeWidget::itemExpanded, this, [this](const QTreeWidgetItem* item) {
            collapsed_.remove(keyOf(item));
        });
        rebuild();
    }

    /// entt::null for folder items and nullptr.
    static entt::entity entityOf(const QTreeWidgetItem* item) {
        if (item == nullptr || item->data(0, FolderRole).isValid()) return entt::null;
        return static_cast<entt::entity>(item->data(0, Qt::UserRole).toUInt());
    }

    /// The full path of a folder item; empty for entity items and nullptr.
    static QString folderOf(const QTreeWidgetItem* item) {
        return item != nullptr ? item->data(0, FolderRole).toString() : QString();
    }

    /// The selected folders that aren't inside another selected one.
    [[nodiscard]] QStringList selectedFolders() const {
        QStringList folders;
        for (const QTreeWidgetItem* item : selectedItems()) {
            if (const QString folder = folderOf(item); !folder.isEmpty()) folders.append(folder);
        }
        QStringList outermost;
        for (const QString& folder : folders) {
            if (std::ranges::none_of(folders, [&](const QString& other) {
                    return other != folder && inFolder(folder, other);
                })) {
                outermost.append(folder);
            }
        }
        return outermost;
    }

    /// Rebuilds now and starts editing the name of `folder`, e.g. right after creating it.
    void editFolder(const QString& folder) {
        rebuild();
        if (const auto it = folders_.find(folder); it != folders_.end()) {
            setCurrentItem(it->second);
            editItem(it->second);
        }
    }

protected:
    void dropEvent(QDropEvent* event) override {
        if (event->source() != this) {
            event->ignore();
            return;
        }
        const QTreeWidgetItem* receiver = nullptr;
        if (const QTreeWidgetItem* target = itemAt(event->position().toPoint())) {
            switch (dropIndicatorPosition()) {
            case OnItem: receiver = target; break;
            case AboveItem:
            case BelowItem: receiver = target->parent(); break;
            case OnViewport: break;
            }
        }
        // Not a move: after a MoveAction Qt would remove the dragged rows itself. The scene
        // changes instead, and the tree follows it.
        event->setDropAction(Qt::CopyAction);
        event->accept();
        drop_(receiver);
    }

private:
    /// Set on folder items only: their full path.
    static constexpr int FolderRole = Qt::UserRole + 1;

    /// Rebuilds when the structure changed; otherwise only the names can have.
    void follow(const editor::SceneChanges& changes) {
        if (changes.structure) {
            rebuild();
            return;
        }
        const QSignalBlocker blocker(this);  // not a folder rename
        for (const entt::entity entity : changes.entities) {
            if (QTreeWidgetItem* item = findItem(entity)) {
                item->setText(0, toQString(scene_.GetName(entity)));
            }
        }
    }

    /// What identifies an item across rebuilds, for its collapsed state.
    QString keyOf(const QTreeWidgetItem* item) const {
        if (const QString folder = folderOf(item); !folder.isEmpty()) {
            return QStringLiteral("folder:") + folder;
        }
        const entt::entity entity = entityOf(item);
        return scene_.IsValid(entity) ? QString::fromStdString(scene_.GetId(entity).ToString()) : QString();
    }

    void rebuild() {
        // Rebuilding isn't a selection change: the selection is restored, quietly.
        const QSignalBlocker blocker(this);
        clear();
        items_.clear();
        folders_.clear();
        // Folders first, so at every level they're listed above the entities.
        for (const QString& folder : collectFolders(scene_)) {
            folderItem(folder);
        }
        for (const entt::entity root : scene_.GetRootEntities()) {
            const std::string& folder = scene_.GetEditorFolder(root);
            if (folder.empty()) {
                addTopLevelItem(makeItem(root));
            } else {
                folderItem(toQString(folder))->addChild(makeItem(root));
            }
        }
        for (QTreeWidgetItemIterator it(this); *it != nullptr; ++it) {
            (*it)->setExpanded(!collapsed_.contains(keyOf(*it)));
        }
        showSelection();
    }

    /// Tree -> Selection.
    void pushSelection() {
        std::vector<entt::entity> entities;
        QTreeWidgetItem* current = currentItem();
        for (const QTreeWidgetItem* item : selectedItems()) {
            if (item != current && entityOf(item) != entt::null) {
                entities.push_back(entityOf(item));
            }
        }
        if (current != nullptr && current->isSelected() && entityOf(current) != entt::null) {
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
        // E.g. an entity selected right after it was created, before the scene's changes arrive.
        if (std::ranges::any_of(selection_.entities(), [this](const entt::entity entity) {
                return scene_.IsValid(entity) && findItem(entity) == nullptr;
            })) {
            rebuild();
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
        auto* item = new QTreeWidgetItem({toQString(scene_.GetName(entity))});
        item->setData(0, Qt::UserRole, entt::to_integral(entity));
        items_.emplace(entity, item);
        for (const entt::entity child : scene_.GetChildren(entity)) {
            item->addChild(makeItem(child));
        }
        return item;
    }

    /// The item of `folder`, created with its parent folders if needed.
    QTreeWidgetItem* folderItem(const QString& folder) {
        if (const auto it = folders_.find(folder); it != folders_.end()) {
            return it->second;
        }
        auto* item = new QTreeWidgetItem({folderName(folder)});
        item->setData(0, FolderRole, folder);
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        if (const QString parent = parentFolder(folder); parent.isEmpty()) {
            addTopLevelItem(item);
        } else {
            folderItem(parent)->addChild(item);
        }
        folders_.emplace(folder, item);
        return item;
    }

    QTreeWidgetItem* findItem(const entt::entity entity) const {
        const auto it = items_.find(entity);
        return it != items_.end() ? it->second : nullptr;
    }

    engine::Scene& scene_;
    editor::Selection& selection_;
    DropHandler drop_;
    RenameHandler rename_;
    /// Every item of the tree, by entity or folder path. Owned by the tree.
    std::unordered_map<entt::entity, QTreeWidgetItem*> items_;
    std::unordered_map<QString, QTreeWidgetItem*> folders_;
    /// keyOf() the items the user collapsed.
    QSet<QString> collapsed_;
    /// Set while the tree's own selection is being pushed, so it isn't shown back.
    bool pushing_ = false;
};

/// The hierarchy tree, with an Add button floating in its bottom-right corner. Right-clicking the
/// tree opens the same menu, plus Focus, Duplicate, Unparent and Delete when it was on an entity, or
/// Rename and Delete Folder on a folder. New entities go in front of the viewport camera, at the
/// root, and become the selection. Reparenting keeps entities where they are in the world.
/// Folders are editor-only groups of root entities (Scene::SetEditorFolder); dropping an entity
/// into one makes it a root again.
class HierarchyPanel : public QWidget {
public:
    explicit HierarchyPanel(editor::EditorContext& context) : context_(context) {
        tree_ = new HierarchyTree(
            context.scene(), context.selection(), context.sceneEvents(),
            [this](const QTreeWidgetItem* receiver) { record(QObject::tr("Move"), [this, receiver] { drop(receiver); }); },
            [this](const QString& folder, const QString& name) {
                record(QObject::tr("Rename Folder"), [&] { renameFolder(folder, name); });
            });
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(tree_);

        auto* menu = new QMenu(this);
        menu->addAction(QObject::tr("Empty Entity"), this, [this] { record(QObject::tr("Add Entity"), [this] { addEmpty(); }); });
        menu->addAction(QObject::tr("Cube"), this, [this] { record(QObject::tr("Add Cube"), [this] { addCube(); }); });
        menu->addSeparator();
        menu->addAction(QObject::tr("Folder"), this, [this] { record(QObject::tr("Add Folder"), [this] { addFolder(); }); });

        const editor::EditActions& edit = context.editActions();
        edit.attach(this);

        // Pressing the right button on an item has already selected it, as with the left one.
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree_, &QWidget::customContextMenuRequested, this, [this, menu, &edit](const QPoint& position) {
            QMenu context_menu;
            QTreeWidgetItem* item = tree_->itemAt(position);
            if (const QString folder = HierarchyTree::folderOf(item); !folder.isEmpty()) {
                context_menu.addAction(QObject::tr("Rename"), this, [this, item] { tree_->editItem(item); });
                context_menu.addAction(QObject::tr("Delete Folder"), this, [this] {
                    record(QObject::tr("Delete Folder"), [this] { deleteSelectedFolders(); });
                });
                context_menu.addSeparator();
            } else if (item != nullptr) {
                context_menu.addAction(edit.focusAction());
                context_menu.addSeparator();
                context_menu.addAction(edit.cutAction());
                context_menu.addAction(edit.copyAction());
                context_menu.addAction(edit.pasteAction());
                context_menu.addAction(edit.duplicateAction());
                if (std::ranges::any_of(movableSelection(), [this](const entt::entity entity) {
                        return context_.scene().GetParent(entity) != entt::null;
                    })) {
                    context_menu.addAction(QObject::tr("Unparent"), this, [this] {
                        record(QObject::tr("Unparent"), [this] { reparentSelected(entt::null); });
                    });
                }
                context_menu.addAction(edit.deleteAction());
                context_menu.addSeparator();
            } else {
                context_menu.addAction(edit.pasteAction());
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
    void record(const QString& text, const std::function<void()>& change) {
        context_.history().edit(text, change);
    }

    entt::entity createAtSpawnPoint(const std::string_view name) {
        engine::Scene& scene = context_.scene();
        const entt::entity entity = scene.CreateEntity(name);
        if (const editor::EditorCameraSubsystem* camera = context_.camera()) {
            engine::Transform transform = scene.GetTransform(entity);
            transform.position = camera->GetSpawnPoint();
            scene.SetTransform(entity, transform);
        }
        return entity;
    }

    void addEmpty() {
        context_.selection().select(createAtSpawnPoint("Entity"));
    }

    void addCube() {
        engine::Scene& scene = context_.scene();
        const entt::entity cube = createAtSpawnPoint("Cube");
        // The built-in cube is one unit wide around its origin: lift it onto the ground.
        engine::Transform transform = scene.GetTransform(cube);
        transform.position += DirectX::SimpleMath::Vector3{0.0f, 0.5f, 0.0f};
        scene.SetTransform(cube, transform);
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

    /// The selection dropped on `receiver`: an entity (become its children), a folder (go into it)
    /// or nullptr (the top level). Selected folders only go into folders or the top level.
    void drop(const QTreeWidgetItem* receiver) {
        const entt::entity parent = HierarchyTree::entityOf(receiver);
        if (parent != entt::null) {
            reparentSelected(parent);
            return;
        }
        const QString folder = HierarchyTree::folderOf(receiver);
        for (const QString& moved : tree_->selectedFolders()) {
            if (!folder.isEmpty() && inFolder(folder, moved)) continue;  // into itself
            moveFolder(moved, joinFolder(folder, folderName(moved)));
        }
        engine::Scene& scene = context_.scene();
        for (const entt::entity entity : movableSelection()) {
            scene.SetParentKeepingWorld(entity, entt::null);
            scene.SetEditorFolder(entity, folder.toStdString());
        }
    }

    /// Replaces the scene's folder list; the hierarchy shows any folder still used by an entity anyway.
    void setFolders(const QStringList& folders) {
        std::vector<std::string> paths;
        for (const QString& folder : folders) paths.push_back(folder.toStdString());
        context_.scene().SetEditorFolders(std::move(paths));
    }

    /// Creates "New Folder" inside the folder the tree's current item is (the top level if none),
    /// and starts editing its name.
    void addFolder() {
        const QString parent = HierarchyTree::folderOf(tree_->currentItem());
        QStringList folders = collectFolders(context_.scene());
        QString folder = joinFolder(parent, QObject::tr("New Folder"));
        for (int number = 1; folders.contains(folder); ++number) {
            folder = joinFolder(parent, QObject::tr("New Folder (%1)").arg(number));
        }
        folders.append(folder);
        setFolders(folders);
        tree_->editFolder(folder);
    }

    void renameFolder(const QString& folder, const QString& name) {
        if (name == folderName(folder)) return;
        if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
            context_.showStatus(QObject::tr("A folder name can't be empty or contain '/'"), 5000);
            return;
        }
        const QString renamed = joinFolder(parentFolder(folder), name);
        if (collectFolders(context_.scene()).contains(renamed)) {
            context_.showStatus(QObject::tr("There is already a folder named %1 here").arg(name), 5000);
            return;
        }
        moveFolder(folder, renamed);
    }

    /// Renames `from` to `to`, with the folders and entities inside it.
    void moveFolder(const QString& from, const QString& to) {
        if (from == to) return;
        const auto moved = [&](const QString& path) { return inFolder(path, from) ? to + path.mid(from.size()) : path; };
        QStringList folders;
        for (const QString& folder : collectFolders(context_.scene())) folders.append(moved(folder));
        folders.removeDuplicates();  // moving into a folder that has one of the same name merges them
        setFolders(folders);
        relocateEntities(moved);
    }

    /// Removes the selected folders. What they held moves up into their parent folder.
    void deleteSelectedFolders() {
        for (const QString& deleted : tree_->selectedFolders()) {
            const QString parent = parentFolder(deleted);
            const auto moved = [&](const QString& path) {
                if (!inFolder(path, deleted)) return path;
                return path == deleted ? parent : joinFolder(parent, path.mid(deleted.size() + 1));
            };
            QStringList folders;
            for (const QString& folder : collectFolders(context_.scene())) {
                if (folder != deleted) folders.append(moved(folder));
            }
            folders.removeDuplicates();
            setFolders(folders);
            relocateEntities(moved);
        }
    }

    /// Gives each root entity in a folder the folder `moved` maps it to.
    void relocateEntities(const std::function<QString(const QString&)>& moved) {
        engine::Scene& scene = context_.scene();
        // A copy: SetEditorFolder doesn't change the roots, but stay safe.
        const std::vector<entt::entity> roots(scene.GetRootEntities().begin(), scene.GetRootEntities().end());
        for (const entt::entity root : roots) {
            const QString folder = toQString(scene.GetEditorFolder(root));
            if (!folder.isEmpty()) scene.SetEditorFolder(root, moved(folder).toStdString());
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
