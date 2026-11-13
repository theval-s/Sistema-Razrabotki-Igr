#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/scripting/ScriptProperties.hpp>

namespace engine::ai {

enum class NodeKind { Sequence, Selector, Condition, Action };
enum class Status { Running, Success, Failure };
enum class ExitReason { Success, Failure, Cancelled, Faulted };

struct Node {
    std::string id;
    NodeKind kind;
    std::vector<std::size_t> children;
    AssetReference script;
    ScriptPropertyMap parameters;
};

class ENGINE_API TreeDefinition {
public:
    static std::shared_ptr<const TreeDefinition> Create(
        std::vector<Node> nodes, std::size_t root, std::string& error);

    const std::vector<Node>& Nodes() const noexcept { return nodes_; }
    std::size_t Root() const noexcept { return root_; }

private:
    TreeDefinition(std::vector<Node> nodes, std::size_t root);
    std::vector<Node> nodes_;
    std::size_t root_;
};

class LeafExecutor {
public:
    virtual ~LeafExecutor() = default;
    virtual bool Check(std::size_t node) = 0;
    virtual void Enter(std::size_t node) = 0;
    virtual Status Tick(std::size_t node, double dt) = 0;
    virtual void Exit(std::size_t node, ExitReason reason) = 0;
};

struct TickResult {
    Status status;
    std::size_t steps;
};

class ENGINE_API TreeInstance {
public:
    TreeInstance(std::shared_ptr<const TreeDefinition> definition,
                 std::unique_ptr<LeafExecutor> leaves);
    ~TreeInstance();
    TreeInstance(const TreeInstance&) = delete;
    TreeInstance& operator=(const TreeInstance&) = delete;

    // A new activation starts on the tick after a terminal result.
    TickResult Tick(double dt, std::size_t stepBudget);
    void Stop();
    [[nodiscard]] const Node* ActiveAction() const noexcept;

private:
    struct Frame {
        std::size_t node;
        std::size_t child = 0;
    };

    void Reset(ExitReason reason);
    std::shared_ptr<const TreeDefinition> definition_;
    std::unique_ptr<LeafExecutor> leaves_;
    std::vector<Frame> stack_;
    bool actionEntered_ = false;
};

}
