#include "tree_graph.hpp"

#include <QColor>

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <string_view>

#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/NodeData>
#include <QtNodes/NodeDelegateModel>
#include <QtNodes/NodeStyle>
#include <QtNodes/StyleCollection>

#include <engine/assets/AssetCatalog.hpp>

namespace behavior {

namespace {

using engine::ai::NodeKind;
using QtNodes::NodeRole;
using QtNodes::PortRole;
using QtNodes::PortType;

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

QString kindName(const NodeKind kind) {
    return QString::fromLatin1(engine::ai::NodeKindName(kind));
}

bool isLeaf(const NodeKind kind) {
    return kind == NodeKind::Condition || kind == NodeKind::Action;
}

/// As the runtime resolves a leaf's script: a catalog UUID or a logical path.
const engine::CatalogEntry* findScript(const engine::AssetCatalog& catalog, const std::string& name) {
    const engine::CatalogEntry* entry = catalog.Find(std::string_view(name), engine::CatalogAssetKind::Script);
    if (entry == nullptr) {
        entry = catalog.FindPath(name);
    }
    return entry != nullptr && entry->metadata.kind == engine::CatalogAssetKind::Script ? entry : nullptr;
}

/// The default node style tinted by kind, so the four kinds tell apart at a glance.
const QVariant& styleOf(const NodeKind kind) {
    static const std::array<QVariant, 4> styles = [] {
        std::array<QVariant, 4> result;
        for (const NodeKind each : {NodeKind::Sequence, NodeKind::Selector, NodeKind::Condition, NodeKind::Action}) {
            QColor color;
            switch (each) {
            case NodeKind::Sequence: color = QColor(56, 118, 78); break;
            case NodeKind::Selector: color = QColor(62, 94, 148); break;
            case NodeKind::Condition: color = QColor(146, 106, 40); break;
            case NodeKind::Action: color = QColor(84, 84, 96); break;
            }
            QtNodes::NodeStyle style = QtNodes::StyleCollection::nodeStyle();
            style.GradientColor0 = color.lighter(125);
            style.GradientColor1 = color;
            style.GradientColor2 = color.darker(110);
            style.GradientColor3 = color.darker(120);
            result[static_cast<std::size_t>(each)] = style.toJson().toVariantMap();
        }
        return result;
    }();
    return styles[static_cast<std::size_t>(kind)];
}

}  // namespace

void TreeGraph::load(const engine::ai::TreeSource& tree, const engine::AssetCatalog& catalog, QStringList& problems) {
    nodes_.clear();
    root_ = QtNodes::InvalidNodeId;
    next_ = 0;

    std::map<std::string, NodeId> ids;  // the first node with each ID, as the runtime looks them up
    for (const engine::ai::NodeSource& source : tree.nodes) {
        const NodeId id = newNodeId();
        Node& node = nodes_[id];
        node.source = source;
        if (source.id.empty()) {
            node.problems << tr("Give this node an ID.");
        } else if (!ids.emplace(source.id, id).second) {
            node.problems << tr("Another node is also called '%1'.").arg(toQString(source.id));
        }
        if (!isLeaf(source.kind)) {
            continue;
        }
        if (source.script.empty()) {
            node.problems << tr("Choose a script for this %1.").arg(kindName(source.kind));
        } else if (const engine::CatalogEntry* entry = findScript(catalog, source.script)) {
            node.script = toQString(entry->path);
        } else {
            node.problems << tr("No script asset '%1'.").arg(toQString(source.script));
        }
    }
    if (const auto root = ids.find(tree.root); root != ids.end()) {
        root_ = root->second;
    } else {
        problems << tr("The root, '%1', is not one of the nodes.").arg(toQString(tree.root));
    }

    // Nodes in file order: a child listed twice runs under the first parent, like the runtime checks.
    for (auto& [id, node] : nodes_) {
        const QString kind = kindName(node.source.kind);
        if (isLeaf(node.source.kind)) {
            if (!node.source.children.empty()) {
                node.problems << tr("%1 nodes run a script, so they can't have children.").arg(kind);
            }
            continue;
        }
        if (node.source.children.empty()) {
            node.problems << tr("A %1 needs at least one child.").arg(kind);
        }
        for (const std::string& name : node.source.children) {
            const auto child = ids.find(name);
            if (child == ids.end()) {
                node.problems << tr("No node '%1' to run.").arg(toQString(name));
            } else if (child->second == root_) {
                node.problems << tr("'%1' is the root, so it can't run under another node.").arg(toQString(name));
            } else if (child->second == id) {
                node.problems << tr("A node can't run itself.");
            } else if (Node& target = nodes_[child->second]; target.parent != QtNodes::InvalidNodeId) {
                node.problems << tr("'%1' already runs under '%2'.")
                                     .arg(toQString(name), toQString(nodes_[target.parent].source.id));
            } else {
                target.parent = id;
                node.children.push_back(child->second);
            }
        }
    }

    if (root_ != QtNodes::InvalidNodeId) {
        std::set<NodeId> reached;
        std::vector<NodeId> pending{root_};
        while (!pending.empty()) {
            const NodeId id = pending.back();
            pending.pop_back();
            if (reached.insert(id).second) {
                pending.insert(pending.end(), nodes_[id].children.begin(), nodes_[id].children.end());
            }
        }
        for (auto& [id, node] : nodes_) {
            if (!reached.contains(id)) {
                node.problems << tr("The root, '%1', never reaches this node.").arg(toQString(tree.root));
            }
        }
    }
    Q_EMIT modelReset();
}

void TreeGraph::arrange(QtNodes::AbstractNodeGeometry& geometry) {
    constexpr double Gap = 30.0;
    constexpr double RowGap = 60.0;
    double row = 0.0;
    for (const auto& [id, node] : nodes_) {
        geometry.recomputeSize(id);
        row = std::max(row, static_cast<double>(node.size.height()));
    }
    row += RowGap;

    std::map<NodeId, double> widths;
    const std::function<double(NodeId)> width = [&](const NodeId id) {
        if (const auto known = widths.find(id); known != widths.end()) {
            return known->second;
        }
        widths[id] = 0.0;  // a node in a cycle counts once
        const Node& node = nodes_.at(id);
        double children = -Gap;
        for (const NodeId child : node.children) {
            children += width(child) + Gap;
        }
        return widths[id] = std::max(static_cast<double>(node.size.width()), children);
    };
    std::set<NodeId> placed;
    const std::function<void(NodeId, double, int)> place = [&](const NodeId id, const double left, const int depth) {
        if (!placed.insert(id).second) {
            return;
        }
        const Node& node = nodes_.at(id);
        const double span = width(id);
        setNodeData(id, NodeRole::Position, QPointF(left + (span - node.size.width()) / 2.0, depth * row));
        double children = -Gap;
        for (const NodeId child : node.children) {
            children += width(child) + Gap;
        }
        double x = left + (span - children) / 2.0;
        for (const NodeId child : node.children) {
            place(child, x, depth + 1);
            x += width(child) + Gap;
        }
    };

    double left = 0.0;
    const auto start = [&](const NodeId id) {
        if (!placed.contains(id)) {
            place(id, left, 0);
            left += width(id) + 3.0 * Gap;
        }
    };
    if (root_ != QtNodes::InvalidNodeId) {
        start(root_);
    }
    for (const auto& [id, node] : nodes_) {
        if (node.parent == QtNodes::InvalidNodeId) {
            start(id);
        }
    }
    // What's left runs in a cycle of its own.
    for (const auto& [id, node] : nodes_) {
        start(id);
    }
}

const TreeGraph::Node* TreeGraph::node(const NodeId id) const {
    const auto found = nodes_.find(id);
    return found != nodes_.end() ? &found->second : nullptr;
}

std::unordered_set<TreeGraph::NodeId> TreeGraph::allNodeIds() const {
    std::unordered_set<NodeId> ids;
    for (const auto& [id, node] : nodes_) {
        ids.insert(id);
    }
    return ids;
}

std::unordered_set<TreeGraph::ConnectionId> TreeGraph::allConnectionIds(const NodeId id) const {
    auto result = connections(id, PortType::In, 0);
    result.merge(connections(id, PortType::Out, 0));
    return result;
}

std::unordered_set<TreeGraph::ConnectionId> TreeGraph::connections(const NodeId id, const PortType type,
                                                                   const QtNodes::PortIndex index) const {
    std::unordered_set<ConnectionId> result;
    const Node* found = node(id);
    if (found == nullptr || index != 0) {
        return result;
    }
    if (type == PortType::In && found->parent != QtNodes::InvalidNodeId) {
        result.insert({found->parent, 0, id, 0});
    } else if (type == PortType::Out) {
        for (const NodeId child : found->children) {
            result.insert({id, 0, child, 0});
        }
    }
    return result;
}

bool TreeGraph::connectionExists(const ConnectionId connection) const {
    const Node* child = node(connection.inNodeId);
    return child != nullptr && child->parent == connection.outNodeId && connection.outPortIndex == 0 &&
           connection.inPortIndex == 0;
}

TreeGraph::NodeId TreeGraph::addNode(QString) {
    return QtNodes::InvalidNodeId;
}

bool TreeGraph::connectionPossible(ConnectionId) const {
    return false;
}

bool TreeGraph::detachPossible(ConnectionId) const {
    return false;
}

void TreeGraph::addConnection(ConnectionId) {}

bool TreeGraph::nodeExists(const NodeId id) const {
    return nodes_.contains(id);
}

QVariant TreeGraph::nodeData(const NodeId id, const NodeRole role) const {
    const Node* found = node(id);
    if (found == nullptr) {
        return {};
    }
    const NodeKind kind = found->source.kind;
    switch (role) {
    case NodeRole::Type:
        return kindName(kind);
    case NodeRole::Position:
        return found->position;
    case NodeRole::Size:
        return found->size;
    case NodeRole::CaptionVisible:
        return true;
    case NodeRole::Caption:
        return QStringLiteral("%1 %2 %3").arg(kindName(kind), QChar(0x00B7), toQString(found->source.id));
    case NodeRole::Style:
        return styleOf(kind);
    case NodeRole::InPortCount:
        return id == root_ ? 0u : 1u;
    case NodeRole::OutPortCount:
        return isLeaf(kind) ? 0u : 1u;
    case NodeRole::ValidationState: {
        if (found->problems.isEmpty()) {
            return {};
        }
        QtNodes::NodeValidationState state;
        state._state = QtNodes::NodeValidationState::State::Error;
        state._stateMessage = found->problems.join(QLatin1Char('\n'));
        return QVariant::fromValue(state);
    }
    default:
        return {};
    }
}

bool TreeGraph::setNodeData(const NodeId id, const NodeRole role, QVariant value) {
    const auto found = nodes_.find(id);
    if (found == nodes_.end()) {
        return false;
    }
    switch (role) {
    case NodeRole::Position:
        found->second.position = value.toPointF();
        Q_EMIT nodePositionUpdated(id);
        return true;
    case NodeRole::Size:
        found->second.size = value.toSize();
        return true;
    default:
        return false;
    }
}

QVariant TreeGraph::portData(const NodeId id, const PortType type, const QtNodes::PortIndex, const PortRole role) const {
    const Node* found = node(id);
    if (found == nullptr) {
        return {};
    }
    switch (role) {
    case PortRole::DataType:
        return QVariant::fromValue(QtNodes::NodeDataType{QStringLiteral("behavior"), QString()});
    case PortRole::ConnectionPolicyRole:
        return QVariant::fromValue(type == PortType::In ? QtNodes::ConnectionPolicy::One
                                                        : QtNodes::ConnectionPolicy::Many);
    case PortRole::CaptionVisible:
        return true;
    case PortRole::Caption: {
        // The child's place in its parent's run order.
        const Node* parent = node(found->parent);
        if (type != PortType::In || parent == nullptr) {
            return QString();
        }
        const auto place = std::ranges::find(parent->children, id) - parent->children.begin();
        return QString::number(place + 1);
    }
    default:
        return {};
    }
}

bool TreeGraph::setPortData(NodeId, PortType, QtNodes::PortIndex, const QVariant&, PortRole) {
    return false;
}

bool TreeGraph::deleteConnection(ConnectionId) {
    return false;
}

bool TreeGraph::deleteNode(NodeId) {
    return false;
}

}  // namespace behavior
