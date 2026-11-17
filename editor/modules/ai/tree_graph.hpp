#pragma once

#include <QPointF>
#include <QSize>
#include <QStringList>

#include <map>
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

/// A .bt.lua tree as a QtNodes graph, drawn top-down: each composite's out port connects to its
/// children, numbered in the order they run. Read-only for now; nodes can only be moved.
class TreeGraph final : public QtNodes::AbstractGraphModel {
    Q_OBJECT

public:
    using NodeId = QtNodes::NodeId;
    using ConnectionId = QtNodes::ConnectionId;

    struct Node {
        engine::ai::NodeSource source;  // `children` as the file lists them, drawn or not
        std::vector<NodeId> children;   // the drawn ones, in run order
        NodeId parent = QtNodes::InvalidNodeId;
        QString script;                 // the script's catalog path; empty when it isn't found
        QStringList problems;           // what would stop the tree from running
        QPointF position;
        QSize size;
    };

    /// Replaces the graph with `tree`, checked as the runtime would load it. Problems that belong
    /// to no node, such as a missing root, go to `problems`.
    void load(const engine::ai::TreeSource& tree, const engine::AssetCatalog& catalog, QStringList& problems);
    /// Places the trees top-down, sized by `geometry`: the root's first, then any unreachable ones.
    void arrange(QtNodes::AbstractNodeGeometry& geometry);
    [[nodiscard]] const Node* node(NodeId id) const;

    using AbstractGraphModel::nodeData;  // and its typed overloads
    using AbstractGraphModel::portData;
    NodeId newNodeId() override { return next_++; }
    std::unordered_set<NodeId> allNodeIds() const override;
    std::unordered_set<ConnectionId> allConnectionIds(NodeId id) const override;
    std::unordered_set<ConnectionId> connections(NodeId id, QtNodes::PortType type,
                                                 QtNodes::PortIndex index) const override;
    bool connectionExists(ConnectionId connection) const override;
    NodeId addNode(QString type) override;
    bool connectionPossible(ConnectionId connection) const override;
    bool detachPossible(ConnectionId connection) const override;
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

private:
    std::map<NodeId, Node> nodes_;
    NodeId root_ = QtNodes::InvalidNodeId;
    NodeId next_ = 0;
};

}  // namespace behavior
