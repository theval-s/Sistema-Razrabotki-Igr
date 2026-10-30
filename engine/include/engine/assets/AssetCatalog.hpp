#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <engine/engine_export.hpp>
#include <engine/core/Uuid.hpp>

namespace engine {

struct AssetReference {
    AssetUuid id;
    std::string path; // diagnostic hint only; never used as a fallback lookup key
};

enum class CatalogAssetKind { Mesh, Texture, Shader, Material, MaterialInstance, Scene };

struct AssetMetadata {
    AssetUuid id;
    CatalogAssetKind kind = CatalogAssetKind::Mesh;
    bool centerOnLoad = false; // mesh import setting
};

struct CatalogEntry {
    AssetMetadata metadata;
    std::string path; // root-qualified, physical source path, e.g. game/shaders/foo.hlsl
    std::filesystem::path source;
};

// Pure codecs do not read files. On failure output arguments remain unchanged.
[[nodiscard]] ENGINE_API bool ParseAssetMetadata(std::string_view json, AssetMetadata& value, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeAssetMetadata(const AssetMetadata& value, std::string& json, std::string& error);
[[nodiscard]] ENGINE_API bool ParseAssetReference(std::string_view json, AssetReference& value, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeAssetReference(const AssetReference& value, std::string& json, std::string& error);

// <source>.meta
[[nodiscard]] ENGINE_API std::filesystem::path AssetMetadataPath(const std::filesystem::path& source);
// Reads and validates the sidecar of `source`. On failure `value` is unchanged.
[[nodiscard]] ENGINE_API bool ReadAssetMetadata(const std::filesystem::path& source, AssetMetadata& value,
                                                std::string& error);
// Creates the sidecar of `source`. Fails if one already exists or `value.kind` is not the
// kind CatalogKindForFile() gives the source.
[[nodiscard]] ENGINE_API bool WriteAssetMetadata(const std::filesystem::path& source, const AssetMetadata& value,
                                                 std::string& error);

// Sidecars are adjacent to their source: model.obj.meta, stone.material.json.meta.
// Only recognized source types need metadata; unrelated files are ignored.
// Missing sidecars are reported while valid entries remain usable. Duplicate
// identities or malformed sidecars leave the previous scan unchanged.
class ENGINE_API AssetCatalog {
public:
    [[nodiscard]] bool Scan(const std::filesystem::path& gameRoot,
                            const std::filesystem::path& engineRoot, std::string& error);
    [[nodiscard]] const CatalogEntry* Find(std::string_view uuid, CatalogAssetKind kind) const;
    [[nodiscard]] const CatalogEntry* Find(std::string_view uuid) const;
    [[nodiscard]] const CatalogEntry* Find(const AssetUuid& uuid, CatalogAssetKind kind) const;
    [[nodiscard]] const CatalogEntry* Find(const AssetUuid& uuid) const;
    [[nodiscard]] const CatalogEntry* FindPath(std::string_view logicalPath) const;
    [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }

private:
    std::unordered_map<AssetUuid, CatalogEntry, AssetUuidHash> entries_;
    std::unordered_map<std::string, AssetUuid> paths_;
};

// The kind a source file is catalogued as, from its (double) extension; empty for
// files that need no metadata.
[[nodiscard]] ENGINE_API std::optional<CatalogAssetKind> CatalogKindForFile(const std::filesystem::path& path);

// Explicit authoring operation: creates metadata only for recognized files
// lacking it. Existing sidecars are never rewritten, including invalid ones.
// An error aborts without silently assigning replacement identities.
[[nodiscard]] ENGINE_API bool GenerateMissingAssetMetadata(const std::filesystem::path& root,
                                                             std::size_t& generated, std::string& error);

} // namespace engine
