#pragma once
#include <engine/assets/AssetCatalog.hpp>
#include <engine/scripting/ScriptProperties.hpp>

namespace engine::ai {
struct BehaviorTreeComponent {
    AssetReference tree;
    bool enabled = true;
    ScriptPropertyMap parameters;
    bool operator==(const BehaviorTreeComponent&) const = default;
};
}
