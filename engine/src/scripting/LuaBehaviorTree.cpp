#include "LuaBehaviorTree.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <engine/core/Log.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/scripting/LuaBindingsContext.hpp>

namespace engine::ai {
namespace {

[[noreturn]] void Fail(const std::string& error) { throw std::runtime_error(error); }

const CatalogEntry& Resolve(const AssetCatalog& catalog, const std::string& name, CatalogAssetKind kind) {
    const auto* entry = catalog.Find(std::string_view(name), kind);
    if (!entry) entry = catalog.FindPath(name);
    if (!entry || entry->metadata.kind != kind) Fail("Missing or wrong kind of behavior asset: " + name);
    return *entry;
}

std::string String(const sol::object& object) {
    if (object.get_type() != sol::type::string) Fail("Expected a behavior string field");
    return object.as<std::string>();
}

sol::table Table(const sol::object& object) {
    if (object.get_type() != sol::type::table) Fail("Expected a behavior table field");
    return object.as<sol::table>();
}

std::vector<sol::object> Array(const sol::object& object) {
    const auto table = Table(object);
    const auto length = table.size();
    std::vector<sol::object> items;
    items.reserve(length);
    for (std::size_t i = 1; i <= length; ++i) {
        auto item = table.raw_get<sol::object>(i);
        if (item.get_type() == sol::type::nil) Fail("Expected a dense behavior array");
        items.push_back(std::move(item));
    }
    std::size_t keys = 0;
    for (const auto& item : table) {
        (void)item;
        ++keys;
    }
    if (keys != length) Fail("Expected a dense behavior array");
    return items;
}

ScriptPropertyMap Parameters(const sol::object& object) {
    ScriptPropertyMap parameters;
    if (object.get_type() == sol::type::nil) return parameters;
    for (const auto& [key, value] : Table(object)) {
        const auto name = String(key);
        switch (value.get_type()) {
        case sol::type::boolean: parameters.emplace(name, value.as<bool>()); break;
        case sol::type::number: parameters.emplace(name, value.as<double>()); break;
        case sol::type::string: parameters.emplace(name, value.as<std::string>()); break;
        default: Fail("Behavior parameters must be scalar");
        }
    }
    std::string error;
    if (!ValidateScriptProperties(parameters, error)) Fail(error);
    return parameters;
}

const char* Name(Status status) {
    switch (status) {
    case Status::Success: return "Success";
    case Status::Failure: return "Failure";
    case Status::Running: return "Running";
    }
    return "Faulted";
}

const char* Name(ExitReason reason) {
    switch (reason) {
    case ExitReason::Success: return "Success";
    case ExitReason::Failure: return "Failure";
    case ExitReason::Cancelled: return "Cancelled";
    case ExitReason::Faulted: return "Faulted";
    }
    return "Faulted";
}

NodeKind Kind(const std::string& name) {
    if (name == "Sequence") return NodeKind::Sequence;
    if (name == "Selector") return NodeKind::Selector;
    if (name == "Action") return NodeKind::Action;
    if (name == "Condition") return NodeKind::Condition;
    Fail("Unknown behavior node kind: " + name);
}

}

class LuaBehaviorLeaves final : public LeafExecutor {
public:
    LuaBehaviorLeaves(sol::state& lua, LuaEntityHandle entity, std::shared_ptr<const TreeDefinition> definition,
                      std::shared_ptr<ScriptPropertyMap> overrides, LuaBehaviorTree::LoadAsset load)
        : lua_(lua), entity_(std::move(entity)), definition_(std::move(definition)),
          overrides_(std::move(overrides)), load_(std::move(load)), blackboard_(lua.create_table()),
          leaves_(definition_->Nodes().size()) {}

    bool Check(std::size_t node) override {
        const auto result = Call(node, "Check", true);
        if (result.get_type() != sol::type::boolean) Fail("Check must return a boolean");
        return result.as<bool>();
    }

    void Enter(std::size_t node) override { (void)Call(node, "OnEnter", false); }
    Status Tick(std::size_t node, double dt) override {
        const auto result = String(Call(node, "Tick", true, dt));
        for (const auto status : {Status::Running, Status::Success, Status::Failure})
            if (result == Name(status)) return status;
        Fail("Tick must return Running, Success, or Failure");
    }
    void Exit(std::size_t node, ExitReason reason) override {
        if (leaves_[node].valid()) (void)Call(node, "OnExit", false, Name(reason));
    }
    const std::string& LastNode() const noexcept { return lastNode_; }

private:
    template<class... Args>
    sol::object Call(std::size_t index, const char* name, bool required, Args&&... args) {
        const auto& node = definition_->Nodes()[index];
        lastNode_ = node.id + " [" + node.script.id.ToString() + "]";
        auto& leaf = leaves_[index];
        if (!leaf.valid()) leaf = load_(node.script, CatalogAssetKind::Script);
        const auto callback = leaf.raw_get<sol::object>(name);
        if (!required && callback.get_type() == sol::type::nil) return sol::make_object(lua_, sol::lua_nil);
        if (callback.get_type() != sol::type::function) Fail(std::string(name) + " must be a function");
        auto parameters = lua_.create_table();
        const auto set = [&](const auto& properties) {
            for (const auto& [key, value] : properties) {
                if (const auto* text = std::get_if<std::string>(&value.value)) parameters[key] = *text;
                else if (const auto* boolean = std::get_if<bool>(&value.value)) parameters[key] = *boolean;
                else parameters[key] = value.Number();
            }
        };
        set(node.parameters);
        set(*overrides_);
        auto context = lua_.create_table_with("entity", entity_, "parameters", parameters, "blackboard", blackboard_);
        LuaBehaviorBudget budget(lua_.lua_state());
        const auto function = callback.as<sol::protected_function>();
        sol::protected_function_result result = function(context, std::forward<Args>(args)...);
        if (!result.valid()) {
            const sol::error error = result;
            Fail(error.what());
        }
        if (result.return_count() == 0) return sol::make_object(lua_, sol::lua_nil);
        return result.get<sol::object>();
    }

    sol::state& lua_;
    LuaEntityHandle entity_;
    std::shared_ptr<const TreeDefinition> definition_;
    std::shared_ptr<ScriptPropertyMap> overrides_;
    LuaBehaviorTree::LoadAsset load_;
    sol::table blackboard_;
    std::vector<sol::table> leaves_;
    std::string lastNode_;
};

static void Exhausted(lua_State* state, lua_Debug*) {
    luaL_error(state, "Behavior callback exceeded its instruction budget");
}

LuaBehaviorBudget::LuaBehaviorBudget(lua_State* state)
    : state_(state), hook_(lua_gethook(state)), mask_(lua_gethookmask(state)), count_(lua_gethookcount(state)) {
    lua_sethook(state_, Exhausted, LUA_MASKCOUNT, 20000);
}

LuaBehaviorBudget::~LuaBehaviorBudget() { lua_sethook(state_, hook_, mask_, count_); }

LuaBehaviorTree::LuaBehaviorTree(sol::state& lua, const AssetCatalog& catalog, LoadAsset load)
    : lua_(lua), catalog_(catalog), load_(std::move(load)) {}

std::shared_ptr<const TreeDefinition> LuaBehaviorTree::LoadDefinition(const AssetReference& reference) {
    if (const auto it = definitions_.find(reference.id); it != definitions_.end()) return it->second;
    const auto definition = load_(reference, CatalogAssetKind::BehaviorTree);
    const auto entries = Array(definition.raw_get<sol::object>("nodes"));
    std::vector<Node> nodes;
    std::unordered_map<std::string, std::size_t> ids;
    nodes.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto table = Table(entry);
        Node node{String(table.raw_get<sol::object>("id")), Kind(String(table.raw_get<sol::object>("kind"))), {}, {}, {}};
        ids.emplace(node.id, nodes.size());
        if (node.kind == NodeKind::Action || node.kind == NodeKind::Condition) {
            const auto& asset = Resolve(catalog_, String(table.raw_get<sol::object>("script")), CatalogAssetKind::Script);
            node.script = {asset.metadata.id, asset.path};
        }
        node.parameters = Parameters(table.raw_get<sol::object>("parameters"));
        nodes.push_back(std::move(node));
    }
    const auto index = [&](const sol::object& id) {
        const auto name = String(id);
        const auto it = ids.find(name);
        if (it == ids.end()) Fail("Unknown behavior node: " + name);
        return it->second;
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto children = Table(entries[i]).raw_get<sol::object>("children");
        if (children.get_type() != sol::type::nil)
            for (const auto& child : Array(children)) nodes[i].children.push_back(index(child));
    }
    std::string error;
    auto compiled = TreeDefinition::Create(std::move(nodes), index(definition.raw_get<sol::object>("root")), error);
    if (!compiled) Fail(error);
    definitions_.emplace(reference.id, compiled);
    return compiled;
}

void LuaBehaviorTree::Register(LuaBindingsContext& bindings) {
    auto api = lua_.create_named_table("BT");
    for (const auto status : {Status::Running, Status::Success, Status::Failure}) api[Name(status)] = Name(status);
    const auto checked = [&bindings](const LuaEntityHandle& entity) {
        auto* scene = bindings.GetScene();
        if (!scene || !entity.IsValid() || !entity.BelongsTo(*scene)) Fail("Invalid behavior entity");
        return LuaEntityHandle::FromScene(*scene, entity.Resolve());
    };
    api.set_function("Attach", [this, &bindings, checked](const LuaEntityHandle& entity, const std::string& asset,
                                                        sol::optional<sol::table> parameters) {
        auto handle = checked(entity);
        const auto& entry = Resolve(catalog_, asset, CatalogAssetKind::BehaviorTree);
        BehaviorTreeComponent value{{entry.metadata.id, entry.path}, true,
                                    parameters ? Parameters(*parameters) : ScriptPropertyMap{}};
        bindings.enqueueSceneCommand([handle, value = std::move(value)] {
            if (!handle.IsValid()) return;
            auto& scene = *handle.GetScene();
            const auto id = handle.Resolve();
            scene.RemoveComponent<BehaviorTreeComponent>(id);
            scene.AddComponent<BehaviorTreeComponent>(id, value);
        });
    });
    api.set_function("SetParameters", [&bindings, checked](const LuaEntityHandle& entity, const sol::table& values) {
        auto handle = checked(entity);
        auto parameters = Parameters(values);
        bindings.enqueueSceneCommand([handle, parameters = std::move(parameters)] {
            if (!handle.IsValid()) return;
            if (auto* component = handle.GetScene()->TryGetComponent<BehaviorTreeComponent>(handle.Resolve()))
                component->parameters = parameters;
        });
    });
    api.set_function("SetEnabled", [&bindings, checked](const LuaEntityHandle& entity, bool enabled) {
        auto handle = checked(entity);
        bindings.enqueueSceneCommand([handle, enabled] {
            if (!handle.IsValid()) return;
            if (auto* component = handle.GetScene()->TryGetComponent<BehaviorTreeComponent>(handle.Resolve()))
                component->enabled = enabled;
        });
    });
    api.set_function("Detach", [&bindings, checked](const LuaEntityHandle& entity) {
        auto handle = checked(entity);
        bindings.enqueueSceneCommand([handle] {
            if (handle.IsValid()) handle.GetScene()->RemoveComponent<BehaviorTreeComponent>(handle.Resolve());
        });
    });
    api.set_function("GetStatus", [this, checked](const LuaEntityHandle& entity) {
        const auto handle = checked(entity);
        const auto* component = handle.GetScene()->TryGetComponent<BehaviorTreeComponent>(handle.Resolve());
        const auto it = agents_.find(handle.Resolve());
        if (!component || !component->enabled || !component->tree.id.IsValid())
            return std::make_tuple(std::string("Stopped"), std::string{});
        if (it == agents_.end()) return std::make_tuple(std::string("Pending"), std::string{});
        const auto* node = it->second.tree ? it->second.tree->ActiveAction() : nullptr;
        return std::make_tuple(it->second.status, node ? node->id : std::string{});
    });
}

void LuaBehaviorTree::Tick(Scene& scene, double dt) {
    time_ += dt;
    std::vector<entt::entity> entities;
    scene.ForEach<BehaviorTreeComponent>([&](entt::entity id, const BehaviorTreeComponent& component) {
        if (component.enabled && component.tree.id.IsValid()) entities.push_back(id);
        else Remove(id);
    });
    if (dt <= 0 || entities.empty()) return;
    std::size_t budget = 256;
    const std::size_t start = nextAgent_ % entities.size();
    std::size_t cursor = start;
    for (std::size_t visited = 0; visited < entities.size() && budget != 0; ++visited) {
        const auto id = entities[cursor];
        cursor = (cursor + 1) % entities.size();
        const auto* component = scene.TryGetComponent<BehaviorTreeComponent>(id);
        if (!component) continue;
        if (const auto it = agents_.find(id); it != agents_.end() && it->second.asset != component->tree.id)
            Remove(id);
        auto [it, inserted] = agents_.try_emplace(id);
        auto& agent = it->second;
        try {
            if (inserted) {
                --budget;
                agent.asset = component->tree.id;
                agent.lastTickTime = time_ - dt;
                auto definition = LoadDefinition(component->tree);
                agent.overrides = std::make_shared<ScriptPropertyMap>(component->parameters);
                auto executor = std::make_unique<LuaBehaviorLeaves>(lua_, LuaEntityHandle::FromScene(scene, id),
                                                                   definition, agent.overrides, load_);
                agent.leaves = executor.get();
                agent.tree = std::make_unique<TreeInstance>(std::move(definition), std::move(executor));
            }
            if (!agent.tree || budget == 0) continue;
            *agent.overrides = component->parameters;
            const auto result = agent.tree->Tick(time_ - agent.lastTickTime, std::min<std::size_t>(budget, 32));
            agent.lastTickTime = time_;
            agent.status = Name(result.status);
            budget -= result.steps;
        } catch (const std::exception& error) {
            if (budget) --budget;
            const auto* leaves = agent.tree ? agent.leaves : nullptr;
            log::Lua().error("Behavior tree {} on {} at {}: {}", agent.asset.ToString(),
                             scene.GetId(id).ToString(), leaves ? leaves->LastNode() : "definition", error.what());
            agent.tree.reset();
            agent.leaves = nullptr;
            agent.status = "Faulted";
        }
    }
    nextAgent_ = cursor == start ? cursor + 1 : cursor;
}

void LuaBehaviorTree::Remove(entt::entity id) noexcept {
    const auto it = agents_.find(id);
    if (it == agents_.end()) return;
    if (it->second.tree) {
        try { it->second.tree->Stop(); }
        catch (const std::exception& error) { log::Lua().error("Behavior cancellation: {}", error.what()); }
        catch (...) {}
    }
    agents_.erase(it);
}

void LuaBehaviorTree::Clear() noexcept {
    while (!agents_.empty()) Remove(agents_.begin()->first);
    definitions_.clear();
    nextAgent_ = 0;
    time_ = 0;
}

}
