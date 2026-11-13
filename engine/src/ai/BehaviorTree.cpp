#include <engine/ai/BehaviorTree.hpp>

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace engine::ai {

TreeDefinition::TreeDefinition(std::vector<Node> nodes, std::size_t root)
    : nodes_(std::move(nodes)), root_(root) {}

std::shared_ptr<const TreeDefinition> TreeDefinition::Create(
    std::vector<Node> nodes, std::size_t root, std::string& error) {
    if (root >= nodes.size()) {
        error = "Invalid behavior tree root";
        return {};
    }
    std::unordered_set<std::string> ids;
    std::vector<bool> hasParent(nodes.size());
    for (const auto& node : nodes) {
        if (node.id.empty() || !ids.insert(node.id).second) {
            error = "Empty or duplicate behavior node ID: " + node.id;
            return {};
        }
        switch (node.kind) {
        case NodeKind::Sequence:
        case NodeKind::Selector:
            if (node.children.empty()) {
                error = "Composite requires children: " + node.id;
                return {};
            }
            break;
        case NodeKind::Condition:
        case NodeKind::Action:
            if (!node.children.empty()) {
                error = "Leaf cannot have children: " + node.id;
                return {};
            }
            break;
        default:
            error = "Unknown behavior node kind: " + node.id;
            return {};
        }
        if (!ValidateScriptProperties(node.parameters, error)) {
            error = node.id + ": " + error;
            return {};
        }
        for (const auto child : node.children) {
            if (child >= nodes.size() || child == root || hasParent[child]) {
                error = "Invalid or repeated child of behavior node: " + node.id;
                return {};
            }
            hasParent[child] = true;
        }
    }
    // Each non-root node has at most one parent, so a reachable cycle is impossible.
    std::vector<std::size_t> pending{root};
    std::size_t reachable = 0;
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        ++reachable;
        const auto& children = nodes[index].children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    if (reachable != nodes.size()) {
        error = "Behavior tree has unreachable nodes or a disconnected cycle";
        return {};
    }
    error.clear();
    return std::shared_ptr<const TreeDefinition>(new TreeDefinition(std::move(nodes), root));
}

TreeInstance::TreeInstance(std::shared_ptr<const TreeDefinition> definition,
                           std::unique_ptr<LeafExecutor> leaves)
    : definition_(std::move(definition)), leaves_(std::move(leaves)) {
    if (!definition_ || !leaves_) throw std::invalid_argument("Behavior tree requires a definition and leaf executor");
    stack_.reserve(definition_->Nodes().size());
}

TreeInstance::~TreeInstance() {
    try { Stop(); } catch (...) {}
}

TickResult TreeInstance::Tick(double dt, std::size_t stepBudget) {
    if (stepBudget == 0) return {Status::Running, 0};
    if (stack_.empty()) stack_.push_back({definition_->Root()});
    const auto& nodes = definition_->Nodes();
    std::size_t steps = 0;
    try {
        while (steps < stepBudget) {
            ++steps;
            const auto index = stack_.back().node;
            const auto& node = nodes[index];
            Status result;
            switch (node.kind) {
            case NodeKind::Sequence:
            case NodeKind::Selector:
                stack_.push_back({node.children[stack_.back().child]});
                continue;
            case NodeKind::Condition:
                result = leaves_->Check(index) ? Status::Success : Status::Failure;
                break;
            case NodeKind::Action:
                if (!actionEntered_) {
                    actionEntered_ = true;
                    leaves_->Enter(index);
                }
                result = leaves_->Tick(index, dt);
                if (result == Status::Running) return {result, steps};
                if (result != Status::Success && result != Status::Failure)
                    throw std::logic_error("Invalid action status");
                actionEntered_ = false;
                leaves_->Exit(index, result == Status::Success ? ExitReason::Success : ExitReason::Failure);
                break;
            }
            stack_.pop_back();
            while (!stack_.empty()) {
                auto& parent = stack_.back();
                const auto& composite = nodes[parent.node];
                const auto advanceOn = composite.kind == NodeKind::Sequence ? Status::Success : Status::Failure;
                if (result == advanceOn && ++parent.child < composite.children.size()) break;
                stack_.pop_back();
            }
            if (stack_.empty()) return {result, steps};
        }
        return {Status::Running, steps};
    } catch (...) {
        try { Reset(ExitReason::Faulted); } catch (...) {}
        throw;
    }
}

const Node* TreeInstance::ActiveAction() const noexcept {
    return actionEntered_ ? &definition_->Nodes()[stack_.back().node] : nullptr;
}

void TreeInstance::Reset(ExitReason reason) {
    const auto active = actionEntered_;
    const auto index = active ? stack_.back().node : 0;
    actionEntered_ = false;
    stack_.clear();
    if (active) leaves_->Exit(index, reason);
}

void TreeInstance::Stop() {
    Reset(ExitReason::Cancelled);
}

}
