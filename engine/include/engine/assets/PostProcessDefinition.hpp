#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>

namespace engine {

struct PostProcessDefinition {
    std::uint32_t version = 1;
    AssetReference shader;
    ShaderCompileOptions options{.entryPoint = "PSMain"};
};

[[nodiscard]] ENGINE_API bool ValidatePostProcessDefinition(const PostProcessDefinition& value, std::string& error);
// Codecs and loaders leave their output unchanged on failure.
[[nodiscard]] ENGINE_API bool ParsePostProcessDefinition(std::string_view json, PostProcessDefinition& value,
                                                        std::string& error);
[[nodiscard]] ENGINE_API bool SerializePostProcessDefinition(const PostProcessDefinition& value, std::string& json,
                                                            std::string& error);
[[nodiscard]] ENGINE_API bool LoadPostProcessDefinition(const std::filesystem::path& file,
                                                       PostProcessDefinition& value, std::string& error);
// UUID is authoritative; path is a diagnostic hint only. Always produces a pixel variant.
[[nodiscard]] ENGINE_API bool PostProcessShaderVariant(const PostProcessDefinition& definition,
                                                      const AssetCatalog& catalog, ShaderVariant& value,
                                                      std::string& error);

} // namespace engine
