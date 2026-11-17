#pragma once

#include <QWidget>

#include <filesystem>
#include <memory>

#include "tree_graph.hpp"

class QAction;
class QFileSystemWatcher;
class QFormLayout;
class QLabel;

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}  // namespace QtNodes

namespace editor {
class EditorContext;
}

namespace behavior {

/// Shows one .bt.lua tree as a graph, with the selected node's details beside it, and reloads
/// it whenever the file changes. Viewing only for now.
class TreePanel : public QWidget {
    Q_OBJECT

public:
    explicit TreePanel(editor::EditorContext& context, QWidget* parent = nullptr);
    ~TreePanel() override;

    /// Shows the tree in `source`. One that can't be read leaves the panel saying why.
    void open(const std::filesystem::path& source);

private:
    void reload();
    void fit();
    void showDetails();
    void openScript(TreeGraph::NodeId id);

    editor::EditorContext& context_;
    std::filesystem::path source_;
    TreeGraph graph_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;  // after graph_, which it draws
    QtNodes::GraphicsView* view_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* problems_ = nullptr;
    QFormLayout* details_ = nullptr;
    QFileSystemWatcher* watcher_ = nullptr;
    QAction* reload_ = nullptr;
    QAction* arrange_ = nullptr;
    QAction* fit_ = nullptr;
    QAction* open_source_ = nullptr;
};

}  // namespace behavior
