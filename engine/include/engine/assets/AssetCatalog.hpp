#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <engine/engine_export.hpp>

namespace engine {

// Canonical lowercase RFC 4122 UUID. This is an authored identity, not a runtime
// AssetHandle or GPU handle. Empty strings are never valid identities.
[[nodiscard]] ENGINE_API bool IsUuid(std::string_view text) noexcept;
[[nodiscard]] ENGINE_API std::string NewUuid();

struct AssetReference {
    std::string id;
    std::string path; // diagnostic hint only; never used as a fallback lookup key
};

enum class CatalogAssetKind { Mesh, Texture, Shader, Material, MaterialInstance, Scene };

struct AssetMetadata {
    std::string id;
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
    [[nodiscard]] const CatalogEntry* FindPath(std::string_view logicalPath) const;
    [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }

private:
    std::unordered_map<std::string, CatalogEntry> entries_;
    std::unordered_map<std::string, std::string> paths_;
};

// Explicit authoring operation: creates metadata only for recognized files
// lacking it. Existing sidecars are never rewritten, including invalid ones.
// An error aborts without silently assigning replacement identities.
[[nodiscard]] ENGINE_API bool GenerateMissingAssetMetadata(const std::filesystem::path& root,
                                                             std::size_t& generated, std::string& error);

} // namespace engine
