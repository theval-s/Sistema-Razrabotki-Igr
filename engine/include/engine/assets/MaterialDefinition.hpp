#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <engine/engine_export.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/rendering/resources/Material.hpp>

namespace engine {

/// Authored values, not GPU handles. Version 2 stores UUID references alongside
/// diagnostic paths; the string fields are retained for version 1 compatibility.
/// An absent field inherits its material's default.
/// A present empty albedoTexture means an explicit JSON null (clear the texture).
/// customBuffer is caller-owned runtime state and deliberately has no on-disk form.
struct MaterialParameterOverrides {
    std::optional<std::string> albedoTexture;
    std::optional<AssetReference> albedoReference; // version 2 authored UUID reference
    std::optional<Vector3> specularColor;
    std::optional<float> shininess;
};

struct MaterialDefinition {
    int version = 1;
    std::string shaderFile; // UTF-8 logical shader name: engine/... or game/...
    std::optional<AssetReference> shaderReference;
    MaterialHook hooks = MaterialHook::None;
    MaterialParameterOverrides defaults;
};

struct MaterialInstanceDefinition {
    int version = 1;
    std::string material; // logical material asset name, never a GPU handle
    std::optional<AssetReference> materialReference;
    MaterialParameterOverrides parameters;
};

/// Version 1 (legacy paths) and version 2 (UUID references) UTF-8 JSON codecs.
/// Return false with a diagnostic; leave the output unchanged on failure.
ENGINE_API bool ParseMaterialDefinition(std::string_view json, MaterialDefinition& result, std::string& error);
ENGINE_API bool ParseMaterialInstanceDefinition(std::string_view json, MaterialInstanceDefinition& result,
                                               std::string& error);
ENGINE_API bool SerializeMaterialDefinition(const MaterialDefinition& definition, std::string& json,
                                            std::string& error);
ENGINE_API bool SerializeMaterialInstanceDefinition(const MaterialInstanceDefinition& definition,
                                                    std::string& json, std::string& error);

} // namespace engine
