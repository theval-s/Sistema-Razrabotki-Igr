#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <engine/assets/AssetCommon.hpp>
#include <engine/engine_export.hpp>

namespace engine {

/// Maps a logical asset name to a file on disk.
///
/// The root is the directory of the executable, never the current working directory:
/// double-clicking an exe or launching it from another tool would otherwise change which
/// assets are found. Names under game/ resolve against the game root instead, which a
/// host may point at a project's content directory.
///
/// Out of line, so Engine.dll and its host share one game root.
class ENGINE_API AssetPathResolver {
public:
    /// Mesh assets address a folder that holds model.<ext> (and optionally texture.png);
    /// a name ending in a model extension addresses that file directly. Assimp chooses the
    /// reader from the extension, so the file format itself is not fixed to .obj.
    [[nodiscard]] static std::filesystem::path ResolveMesh(const AssetDesc& desc);

    /// A name ending in a known image extension addresses that file; anything else is a
    /// folder holding texture.png, which is what the legacy model loader used.
    [[nodiscard]] static std::filesystem::path ResolveTexture(const AssetDesc& desc);

    /// Names are game/..., engine/... or the legacy shaders/... (engine).
    /// The executable directory is the common root for files and generated source locations.
    [[nodiscard]] static std::filesystem::path ResolveShader(const AssetDesc& desc);

    [[nodiscard]] static std::filesystem::path ResolveMaterial(const AssetDesc& desc, bool instance = false);

    /// Default on-disk location for a kind when the caller gave only a logical name.
    [[nodiscard]] static std::filesystem::path DefaultPath(std::string_view name, AssetKind kind);

    [[nodiscard]] static const std::filesystem::path& GetRoot();

    /// Where game/... names resolve; empty restores the default, <root>/game.
    /// Not synchronized: call before the first asset is requested.
    static void SetGameRoot(std::filesystem::path directory);
    [[nodiscard]] static std::filesystem::path GetGameRoot();

    /// A relative path starting with game/ resolves against the game root, any other one
    /// against the executable directory. Absolute paths are returned as they are.
    [[nodiscard]] static std::filesystem::path ToAbsolute(const std::filesystem::path& path);

    /// Normalized game/... or engine/... name of a file, empty when it is in neither root.
    [[nodiscard]] static std::string ToName(const std::filesystem::path& absolute);

    [[nodiscard]] static bool IsImageFile(const std::filesystem::path& path);
    [[nodiscard]] static bool IsModelFile(const std::filesystem::path& path);
};

}  // namespace engine
