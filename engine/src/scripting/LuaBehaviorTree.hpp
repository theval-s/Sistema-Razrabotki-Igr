#pragma once

#include <engine/ai/BehaviorTree.hpp>
#include <engine/ai/BehaviorTreeComponent.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <sol/sol.hpp>
#include <functional>
#include <unordered_map>

namespace engine { struct LuaBindingsContext; }

namespace engine::ai {

class LuaBehaviorLeaves;

class LuaBehaviorTree {
public:
    using LoadAsset = std::function<sol::table(const AssetReference&, CatalogAssetKind)>;

    LuaBehaviorTree(sol::state& lua, const AssetCatalog& catalog, LoadAsset load);
    void Register(LuaBindingsContext& bindings);
    void Tick(Scene& scene, double dt);
    void Remove(entt::entity entity) noexcept;
    void Clear() noexcept;

private:
    struct Agent {
        AssetUuid asset;
        std::unique_ptr<TreeInstance> tree;
        std::shared_ptr<ScriptPropertyMap> overrides;
        LuaBehaviorLeaves* leaves = nullptr;
        double lastTickTime = 0;
        std::string status = "Pending";
    };

    std::shared_ptr<const TreeDefinition> LoadDefinition(const AssetReference& reference);
    sol::state& lua_;
    const AssetCatalog& catalog_;
    LoadAsset load_;
    std::unordered_map<entt::entity, Agent> agents_;
    std::unordered_map<AssetUuid, std::shared_ptr<const TreeDefinition>, AssetUuidHash> definitions_;
    std::size_t nextAgent_ = 0;
    double time_ = 0;
};

// Applies a Lua VM instruction limit to an outer behavior callback.
class LuaBehaviorBudget {
public:
    explicit LuaBehaviorBudget(lua_State* state);
    ~LuaBehaviorBudget();
private:
    lua_State* state_;
    lua_Hook hook_;
    int mask_;
    int count_;
};

}
