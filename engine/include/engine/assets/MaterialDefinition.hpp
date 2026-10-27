#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <engine/engine_export.hpp>
#include <engine/rendering/resources/Material.hpp>

namespace engine {

/// Authored values, not GPU handles. An absent field inherits its material's default.
/// A present empty albedoTexture means an explicit JSON null (clear the texture).
/// customBuffer is caller-owned runtime state and deliberately has no on-disk form.
struct MaterialParameterOverrides {
    std::optional<std::string> albedoTexture;
    std::optional<Vector3> specularColor;
    std::optional<float> shininess;
};

struct MaterialDefinition {
    std::string shaderFile; // UTF-8 logical shader name: engine/... or game/...
    MaterialHook hooks = MaterialHook::None;
    MaterialParameterOverrides defaults;
};

struct MaterialInstanceDefinition {
    std::string material; // logical material asset name, never a GPU handle
    MaterialParameterOverrides parameters;
};

/// Version 1 UTF-8 JSON codecs shared by the importer and editor.
/// Return false with a diagnostic; leave the output unchanged on failure.
ENGINE_API bool ParseMaterialDefinition(std::string_view json, MaterialDefinition& result, std::string& error);
ENGINE_API bool ParseMaterialInstanceDefinition(std::string_view json, MaterialInstanceDefinition& result,
                                               std::string& error);
ENGINE_API bool SerializeMaterialDefinition(const MaterialDefinition& definition, std::string& json,
                                            std::string& error);
ENGINE_API bool SerializeMaterialInstanceDefinition(const MaterialInstanceDefinition& definition,
                                                    std::string& json, std::string& error);

} // namespace engine
