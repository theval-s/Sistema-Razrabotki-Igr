#pragma once

#include <QPointer>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <engine/scripting/LuaRuntime.hpp>

#include "tree_graph.hpp"

class QAction;
class QComboBox;
class QFileSystemWatcher;
class QFormLayout;
class QGroupBox;
class QLabel;
class QTableWidget;
class QTimer;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}  // namespace QtNodes

namespace engine {
class AssetCatalog;
}

namespace behavior {

/// Edits one .bt.lua tree as a graph, with the selected node's details beside it. Saving rewrites
/// the file in the editor's own layout. When the file changes on disk, the panel reloads it.
/// While the game runs, it shows what an entity running the tree is doing.
class TreePanel : public QWidget {
    Q_OBJECT

public:
    /// An entity whose Behavior Tree component runs the open tree.
    struct Agent {
        entt::entity entity = entt::null;
        QString name;
        bool selected = false;  // in the editor
    };
    /// The running game as the panel follows it, filled in by the AI module while Play runs.
    struct Game {
        std::function<std::vector<Agent>(const engine::AssetUuid& tree)> agents;
        std::function<std::optional<engine::LuaRuntime::BehaviorAgentState>(entt::entity entity)> state;
    };

    /// Finds scripts in `catalog`, which must outlive the panel.
    explicit TreePanel(const engine::AssetCatalog& catalog, QWidget* parent = nullptr);
    ~TreePanel() override;

    /// Shows the tree in `source`, after offering to save the open one. One that can't be read
    /// leaves the panel saying why.
    void open(const std::filesystem::path& source);
    /// Offers to save unsaved changes. False if the user cancelled.
    bool maybeSave(QWidget* parent);
    /// Writes the tree to its file, after a warning if that loses text the editor didn't write.
    bool save();
    /// Follows `game` a few times a second, until given an empty one.
    void setGame(Game game);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// Reads the file again, dropping unsaved changes.
    void reload();
    /// Reloads a file that changed on disk, after asking if that loses unsaved changes.
    void fileChanged();
    /// Editors that save by replacing the file drop it from the watcher.
    void watch();
    /// The title, problems and actions, after any change.
    void updateState();
    void fit();
    void arrange();
    /// Rebuilds the details once control returns to the event loop, so a field can ask for it.
    void scheduleDetails();
    void showDetails();
    void showEditError(const QString& error);
    void showMenu(const QPoint& position);
    /// What the watched agent is doing now.
    void showGame();
    void openScript(TreeGraph::NodeId id);
    /// Runs `change` as one undoable step; it returns false if it changed nothing.
    bool edit(const QString& text, const std::function<bool()>& change);
    void addNode(engine::ai::NodeKind kind, QPointF position);
    void addChild(TreeGraph::NodeId parent, engine::ai::NodeKind kind);
    [[nodiscard]] TreeGraph::NodeId selectedNode() const;
    void select(TreeGraph::NodeId id);

    const engine::AssetCatalog& catalog_;
    std::filesystem::path source_;
    engine::AssetUuid tree_id_;  // from the file's .meta, for finding its agents
    std::string disk_;      // the file as last read or saved
    std::string baseline_;  // the graph as last read or saved, written out: an edited one differs
    QString load_error_;
    bool dirty_ = false;
    bool asking_ = false;
    bool details_pending_ = false;
    TreeGraph graph_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;  // after graph_, which it draws
    QtNodes::GraphicsView* view_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* problems_ = nullptr;
    QFormLayout* details_ = nullptr;
    TreeGraph::NodeId details_node_ = QtNodes::InvalidNodeId;
    QPointer<QLabel> edit_error_;
    QPointer<QLabel> node_problems_;
    QFileSystemWatcher* watcher_ = nullptr;
    QAction* save_ = nullptr;
    QAction* undo_ = nullptr;
    QAction* redo_ = nullptr;
    QAction* reload_ = nullptr;
    QAction* arrange_ = nullptr;
    QAction* fit_ = nullptr;
    QAction* open_source_ = nullptr;

    Game game_;
    QTimer* game_timer_ = nullptr;
    QGroupBox* game_box_ = nullptr;
    QComboBox* agents_ = nullptr;
    QLabel* agent_status_ = nullptr;
    QTableWidget* blackboard_ = nullptr;
    std::vector<Agent> agent_list_;          // as agents_ lists them
    entt::entity followed_ = entt::null;  // the selected agent when last seen, so a new selection is followed
};

}  // namespace behavior
