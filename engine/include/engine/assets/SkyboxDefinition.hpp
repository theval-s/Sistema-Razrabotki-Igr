#pragma once

#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>
#include <string>
#include <string_view>

namespace engine {
/// .skybox.json: exactly {"version":1,"cubemap":{"id":"<uuid>","path":"<hint>"}}.
/// version and cubemap are required; reference path is optional and diagnostic only.
/// The UUID must resolve to a Texture asset whose source is DDS and whose actual
/// resource is one six-face cubemap (not a cube array). HDR and LDR use the DDS loader.
struct SkyboxDefinition {
    int version{1};
    AssetReference cubemap;
    bool operator==(const SkyboxDefinition&) const = default;
};

/// Pure structural validation; resource shape is checked once during dependency resolution.
/// Codecs reject unknown fields and unsupported versions; outputs are unchanged on failure.
[[nodiscard]] ENGINE_API bool ValidateSkyboxDefinition(const SkyboxDefinition& value, std::string& error);
[[nodiscard]] ENGINE_API bool ParseSkyboxDefinition(std::string_view json,
    SkyboxDefinition& value, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeSkyboxDefinition(const SkyboxDefinition& value,
    std::string& json, std::string& error);
} // namespace engine
