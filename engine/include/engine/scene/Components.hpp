#pragma once

#include <string>
#include <vector>

#include <entt/entity/entity.hpp>

#include <engine/scene/Transform.hpp>

// Components every entity created through Scene::CreateEntity() has.
// Maintained by Scene -- modify Hierarchy only via Scene::SetParent().

namespace engine {

struct Name {
    std::string value;
};

struct Tags {
    std::vector<std::string> values;
};

struct EntityId {
    std::string value; // persistent UUID, independent of EnTT's transient entity value
};

struct Hierarchy {
    entt::entity parent{entt::null};
    std::vector<entt::entity> children;
};

}  // namespace engine
