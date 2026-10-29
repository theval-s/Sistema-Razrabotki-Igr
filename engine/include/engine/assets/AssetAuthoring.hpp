#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>

// Editor-side file operations that keep sources and their .meta sidecars paired, so a
// catalog scan stays valid afterwards. None of them touch a loaded AssetSystem: rescan
// its catalog once the files are in place. Paths are absolute; errors are UTF-8.

namespace engine {

/// Renames a file together with its sidecar, or a whole directory. UUID is the same.
/// Fails when `to` exists (a case-only rename is allowed) or the rename would change the asset kind.
[[nodiscard]] ENGINE_API bool MoveAsset(const std::filesystem::path& from, const std::filesystem::path& to,
                                        std::string& error);

/// Copies a file, or a directory tree, giving every recognized copy a new UUID.
/// Meta files are not copied, only import settings are. References inside a copied tree still
/// point at the originals. Fails without changes when `to` exists or lies inside `from`.
[[nodiscard]] ENGINE_API bool CopyAsset(const std::filesystem::path& from, const std::filesystem::path& to,
                                        std::string& error);

/// JSON files under `root` (scenes, materials, ...) whose text mentions `id`.
/// NOTE: Text search. Using Asset database of some kind might be better
[[nodiscard]] ENGINE_API std::vector<std::filesystem::path> FindAssetReferences(const std::filesystem::path& root,
                                                                               const AssetUuid& id);

}  // namespace engine
