#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <engine/rendering/resources/ShaderContext.hpp>

namespace engine {

/// Which asset table a name belongs to. Also the index of the importer that handles it.
enum class AssetKind : uint8_t {
    Mesh,
    Texture,
    Shader,
    Material,
    MaterialInstance,
    Count
};

enum class AssetState : uint8_t {
    Unloaded,  /// known by name, nothing has been read yet
    Loading,   /// a worker is reading the source files
    Ready,     /// the value can be used
    Failed,    /// the last attempt failed, the reason is stored with the record
};

/// Everything an importer needs to read one asset.
struct AssetDesc {
    std::string name;             /// normalized name, as stored in the table
    std::filesystem::path path;   /// resolved source file or directory
    bool centerOnLoad = false;    /// meshes only: recenter and normalize the geometry
    ShaderType shaderStage = ShaderType::Vertex;
    ShaderCompileOptions shaderOptions;
    bool shaderFromMemory = false;
    std::string shaderSource;     /// generated shader source, owned by the record while the worker compiles
};

}  // namespace engine
