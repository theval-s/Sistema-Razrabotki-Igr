#pragma once

#include <QPointF>
#include <QSize>
#include <QStringList>

#include <array>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <QtNodes/AbstractGraphModel>

#include <engine/ai/BehaviorTreeSource.hpp>

namespace engine {
class AssetCatalog;
}

namespace QtNodes {
class AbstractNodeGeometry;
}

namespace behavior {

/// Every kind of node, in menu order.
inline constexpr std::array NodeKinds{engine::ai::NodeKind::Sequence, engine::ai::NodeKind::Selector,
                                      engine::ai::NodeKind::Condition, engine::ai::NodeKind::Action};

/// Conditions and actions run a script and have no children.
[[nodiscard]] constexpr bool isLeaf(const engine::ai::NodeKind kind) {
    return kind == engine::ai::NodeKind::Condition || kind == engine::ai::NodeKind::Action;
}

/// A .bt.lua tree as a QtNodes graph, drawn top-down: each composite's out port connects to its
/// children, which run left to right. Every change re-checks the tree as the runtime would load it.
class TreeGraph final : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    using NodeId = QtNodes::NodeId;
    using ConnectionId = QtNodes::ConnectionId;

    struct Node {
        engine::ai::NodeSource source;  // its `children` stay empty: see below
        std::vector<NodeId> children;   // in run order, which is left to right
        NodeId parent = QtNodes::InvalidNodeId;
        QString script;                 // the script's catalog path; empty when it isn't found
        QStringList fileProblems;       // file contents the graph can't show; the next save drops them
        QStringList problems;           // all of them, as shown
        QPointF position;
        QSize size;
    };

    /// Everything undo needs to bring the graph back.
    struct Snapshot {
        std::map<NodeId, Node> nodes;
        NodeId root = QtNodes::InvalidNodeId;
        std::string rootName;
        NodeId next = 0;
    };

    /// Replaces the graph with `tree`, whose scripts are looked up in `catalog`.
    void load(const engine::ai::TreeSource& tree, const engine::AssetCatalog& catalog);
    /// The tree as the graph shows it, for saving.
    [[nodiscard]] engine::ai::TreeSource tree() const;
    /// Problems that belong to no node, such as a missing root.
    [[nodiscard]] const QStringList& problems() const { return problems_; }
    /// A graph that isn't editable refuses every change but moving nodes, such as when its file can't be read.
    void setEditable(bool editable) { editable_ = editable; }
    [[nodiscard]] bool editable() const { return editable_; }
    /// Places the trees top-down, sized by `geometry`: the root's first, then any unreachable ones.
    void arrange(QtNodes::AbstractNodeGeometry& geometry);
    [[nodiscard]] const Node* node(NodeId id) const;
    /// The node called `name`, or InvalidNodeId.
    [[nodiscard]] NodeId find(const std::string& name) const;
    [[nodiscard]] NodeId root() const { return root_; }
    /// While the game runs, the node an agent runs: drawn with a selected node's border, or red where it faulted.
    void setActive(NodeId id, bool faulted);
    [[nodiscard]] NodeId active() const { return active_; }

    // Edits for the panel, which records them for undo.
    /// A new node with an unused ID, unconnected; the first in an empty graph becomes the root.
    NodeId create(engine::ai::NodeKind kind, QPointF position);
    bool rename(NodeId id, const std::string& name, QString& error);
    /// Leaves lose their script when they become composites; composites must have no children to become leaves.
    bool setKind(NodeId id, engine::ai::NodeKind kind, QString& error);
    bool setScript(NodeId id, const std::string& script);
    bool setParameters(NodeId id, engine::ScriptPropertyMap parameters);
    /// Only a node that runs under no other can be the root.
    bool setRoot(NodeId id);
    /// After a save, the file holds what the graph shows.
    void clearFileProblems();
    [[nodiscard]] Snapshot snapshot() const;
    void restore(Snapshot snapshot);
    /// Whether QtNodes can paste `json`: it drops in each node, then each link, and fails on a link we refuse.
    [[nodiscard]] bool pastable(const QJsonObject& json) const;

    using AbstractGraphModel::nodeData;  // and its typed overloads
    using AbstractGraphModel::portData;
    NodeId newNodeId() override { return next_++; }
    std::unordered_set<NodeId> allNodeIds() const override;
    std::unordered_set<ConnectionId> allConnectionIds(NodeId id) const override;
    std::unordered_set<ConnectionId> connections(NodeId id, QtNodes::PortType type,
                                                 QtNodes::PortIndex index) const override;
    bool connectionExists(ConnectionId connection) const override;
    NodeId addNode(QString type) override;
    /// From a composite to a node that has no parent and doesn't run the composite itself.
    bool connectionPossible(ConnectionId connection) const override;
    bool detachPossible(ConnectionId connection) const override;
    /// Also restores links that make a cycle, which undo can bring back from a file that had one.
    void addConnection(ConnectionId connection) override;
    bool nodeExists(NodeId id) const override;
    QVariant nodeData(NodeId id, QtNodes::NodeRole role) const override;
    bool setNodeData(NodeId id, QtNodes::NodeRole role, QVariant value) override;
    QVariant portData(NodeId id, QtNodes::PortType type, QtNodes::PortIndex index,
                      QtNodes::PortRole role) const override;
    bool setPortData(NodeId id, QtNodes::PortType type, QtNodes::PortIndex index, const QVariant& value,
                     QtNodes::PortRole role) override;
    bool deleteConnection(ConnectionId connection) override;
    bool deleteNode(NodeId id) override;
    /// For QtNodes' undo, copy and paste.
    QJsonObject saveNode(NodeId id) const override;
    void loadNode(const QJsonObject& json) override;

Q_SIGNALS:
    /// The tree, or what's wrong with it, may have changed.
    void changed();

private:
    /// Works out every problem again and repaints the nodes whose problems changed.
    void check();
    /// Orders `parent`'s children by position, left to right.
    void sortChildren(NodeId parent);
    [[nodiscard]] std::string unusedId(const std::string& base) const;

    const engine::AssetCatalog* catalog_ = nullptr;
    std::map<NodeId, Node> nodes_;  // by ID, which is file order: new nodes go last
    NodeId root_ = QtNodes::InvalidNodeId;
    std::string rootName_;          // what to save while there's no root node
    NodeId next_ = 0;
    QStringList problems_;
    bool editable_ = false;
    NodeId active_ = QtNodes::InvalidNodeId;
    bool faulted_ = false;
};

}  // namespace behavior
