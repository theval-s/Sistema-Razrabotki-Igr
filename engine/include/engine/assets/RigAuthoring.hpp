#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <engine/assets/AssetCatalog.hpp>

namespace engine {

// Version 1 runtime descriptors. Pure codecs leave outputs unchanged on failure.
struct SkeletonDefinition { AssetReference source; };
struct SkinnedMeshDefinition { AssetReference skeleton; };
struct AnimationClipDefinition { AssetReference skeleton; std::string clip; };
ENGINE_API bool ParseSkeletonDefinition(std::string_view json, SkeletonDefinition& value, std::string& error);
ENGINE_API bool ParseSkinnedMeshDefinition(std::string_view json, SkinnedMeshDefinition& value, std::string& error);
ENGINE_API bool ParseAnimationClipDefinition(std::string_view json, AnimationClipDefinition& value, std::string& error);
ENGINE_API bool SerializeSkeletonDefinition(const SkeletonDefinition& value, std::string& json, std::string& error);
ENGINE_API bool SerializeSkinnedMeshDefinition(const SkinnedMeshDefinition& value, std::string& json, std::string& error);
ENGINE_API bool SerializeAnimationClipDefinition(const AnimationClipDefinition& value, std::string& json, std::string& error);

struct RigClipInfo {
    std::string name;
    double durationSeconds = 0;
    bool importable = false;
    std::string diagnostic;
};

struct RigInspection {
    std::string sourceRevision;
    bool importable = false;
    std::uint32_t jointCount = 0;
    std::uint32_t sectionCount = 0;
    std::vector<RigClipInfo> clips;
    bool canExtractBaseColor = false;
    std::vector<std::string> diagnostics;
};

/// CPU-only, read-only and safe on a worker. Uses the runtime conversion rules.
/// A successfully read report may contain unsupported clips; select importable ones.
ENGINE_API bool InspectAnimatedGlb(const std::filesystem::path& source, RigInspection& result, std::string& error);

struct RigAuthoringContext {
    std::filesystem::path gameRoot;
    std::filesystem::path engineRoot;
    /// Outside both scanned roots, on the destination volume (e.g. project/.sri/rig-transactions).
    std::filesystem::path transactionRoot;
};

struct RigClipBinding {
    AssetReference asset;
    std::string sourceName;
    bool removed = false; // reserved UUID; a removed clip is never silently resurrected
};

struct RigAssetGroup {
    std::filesystem::path directory;
    std::string revision;
    AssetReference source;
    AssetReference skeleton;
    AssetReference mesh;
    std::optional<AssetReference> texture;
    std::optional<AssetReference> materialInstance;
    std::vector<RigClipBinding> clips;
    float placementScale = 1;
};

/// Reads rig.import.json, or infers an unambiguous prepared single-source rig from
/// existing descriptors/sidecars. Does not write or assign identities. Paths/UUIDs
/// are checked against the catalog; unrelated neighbouring assets are not owned.
/// UUIDs resolve active paths; hints may be stale. This checks structure, not GLB
/// conversion, so a changed/broken source can be repaired by explicit reimport.
/// Prepared rigs do not infer ownership of neighbouring materials or textures.
ENGINE_API bool ReadRigAssetGroup(const RigAuthoringContext& context, const std::filesystem::path& directory,
                                 RigAssetGroup& result, std::string& error);

struct RigImportRequest {
    std::filesystem::path sourceGlb;
    std::filesystem::path destinationDirectory; // absent dedicated directory beneath gameRoot
    std::string expectedSourceRevision;
    std::vector<std::string> clips;
    std::optional<AssetReference> baseMaterial;
    bool extractBaseColor = true;
    float placementScale = 1;
};

struct RigClipMapping {
    AssetUuid existingId; // empty for a new clip; existing IDs retain their descriptor filenames
    // An explicitly named tombstone may be reactivated with remove=false.
    std::string sourceName;
    bool remove = false;
};

struct RigReimportRequest {
    std::filesystem::path directory;
    std::filesystem::path sourceGlb;
    std::string expectedSourceRevision;
    std::string expectedGroupRevision;
    // One explicit choice per active existing clip; missing/renamed names require
    // remapping or removal. Additional empty-ID choices create new descriptors.
    std::vector<RigClipMapping> clips;
    bool refreshTexture = false; // generated material-instance edits are always preserved
};

enum class RigPublishState { Unchanged, Committed, RecoveryRequired };
struct RigPublishResult {
    RigPublishState state = RigPublishState::Unchanged;
    RigAssetGroup group;
    std::string error;
    std::vector<std::string> warnings;
    std::filesystem::path recoveryDirectory;
};

/// Main-thread authoring operations; serialize with catalog scans and loads. Stage
/// and CPU-validate before publication. Reimport preserves identities and unrelated
/// files. Failure restores the old tree or retains recovery data explicitly. Loaded
/// assets are refreshed at the next scene reload, not by a catalog scan.
ENGINE_API RigPublishResult ImportAnimatedRig(const RigAuthoringContext& context, const RigImportRequest& request);
ENGINE_API RigPublishResult ReimportAnimatedRig(const RigAuthoringContext& context, const RigReimportRequest& request);
/// New identities for owned assets; remap typed internal references only. External
/// materials/textures remain shared. Duplicating scene entities needs no such remap.
ENGINE_API RigPublishResult DuplicateAnimatedRig(const RigAuthoringContext& context,
                                                const std::filesystem::path& directory,
                                                const std::filesystem::path& destinationDirectory,
                                                std::string_view expectedGroupRevision);

} // namespace engine
