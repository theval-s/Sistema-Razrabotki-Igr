#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/rendering/PostProcessing.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

namespace engine {

struct RenderPipelineDefinition {
    std::uint32_t version = 1;
    AssetReference shader;
    std::string vertexEntryPoint = "VSMain";
    std::string pixelEntryPoint = "PSMain";
    std::vector<ShaderDefine> defines;
    ProceduralBlend blend = ProceduralBlend::Opaque;
    ProceduralDepth depth = ProceduralDepth::ReadWrite;
};

[[nodiscard]] ENGINE_API bool ValidateRenderPipelineDefinition(const RenderPipelineDefinition& value,
                                                              std::string& error);
// Codecs, loaders and variant resolution leave their output unchanged on failure.
[[nodiscard]] ENGINE_API bool ParseRenderPipelineDefinition(std::string_view json, RenderPipelineDefinition& value,
                                                           std::string& error);
[[nodiscard]] ENGINE_API bool SerializeRenderPipelineDefinition(const RenderPipelineDefinition& value,
                                                               std::string& json, std::string& error);
[[nodiscard]] ENGINE_API bool LoadRenderPipelineDefinition(const std::filesystem::path& file,
                                                          RenderPipelineDefinition& value, std::string& error);
// UUID is authoritative; path is a diagnostic hint only. Vertex first, pixel second.
[[nodiscard]] ENGINE_API bool RenderPipelineShaderVariants(const RenderPipelineDefinition& definition,
                                                          const AssetCatalog& catalog,
                                                          std::array<ShaderVariant, 2>& value, std::string& error);

} // namespace engine
