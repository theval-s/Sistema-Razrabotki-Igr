#include "tree_graph.hpp"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <string_view>
#include <utility>

#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QtNodes/NodeData>
#include <QtNodes/NodeDelegateModel>
#include <QtNodes/NodeStyle>
#include <QtNodes/StyleCollection>

#include <engine/assets/AssetCatalog.hpp>

namespace behavior {

namespace {

using engine::ai::NodeKind;
using QtNodes::InvalidNodeId;
using QtNodes::NodeRole;
using QtNodes::PortRole;
using QtNodes::PortType;

QString toQString(const std::string& text) {
    return QString::fromStdString(text);
}

QString kindName(const NodeKind kind) {
    return QString::fromLatin1(engine::ai::NodeKindName(kind));
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
        for (const NodeKind each : NodeKinds) {
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

QJsonObject parametersToJson(const engine::ScriptPropertyMap& parameters) {
    QJsonObject json;
    for (const auto& [name, value] : parameters) {
        if (const auto* text = std::get_if<std::string>(&value.value)) {
            json[toQString(name)] = toQString(*text);
        } else if (const auto* boolean = std::get_if<bool>(&value.value)) {
            json[toQString(name)] = *boolean;
        } else {
            json[toQString(name)] = value.Number();
        }
    }
    return json;
}

engine::ScriptPropertyMap parametersFromJson(const QJsonObject& json) {
    engine::ScriptPropertyMap parameters;
    for (auto it = json.begin(); it != json.end(); ++it) {
        const std::string name = it.key().toStdString();
        const QJsonValue value = it.value();
        if (value.isString()) {
            parameters.emplace(name, value.toString().toStdString());
        } else if (value.isBool()) {
            parameters.emplace(name, value.toBool());
        } else if (value.isDouble()) {
            parameters.emplace(name, value.toDouble());
        }
    }
    return parameters;
}

}  // namespace

void TreeGraph::load(const engine::ai::TreeSource& tree, const engine::AssetCatalog& catalog) {
    catalog_ = &catalog;
    nodes_.clear();
    root_ = InvalidNodeId;
    rootName_ = tree.root;
    next_ = 0;

    // IDs follow file order, so tree.nodes[id] is the node's own entry.
    std::map<std::string, NodeId> ids;  // the first node with each ID, as the runtime looks them up
    for (const engine::ai::NodeSource& source : tree.nodes) {
        const NodeId id = newNodeId();
        Node& node = nodes_[id];
        node.source = source;
        node.source.children.clear();
        ids.emplace(source.id, id);
    }
    if (const auto root = ids.find(tree.root); root != ids.end()) {
        root_ = root->second;
    }

    // In file order: a child listed twice runs under the first parent, as the runtime checks.
    for (auto& [id, node] : nodes_) {
        const std::vector<std::string>& names = tree.nodes[id].children;
        if (isLeaf(node.source.kind)) {
            if (!names.empty()) {
                node.fileProblems << tr("%1 nodes run a script, so they can't have children; saving drops them.")
                                         .arg(kindName(node.source.kind));
            }
            continue;
        }
        for (const std::string& name : names) {
            const auto child = ids.find(name);
            if (child == ids.end()) {
                node.fileProblems << tr("No node '%1' to run; saving drops it.").arg(toQString(name));
            } else if (child->second == root_) {
                node.fileProblems << tr("'%1' is the root, so it can't run under another node; saving drops it.")
                                         .arg(toQString(name));
            } else if (child->second == id) {
                node.fileProblems << tr("A node can't run itself; saving drops it.");
            } else if (Node& target = nodes_[child->second]; target.parent != InvalidNodeId) {
                node.fileProblems << tr("'%1' already runs under '%2'; saving drops it here.")
                                         .arg(toQString(name), toQString(nodes_[target.parent].source.id));
            } else {
                target.parent = id;
                node.children.push_back(child->second);
            }
        }
    }
    check();
    Q_EMIT modelReset();
}

engine::ai::TreeSource TreeGraph::tree() const {
    engine::ai::TreeSource tree;
    tree.root = root_ != InvalidNodeId ? nodes_.at(root_).source.id : rootName_;
    for (const auto& [id, node] : nodes_) {
        engine::ai::NodeSource source = node.source;
        for (const NodeId child : node.children) {
            source.children.push_back(nodes_.at(child).source.id);
        }
        tree.nodes.push_back(std::move(source));
    }
    return tree;
}

void TreeGraph::check() {
    std::map<std::string, int> uses;
    for (const auto& [id, node] : nodes_) {
        ++uses[node.source.id];
    }
    std::set<NodeId> reached;
    if (root_ != InvalidNodeId) {
        std::vector<NodeId> pending{root_};
        while (!pending.empty()) {
            const NodeId id = pending.back();
            pending.pop_back();
            if (reached.insert(id).second) {
                pending.insert(pending.end(), nodes_.at(id).children.begin(), nodes_.at(id).children.end());
            }
        }
    }
    const QString rootName = toQString(root_ != InvalidNodeId ? nodes_.at(root_).source.id : rootName_);

    std::vector<NodeId> repaint;
    for (auto& [id, node] : nodes_) {
        QStringList problems = node.fileProblems;
        const QString kind = kindName(node.source.kind);
        if (node.source.id.empty()) {
            problems << tr("Give this node an ID.");
        } else if (uses[node.source.id] > 1) {
            problems << tr("Another node is also called '%1'.").arg(toQString(node.source.id));
        }
        node.script.clear();
        if (isLeaf(node.source.kind)) {
            const engine::CatalogEntry* entry =
                catalog_ != nullptr && !node.source.script.empty() ? findScript(*catalog_, node.source.script) : nullptr;
            if (entry != nullptr) {
                node.script = toQString(entry->path);
            } else if (node.source.script.empty()) {
                problems << tr("Choose a script for this %1.").arg(kind);
            } else {
                problems << tr("No script asset '%1'.").arg(toQString(node.source.script));
            }
        } else if (node.children.empty()) {
            problems << tr("A %1 needs at least one child.").arg(kind);
        }
        if (root_ != InvalidNodeId && !reached.contains(id)) {
            problems << tr("The root, '%1', never reaches this node.").arg(rootName);
        }
        if (problems != node.problems) {
            node.problems = std::move(problems);
            repaint.push_back(id);
        }
    }

    problems_.clear();
    if (!nodes_.empty() && root_ == InvalidNodeId) {
        problems_ << (rootName_.empty() ? tr("The tree has no root.")
                                        : tr("The root, '%1', is not one of the nodes.").arg(rootName))
                  << tr("Right-click a node that runs under no other and choose Set as Root.");
    }
    for (const NodeId id : repaint) {
        Q_EMIT nodeUpdated(id);
    }
    Q_EMIT changed();
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
    // Positions are set directly: going through setNodeData would reorder children halfway.
    std::set<NodeId> placed;
    const std::function<void(NodeId, double, int)> place = [&](const NodeId id, const double left, const int depth) {
        if (!placed.insert(id).second) {
            return;
        }
        Node& node = nodes_.at(id);
        const double span = width(id);
        node.position = QPointF(left + (span - node.size.width()) / 2.0, depth * row);
        Q_EMIT nodePositionUpdated(id);
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
    if (root_ != InvalidNodeId) {
        start(root_);
    }
    for (const auto& [id, node] : nodes_) {
        if (node.parent == InvalidNodeId) {
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

TreeGraph::NodeId TreeGraph::create(const NodeKind kind, const QPointF position) {
    const NodeId id = newNodeId();
    Node& node = nodes_[id];
    node.source.kind = kind;
    node.source.id = unusedId(kindName(kind).toLower().toStdString());
    node.position = position;
    if (root_ == InvalidNodeId && nodes_.size() == 1) {
        root_ = id;
    }
    Q_EMIT nodeCreated(id);
    check();
    return id;
}

bool TreeGraph::rename(const NodeId id, const std::string& name, QString& error) {
    const auto found = nodes_.find(id);
    if (!editable_ || found == nodes_.end() || found->second.source.id == name) {
        return false;
    }
    if (name.empty()) {
        error = tr("A node needs an ID.");
        return false;
    }
    if (std::ranges::any_of(nodes_, [&](const auto& other) { return other.second.source.id == name; })) {
        error = tr("Another node is already called '%1'.").arg(toQString(name));
        return false;
    }
    found->second.source.id = name;
    Q_EMIT nodeUpdated(id);
    check();
    return true;
}

bool TreeGraph::setKind(const NodeId id, const NodeKind kind, QString& error) {
    const auto found = nodes_.find(id);
    if (!editable_ || found == nodes_.end() || found->second.source.kind == kind) {
        return false;
    }
    Node& node = found->second;
    if (isLeaf(kind) && !node.children.empty()) {
        error = tr("Remove its children first: a %1 can't have any.").arg(kindName(kind));
        return false;
    }
    if (!isLeaf(kind)) {
        node.source.script.clear();
    }
    node.source.kind = kind;
    Q_EMIT nodeUpdated(id);  // its ports change too
    check();
    return true;
}

bool TreeGraph::setScript(const NodeId id, const std::string& script) {
    const auto found = nodes_.find(id);
    if (!editable_ || found == nodes_.end() || !isLeaf(found->second.source.kind) ||
        found->second.source.script == script) {
        return false;
    }
    found->second.source.script = script;
    check();
    return true;
}

bool TreeGraph::setParameters(const NodeId id, engine::ScriptPropertyMap parameters) {
    const auto found = nodes_.find(id);
    if (!editable_ || found == nodes_.end() || found->second.source.parameters == parameters) {
        return false;
    }
    found->second.source.parameters = std::move(parameters);
    Q_EMIT changed();
    return true;
}

bool TreeGraph::setRoot(const NodeId id) {
    const auto found = nodes_.find(id);
    if (!editable_ || found == nodes_.end() || id == root_ || found->second.parent != InvalidNodeId) {
        return false;
    }
    const NodeId old = std::exchange(root_, id);
    Q_EMIT nodeUpdated(id);  // it loses its in port, and the old root gains one
    if (old != InvalidNodeId) {
        Q_EMIT nodeUpdated(old);
    }
    check();
    return true;
}

void TreeGraph::clearFileProblems() {
    for (auto& [id, node] : nodes_) {
        node.fileProblems.clear();
    }
    check();
}

TreeGraph::Snapshot TreeGraph::snapshot() const {
    return {nodes_, root_, rootName_, next_};
}

void TreeGraph::restore(Snapshot snapshot) {
    nodes_ = std::move(snapshot.nodes);
    root_ = snapshot.root;
    rootName_ = std::move(snapshot.rootName);
    next_ = snapshot.next;
    check();
    Q_EMIT modelReset();
}

bool TreeGraph::pastable(const QJsonObject& json) const {
    std::map<NodeId, QJsonObject> pasted;
    for (const QJsonValue& value : json["nodes"].toArray()) {
        const QJsonObject node = value.toObject();
        if (!node["id"].isDouble() || !pasted.emplace(static_cast<NodeId>(node["id"].toInteger()), node).second) {
            return false;
        }
    }
    std::set<NodeId> children;
    for (const QJsonValue& value : json["connections"].toArray()) {
        const ConnectionId connection = QtNodes::fromJson(value.toObject());
        const auto parent = pasted.find(connection.outNodeId);
        const auto child = pasted.find(connection.inNodeId);
        if (parent == pasted.end() || child == pasted.end() || connection.outPortIndex != 0 ||
            connection.inPortIndex != 0 || connection.inNodeId == connection.outNodeId ||
            !children.insert(connection.inNodeId).second || child->second["root"].toBool()) {
            return false;
        }
        const QString kind = parent->second["kind"].toString();
        if (kind != kindName(NodeKind::Sequence) && kind != kindName(NodeKind::Selector)) {
            return false;
        }
    }
    return !pasted.empty();
}

void TreeGraph::sortChildren(const NodeId parent) {
    std::vector<NodeId>& children = nodes_.at(parent).children;
    const std::vector<NodeId> before = children;
    std::ranges::stable_sort(children, {}, [this](const NodeId id) { return nodes_.at(id).position.x(); });
    if (children != before) {
        for (const NodeId child : children) {
            Q_EMIT nodeUpdated(child);  // its number changed
        }
        Q_EMIT changed();
    }
}

std::string TreeGraph::unusedId(const std::string& base) const {
    const auto used = [this](const std::string& id) {
        return std::ranges::any_of(nodes_, [&](const auto& node) { return node.second.source.id == id; });
    };
    std::string id = base;
    for (int number = 2; used(id); ++number) {
        id = base + std::to_string(number);
    }
    return id;
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
    if (type == PortType::In && found->parent != InvalidNodeId) {
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
    return InvalidNodeId;  // the panel adds nodes through create()
}

bool TreeGraph::connectionPossible(const ConnectionId connection) const {
    const Node* parent = node(connection.outNodeId);
    const Node* child = node(connection.inNodeId);
    if (!editable_ || parent == nullptr || child == nullptr || connection.outPortIndex != 0 ||
        connection.inPortIndex != 0 || isLeaf(parent->source.kind) || connection.inNodeId == root_ ||
        child->parent != InvalidNodeId) {
        return false;
    }
    // The child mustn't be the parent or run it. A file can hold a cycle, so count the steps.
    NodeId above = connection.outNodeId;
    for (std::size_t steps = 0; above != InvalidNodeId && steps <= nodes_.size(); ++steps) {
        if (above == connection.inNodeId) {
            return false;
        }
        above = nodes_.at(above).parent;
    }
    return above == InvalidNodeId;
}

bool TreeGraph::detachPossible(ConnectionId) const {
    return editable_;
}

void TreeGraph::addConnection(const ConnectionId connection) {
    const Node* parent = node(connection.outNodeId);
    const Node* child = node(connection.inNodeId);
    if (parent == nullptr || child == nullptr || connection.outPortIndex != 0 || connection.inPortIndex != 0 ||
        isLeaf(parent->source.kind) || connection.inNodeId == root_ || child->parent != InvalidNodeId ||
        connection.inNodeId == connection.outNodeId) {
        return;
    }
    nodes_.at(connection.inNodeId).parent = connection.outNodeId;
    nodes_.at(connection.outNodeId).children.push_back(connection.inNodeId);
    Q_EMIT connectionCreated(connection);
    sortChildren(connection.outNodeId);
    Q_EMIT nodeUpdated(connection.inNodeId);  // it shows its number now
    check();
}

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
        if (found->second.parent != InvalidNodeId) {
            sortChildren(found->second.parent);
        }
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

bool TreeGraph::deleteConnection(const ConnectionId connection) {
    if (!connectionExists(connection)) {
        return false;
    }
    std::erase(nodes_.at(connection.outNodeId).children, connection.inNodeId);
    nodes_.at(connection.inNodeId).parent = InvalidNodeId;
    Q_EMIT connectionDeleted(connection);
    Q_EMIT nodeUpdated(connection.inNodeId);  // it loses its number
    for (const NodeId sibling : nodes_.at(connection.outNodeId).children) {
        Q_EMIT nodeUpdated(sibling);
    }
    check();
    return true;
}

bool TreeGraph::deleteNode(const NodeId id) {
    const auto found = nodes_.find(id);
    if (!editable_ || found == nodes_.end()) {
        return false;
    }
    for (const ConnectionId connection : allConnectionIds(id)) {
        deleteConnection(connection);
    }
    if (id == root_) {
        root_ = InvalidNodeId;
        rootName_.clear();
    }
    nodes_.erase(id);
    Q_EMIT nodeDeleted(id);
    check();
    return true;
}

QJsonObject TreeGraph::saveNode(const NodeId id) const {
    const Node* found = node(id);
    if (found == nullptr) {
        return {};
    }
    QJsonObject json;
    json["id"] = static_cast<qint64>(id);
    json["position"] = QJsonObject{{"x", found->position.x()}, {"y", found->position.y()}};
    json["kind"] = kindName(found->source.kind);
    json["name"] = toQString(found->source.id);
    json["script"] = toQString(found->source.script);
    json["parameters"] = parametersToJson(found->source.parameters);
    json["root"] = id == root_;
    return json;
}

void TreeGraph::loadNode(const QJsonObject& json) {
    // Always adds the node: QtNodes goes on to use it.
    const auto id = static_cast<NodeId>(json["id"].toInteger());
    if (nodes_.contains(id)) {
        return;
    }
    next_ = std::max(next_, id + 1);
    Node node;
    node.source.kind = NodeKind::Action;
    for (const NodeKind kind : NodeKinds) {
        if (json["kind"].toString() == kindName(kind)) {
            node.source.kind = kind;
        }
    }
    const QString name = json["name"].toString();
    node.source.id = unusedId(name.isEmpty() ? kindName(node.source.kind).toLower().toStdString() : name.toStdString());
    if (isLeaf(node.source.kind)) {
        node.source.script = json["script"].toString().toStdString();
    }
    node.source.parameters = parametersFromJson(json["parameters"].toObject());
    const QJsonObject position = json["position"].toObject();
    node.position = QPointF(position["x"].toDouble(), position["y"].toDouble());
    nodes_.emplace(id, std::move(node));
    if (json["root"].toBool() && root_ == InvalidNodeId) {
        root_ = id;
    }
    Q_EMIT nodeCreated(id);
    check();
}

}  // namespace behavior
