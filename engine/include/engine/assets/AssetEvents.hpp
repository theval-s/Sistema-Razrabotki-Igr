#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include <engine/core/ResourceID.hpp>
#include <engine/assets/AssetCommon.hpp>

// Events published by AssetSystem on Engine::GetEvents() once a load attempt has settled.

namespace engine {

class AssetSystem;

// Correlation key: (source, kind, slot, generation), not id/name alone.
// source is a non-owning identity, never dereferenced by observers. Events are
// delivered synchronously on the engine thread after the state transition.
// Subscriptions and cached identities must not outlive the source AssetSystem;
// clear scene-scoped caches on scene replacement. Pointer addresses are not durable
// IDs and must not be serialized or compared across destroyed/recreated engines.
// slot/generation are the original request handle, captured BEFORE Fail retires it.
// Defaults preserve old aggregate initializers but mean "uncorrelated": consumers
// requiring exact matching must ignore null source or the invalid slot sentinel.
// Ready-asset Reload success uses the existing live handle; this identifies a
// request generation, not a distinct reload invocation. Reload is synchronous.
struct AssetLoadedEvent {
    ResourceID id = 0;
    std::string name;
    AssetKind kind = AssetKind::Mesh;
    const AssetSystem* source = nullptr;
    std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
};

struct AssetFailedEvent {
    ResourceID id = 0;
    std::string name;
    AssetKind kind = AssetKind::Mesh;
    std::string error;
    const AssetSystem* source = nullptr;
    std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
};

}  // namespace engine
