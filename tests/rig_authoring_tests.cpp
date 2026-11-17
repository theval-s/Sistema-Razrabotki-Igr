#include <engine/assets/RigAuthoring.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using namespace engine;
namespace fs = std::filesystem;
class RigAuthoring : public ::testing::Test {
protected:
    fs::path root = fs::temp_directory_path() / ("sri-rig-authoring-" + NewUuid());
    RigAuthoringContext context{root / "game", {}, root / ".sri/rig-transactions"};
    fs::path fixture = fs::path(SRI_SOURCE_ROOT) / "example_game/models/TestBot";
    std::string error;
    void SetUp() override { fs::create_directories(context.gameRoot / "models"); }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    RigInspection Inspect(const fs::path& source) {
        RigInspection inspection;
        EXPECT_TRUE(InspectAnimatedGlb(source, inspection, error)) << error;
        return inspection;
    }
    RigPublishResult Import() {
        const auto inspection = Inspect(fixture / "model.glb");
        RigImportRequest request;
        request.sourceGlb = fixture / "model.glb";
        request.destinationDirectory = context.gameRoot / "models/Bot";
        request.expectedSourceRevision = inspection.sourceRevision;
        for (const auto& clip : inspection.clips) if (clip.importable) request.clips.push_back(clip.name);
        return ImportAnimatedRig(context, request);
    }
    fs::path Physical(const AssetReference& ref) { return context.gameRoot / FromUtf8(ref.path.substr(5)); }
    std::string Text(const fs::path& path) {
        std::string text; EXPECT_TRUE(ReadTextFile(path, text, error)) << error; return text;
    }
    void Write(const fs::path& path, std::string_view text) { ASSERT_TRUE(WriteFileAtomic(path, text, error)) << error; }
    template<class Edit> fs::path EditedSource(Edit edit) {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(ReadBinaryFile(fixture / "model.glb", bytes, error)) << error;
        const auto word = [&bytes](std::size_t p) { return std::uint32_t(bytes[p]) | (std::uint32_t(bytes[p+1]) << 8) | (std::uint32_t(bytes[p+2]) << 16) | (std::uint32_t(bytes[p+3]) << 24); };
        const auto jsonLength = word(12);
        auto json = nlohmann::json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLength);
        std::vector<std::uint8_t> bin(bytes.begin() + 28 + jsonLength, bytes.end());
        edit(json, bin);
        while (bin.size() % 4) bin.push_back(0);
        json["buffers"][0]["byteLength"] = bin.size();
        auto text = json.dump(); while (text.size() % 4) text += ' ';
        std::vector<std::uint8_t> output;
        const auto append = [&output](std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) output.push_back(static_cast<std::uint8_t>(v >> (i * 8))); };
        append(0x46546c67); append(2); append(static_cast<std::uint32_t>(28 + text.size() + bin.size()));
        append(static_cast<std::uint32_t>(text.size())); append(0x4e4f534a);
        output.insert(output.end(), text.begin(), text.end());
        append(static_cast<std::uint32_t>(bin.size())); append(0x004e4942);
        output.insert(output.end(), bin.begin(), bin.end());
        const auto path = root / "source.glb";
        EXPECT_TRUE(WriteFileAtomic(path, {reinterpret_cast<const char*>(output.data()), output.size()}, error)) << error;
        return path;
    }
};

TEST_F(RigAuthoring, InspectionAndNewGraphUseRuntimeNamesAndTypedReferences) {
    const auto inspection = Inspect(fixture / "model.glb");
    ASSERT_TRUE(inspection.importable);
    ASSERT_EQ(inspection.clips.size(), 2u);
    EXPECT_GT(inspection.jointCount, 0u);
    EXPECT_GT(inspection.sectionCount, 0u);
    for (const auto& clip : inspection.clips) { EXPECT_TRUE(clip.importable) << clip.diagnostic; EXPECT_GT(clip.durationSeconds, 0); }
    const auto result = Import();
    ASSERT_EQ(result.state, RigPublishState::Committed) << result.error;
    AssetCatalog catalog; ASSERT_TRUE(catalog.Scan(context.gameRoot, {}, error)) << error;
    EXPECT_EQ(catalog.Size(), 5u);
    EXPECT_FALSE(CatalogKindForFile(result.group.directory / "rig.import.json").has_value());
    SkeletonDefinition skeleton;
    ASSERT_TRUE(ParseSkeletonDefinition(Text(Physical(result.group.skeleton)), skeleton, error)) << error;
    EXPECT_EQ(skeleton.source, result.group.source);
    SkinnedMeshDefinition mesh;
    ASSERT_TRUE(ParseSkinnedMeshDefinition(Text(Physical(result.group.mesh)), mesh, error));
    EXPECT_EQ(mesh.skeleton, result.group.skeleton);
    RigAssetGroup reread;
    ASSERT_TRUE(ReadRigAssetGroup(context, result.group.directory, reread, error)) << error;
    EXPECT_EQ(reread.revision, result.group.revision);
    EXPECT_EQ(reread.source, result.group.source);
}

TEST_F(RigAuthoring, FailedValidationAndStaleRevisionLeaveDestinationUnchanged) {
    auto result = Import(); ASSERT_EQ(result.state, RigPublishState::Committed) << result.error;
    const auto originalManifest = Text(result.group.directory / "rig.import.json");
    const auto originalSource = Inspect(Physical(result.group.source)).sourceRevision;
    RigReimportRequest request;
    request.directory = result.group.directory;
    request.sourceGlb = fixture / "model.glb";
    request.expectedSourceRevision = originalSource;
    request.expectedGroupRevision = result.group.revision;
    for (const auto& clip : result.group.clips) request.clips.push_back({clip.asset.id, "absent clip", false});
    const auto failed = ReimportAnimatedRig(context, request);
    EXPECT_EQ(failed.state, RigPublishState::Unchanged); EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(Text(result.group.directory / "rig.import.json"), originalManifest);
    EXPECT_EQ(Inspect(Physical(result.group.source)).sourceRevision, originalSource);
    Write(result.group.directory / "notes.txt", "external edit");
    request.clips.clear();
    for (const auto& clip : result.group.clips) request.clips.push_back({clip.asset.id, clip.sourceName, false});
    const auto stale = ReimportAnimatedRig(context, request);
    EXPECT_EQ(stale.state, RigPublishState::Unchanged);
    EXPECT_NE(stale.error.find("revision"), std::string::npos);
    EXPECT_EQ(Text(result.group.directory / "notes.txt"), "external edit");
    EXPECT_EQ(Text(result.group.directory / "rig.import.json"), originalManifest);
}

TEST_F(RigAuthoring, RemapAndRemovalRetainIdentitiesAndReserveRemovedNames) {
    auto imported = Import(); ASSERT_EQ(imported.state, RigPublishState::Committed) << imported.error;
    ASSERT_EQ(imported.group.clips.size(), 2u);
    Write(imported.group.directory / "notes.txt", "keep me");
    fs::create_directory(imported.group.directory / "empty-user-folder");
    RigAssetGroup before;
    ASSERT_TRUE(ReadRigAssetGroup(context, imported.group.directory, before, error)) << error;
    RigReimportRequest request;
    request.directory = before.directory; request.sourceGlb = fixture / "model.glb";
    request.expectedSourceRevision = Inspect(request.sourceGlb).sourceRevision;
    request.expectedGroupRevision = before.revision;
    request.clips = {{before.clips[0].asset.id, before.clips[1].sourceName, false},
                     {before.clips[1].asset.id, {}, true}};
    auto changed = ReimportAnimatedRig(context, request);
    ASSERT_EQ(changed.state, RigPublishState::Committed) << changed.error;
    EXPECT_EQ(changed.group.source, before.source); EXPECT_EQ(changed.group.skeleton, before.skeleton); EXPECT_EQ(changed.group.mesh, before.mesh);
    EXPECT_EQ(changed.group.clips[0].asset, before.clips[0].asset);
    EXPECT_EQ(changed.group.clips[0].sourceName, before.clips[1].sourceName);
    EXPECT_TRUE(changed.group.clips[1].removed);
    EXPECT_FALSE(fs::exists(Physical(before.clips[1].asset)));
    EXPECT_FALSE(fs::exists(AssetMetadataPath(Physical(before.clips[1].asset))));
    EXPECT_EQ(Text(before.directory / "notes.txt"), "keep me");
    EXPECT_TRUE(fs::is_directory(before.directory / "empty-user-folder"));
    RigAssetGroup read;
    ASSERT_TRUE(ReadRigAssetGroup(context, before.directory, read, error)) << error;
    request.expectedGroupRevision = read.revision;
    request.clips = {{read.clips[0].asset.id, before.clips[0].sourceName, false}, {{}, before.clips[1].sourceName, false}};
    auto added = ReimportAnimatedRig(context, request);
    ASSERT_EQ(added.state, RigPublishState::Committed) << added.error;
    ASSERT_EQ(added.group.clips.size(), 3u);
    EXPECT_NE(added.group.clips[2].asset.id, before.clips[1].asset.id);
    EXPECT_NE(added.group.clips[2].asset.path, before.clips[1].asset.path);
    EXPECT_TRUE(added.group.clips[1].removed);
}

TEST_F(RigAuthoring, DuplicateRemapsOwnedGraphAndSharesExternalMaterial) {
    const auto materialPath = context.gameRoot / "base.material.json";
    // The material codec validates syntax, not shader availability; the rig only
    // owns the instance, and its external material is checked through the catalog.
    MaterialDefinition material; material.shaderFile = "engine/shaders/base.hlsl";
    std::string materialJson; ASSERT_TRUE(SerializeMaterialDefinition(material, materialJson, error)) << error;
    Write(materialPath, materialJson);
    AssetReference base{NewUuid(), "game/base.material.json"};
    ASSERT_TRUE(WriteAssetMetadata(materialPath, {base.id, CatalogAssetKind::Material}, error)) << error;
    RigImportRequest request; request.sourceGlb = fixture / "model.glb";
    request.destinationDirectory = context.gameRoot / "models/Bot";
    const auto inspected = Inspect(request.sourceGlb); request.expectedSourceRevision = inspected.sourceRevision;
    for (const auto& clip : inspected.clips) request.clips.push_back(clip.name);
    request.baseMaterial = base;
    auto imported = ImportAnimatedRig(context, request);
    ASSERT_EQ(imported.state, RigPublishState::Committed) << imported.error;
    ASSERT_TRUE(imported.group.materialInstance);
    MaterialInstanceDefinition instance;
    ASSERT_TRUE(ParseMaterialInstanceDefinition(Text(Physical(*imported.group.materialInstance)), instance, error)) << error;
    instance.parameters.shininess = 42.f;
    std::string json; ASSERT_TRUE(SerializeMaterialInstanceDefinition(instance, json, error));
    Write(Physical(*imported.group.materialInstance), json);
    // UUID references survive external base-material moves without rewriting
    // the user's instance (including its now-stale diagnostic hint).
    fs::rename(materialPath, context.gameRoot / "renamed.material.json");
    fs::rename(AssetMetadataPath(materialPath), AssetMetadataPath(context.gameRoot / "renamed.material.json"));
    Write(imported.group.directory / "unrelated.lua", "return '" + imported.group.source.id.ToString() + "'");
    RigAssetGroup group;
    ASSERT_TRUE(ReadRigAssetGroup(context, imported.group.directory, group, error)) << error;
    RigReimportRequest reimport;
    reimport.directory = group.directory; reimport.sourceGlb = fixture / "model.glb";
    reimport.expectedGroupRevision = group.revision; reimport.expectedSourceRevision = inspected.sourceRevision;
    for (const auto& clip : group.clips) reimport.clips.push_back({clip.asset.id, clip.sourceName, false});
    const auto refreshed = ReimportAnimatedRig(context, reimport);
    ASSERT_EQ(refreshed.state, RigPublishState::Committed) << refreshed.error;
    EXPECT_EQ(Text(Physical(*group.materialInstance)), json);
    group = refreshed.group;
    auto duplicate = DuplicateAnimatedRig(context, group.directory, context.gameRoot / "models/Copy", group.revision);
    ASSERT_EQ(duplicate.state, RigPublishState::Committed) << duplicate.error;
    EXPECT_NE(duplicate.group.source.id, group.source.id); EXPECT_NE(duplicate.group.skeleton.id, group.skeleton.id); EXPECT_NE(duplicate.group.mesh.id, group.mesh.id);
    EXPECT_FALSE(fs::exists(duplicate.group.directory / "unrelated.lua"));
    EXPECT_EQ(Text(group.directory / "unrelated.lua"), "return '" + group.source.id.ToString() + "'");
    SkeletonDefinition skeleton;
    ASSERT_TRUE(ParseSkeletonDefinition(Text(Physical(duplicate.group.skeleton)), skeleton, error));
    EXPECT_EQ(skeleton.source, duplicate.group.source);
    for (const auto& clip : duplicate.group.clips) {
        AnimationClipDefinition definition;
        ASSERT_TRUE(ParseAnimationClipDefinition(Text(Physical(clip.asset)), definition, error));
        EXPECT_EQ(definition.skeleton, duplicate.group.skeleton);
    }
    ASSERT_TRUE(ParseMaterialInstanceDefinition(Text(Physical(*duplicate.group.materialInstance)), instance, error));
    EXPECT_EQ(instance.materialReference, base); EXPECT_EQ(instance.parameters.shininess, 42.f);
}

TEST_F(RigAuthoring, AdoptsPreparedTestBotWithoutWritingOrAssigningIdentities) {
    const auto bot = context.gameRoot / "models/TestBot";
    fs::copy(fixture, bot, fs::copy_options::recursive);
    const auto original = Text(bot / "model.skeleton.json");
    RigAssetGroup group;
    ASSERT_TRUE(ReadRigAssetGroup(context, bot, group, error)) << error;
    EXPECT_EQ(group.source.id.ToString(), "b52e7e3e-920c-48b7-b782-8f403e061001");
    EXPECT_EQ(group.clips.size(), 2u);
    EXPECT_FALSE(fs::exists(bot / "rig.import.json"));
    EXPECT_EQ(Text(bot / "model.skeleton.json"), original);
    auto duplicated = DuplicateAnimatedRig(context, bot, context.gameRoot / "models/Independent", group.revision);
    EXPECT_EQ(duplicated.state, RigPublishState::Committed) << duplicated.error;
    EXPECT_NE(duplicated.group.source.id, group.source.id);
    EXPECT_FALSE(fs::exists(bot / "rig.import.json"));
}

TEST_F(RigAuthoring, DescriptorCodecsRejectDuplicateFieldsAndKeepOutputs) {
    SkeletonDefinition definition{{NewUuid(), "game/model.glb"}};
    const auto before = definition.source;
    EXPECT_FALSE(ParseSkeletonDefinition(R"({"version":1,"version":1,"source":{}})", definition, error));
    EXPECT_EQ(definition.source, before);
    std::string json;
    ASSERT_TRUE(SerializeSkeletonDefinition(definition, json, error)) << error;
    SkeletonDefinition parsed; ASSERT_TRUE(ParseSkeletonDefinition(json, parsed, error));
    EXPECT_EQ(parsed.source, before);
}

TEST_F(RigAuthoring, UnsupportedClipDoesNotHideImportableClips) {
    const auto path = EditedSource([](auto& json, auto&) { json["animations"][0]["samplers"][0]["interpolation"] = "STEP"; });
    const auto inspection = Inspect(path);
    ASSERT_TRUE(inspection.importable);
    ASSERT_EQ(inspection.clips.size(), 2u);
    EXPECT_EQ(inspection.clips[0].name, "idle"); EXPECT_FALSE(inspection.clips[0].importable);
    EXPECT_FALSE(inspection.clips[0].diagnostic.empty());
    EXPECT_EQ(inspection.clips[1].name, "walk"); EXPECT_TRUE(inspection.clips[1].importable);
    EXPECT_DOUBLE_EQ(inspection.clips[0].durationSeconds, 1.0);
    RigImportRequest request; request.sourceGlb = path; request.expectedSourceRevision = inspection.sourceRevision;
    request.destinationDirectory = context.gameRoot / "models/Selected"; request.clips = {"walk"};
    const auto selected = ImportAnimatedRig(context, request);
    EXPECT_EQ(selected.state, RigPublishState::Committed) << selected.error;
    request.destinationDirectory = context.gameRoot / "models/Rejected"; request.clips = {"idle"};
    const auto rejected = ImportAnimatedRig(context, request);
    EXPECT_EQ(rejected.state, RigPublishState::Unchanged);
    EXPECT_FALSE(fs::exists(request.destinationDirectory));
}

TEST_F(RigAuthoring, ExtractsEmbeddedSingleMaterialBaseColorWithOwnIdentity) {
    // A complete 1x1 PNG, embedded in a newly appended buffer view.
    const std::vector<std::uint8_t> png = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,
        0,0,0,11,73,68,65,84,120,218,99,100,248,15,0,1,5,1,1,39,24,227,102,0,0,0,0,73,69,78,68,174,66,96,130};
    const auto path = EditedSource([&](auto& json, auto& bin) {
        const auto view = json["bufferViews"].size();
        json["bufferViews"].push_back({{"buffer",0},{"byteOffset",bin.size()},{"byteLength",png.size()}});
        bin.insert(bin.end(), png.begin(), png.end());
        json["images"] = nlohmann::json::array({{{"bufferView",view},{"mimeType","image/png"}}});
        json["textures"] = nlohmann::json::array({{{"source",0}}});
        json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {{"index",0}};
    });
    const auto inspection = Inspect(path); ASSERT_TRUE(inspection.canExtractBaseColor);
    RigImportRequest request; request.sourceGlb = path; request.expectedSourceRevision = inspection.sourceRevision;
    request.destinationDirectory = context.gameRoot / "models/Textured"; request.clips = {"idle"};
    auto result = ImportAnimatedRig(context, request);
    ASSERT_EQ(result.state, RigPublishState::Committed) << result.error;
    ASSERT_TRUE(result.group.texture);
    std::vector<std::uint8_t> extracted;
    ASSERT_TRUE(ReadBinaryFile(Physical(*result.group.texture), extracted, error)); EXPECT_EQ(extracted, png);
    AssetMetadata metadata; ASSERT_TRUE(ReadAssetMetadata(Physical(*result.group.texture), metadata, error));
    EXPECT_EQ(metadata.id, result.group.texture->id); EXPECT_EQ(metadata.kind, CatalogAssetKind::Texture);
}

TEST_F(RigAuthoring, RejectsUnsafeTransactionRootsAndSourceRevisionsBeforePublication) {
    RigImportRequest request; request.sourceGlb = fixture / "model.glb";
    request.destinationDirectory = context.gameRoot / "models/Unsafe";
    request.expectedSourceRevision = "stale"; request.clips = {"idle"};
    auto stale = ImportAnimatedRig(context, request);
    EXPECT_EQ(stale.state, RigPublishState::Unchanged); EXPECT_FALSE(fs::exists(request.destinationDirectory));
    request.expectedSourceRevision = Inspect(request.sourceGlb).sourceRevision;
    auto unsafe = context; unsafe.transactionRoot = context.gameRoot / "transactions";
    auto rejected = ImportAnimatedRig(unsafe, request);
    EXPECT_EQ(rejected.state, RigPublishState::Unchanged);
    EXPECT_FALSE(fs::exists(request.destinationDirectory));
    EXPECT_FALSE(fs::exists(unsafe.transactionRoot));
    request.destinationDirectory = fs::path("models") / ".." / "Unsafe";
    rejected = ImportAnimatedRig(context, request);
    EXPECT_EQ(rejected.state, RigPublishState::Unchanged);
    EXPECT_FALSE(fs::exists(context.gameRoot / "Unsafe"));
}

TEST_F(RigAuthoring, ChangedSourceCanBeReadAndRepairedWithExplicitMappings) {
    const auto imported = Import(); ASSERT_EQ(imported.state, RigPublishState::Committed) << imported.error;
    const auto changed = EditedSource([](auto& json, auto&) { json["animations"][0]["name"] = "rest"; });
    fs::copy_file(changed, Physical(imported.group.source), fs::copy_options::overwrite_existing);
    RigAssetGroup group;
    ASSERT_TRUE(ReadRigAssetGroup(context, imported.group.directory, group, error)) << error;
    RigReimportRequest request;
    request.directory = group.directory; request.sourceGlb = Physical(group.source);
    request.expectedSourceRevision = Inspect(request.sourceGlb).sourceRevision;
    request.expectedGroupRevision = group.revision;
    for (const auto& clip : group.clips)
        request.clips.push_back({clip.asset.id, clip.sourceName == "idle" ? "rest" : clip.sourceName, false});
    const auto repaired = ReimportAnimatedRig(context, request);
    ASSERT_EQ(repaired.state, RigPublishState::Committed) << repaired.error;
    EXPECT_EQ(repaired.group.clips[0].asset.id, group.clips[0].asset.id);

    // Even a corrupt current source must not prevent selecting a good replacement.
    Write(Physical(group.source), "broken GLB");
    ASSERT_TRUE(ReadRigAssetGroup(context, group.directory, group, error)) << error;
    request.expectedGroupRevision = group.revision; request.sourceGlb = changed;
    const auto replaced = ReimportAnimatedRig(context, request);
    EXPECT_EQ(replaced.state, RigPublishState::Committed) << replaced.error;
}

TEST_F(RigAuthoring, FolderAndAssetMovesResolveByUuidIncludingTombstones) {
    auto imported = Import(); ASSERT_EQ(imported.state, RigPublishState::Committed) << imported.error;
    RigReimportRequest request;
    request.directory = imported.group.directory; request.sourceGlb = fixture / "model.glb";
    request.expectedSourceRevision = Inspect(request.sourceGlb).sourceRevision;
    request.expectedGroupRevision = imported.group.revision;
    for (std::size_t i = 0; i < imported.group.clips.size(); ++i)
        request.clips.push_back({imported.group.clips[i].asset.id, imported.group.clips[i].sourceName, i == 0});
    const auto removed = ReimportAnimatedRig(context, request);
    ASSERT_EQ(removed.state, RigPublishState::Committed) << removed.error;
    const auto moved = context.gameRoot / "models/Moved";
    fs::rename(removed.group.directory, moved);
    fs::rename(moved / "model.skeleton.json", moved / "renamed.skeleton.json");
    fs::rename(moved / "model.skeleton.json.meta", moved / "renamed.skeleton.json.meta");
    RigAssetGroup group;
    ASSERT_TRUE(ReadRigAssetGroup(context, moved, group, error)) << error;
    EXPECT_EQ(group.skeleton.id, imported.group.skeleton.id);
    EXPECT_EQ(group.skeleton.path, "game/models/Moved/renamed.skeleton.json");
    ASSERT_TRUE(group.clips[0].removed);
    EXPECT_TRUE(group.clips[0].asset.path.starts_with("game/models/Moved/"));
    // Earlier manifests did not store a relative path for absent descriptors.
    auto legacy = nlohmann::json::parse(Text(moved / "rig.import.json"));
    for (auto& clip : legacy["clips"]) clip.erase("relativePath");
    Write(moved / "rig.import.json", legacy.dump());
    ASSERT_TRUE(ReadRigAssetGroup(context, moved, group, error)) << error;
    EXPECT_TRUE(group.clips[0].asset.path.starts_with("game/models/Moved/"));
    auto duplicated = DuplicateAnimatedRig(context, moved, context.gameRoot / "models/Copy", group.revision);
    ASSERT_EQ(duplicated.state, RigPublishState::Committed) << duplicated.error;
    EXPECT_NE(duplicated.group.clips[0].asset.id, group.clips[0].asset.id);
    EXPECT_TRUE(duplicated.group.clips[0].removed);

    // Only naming the reserved ID explicitly reactivates it.
    request.directory = moved; request.expectedGroupRevision = group.revision;
    request.clips.clear();
    for (const auto& clip : group.clips) request.clips.push_back({clip.asset.id, clip.sourceName, false});
    auto restored = ReimportAnimatedRig(context, request);
    ASSERT_EQ(restored.state, RigPublishState::Committed) << restored.error;
    EXPECT_FALSE(restored.group.clips[0].removed);
    EXPECT_EQ(restored.group.clips[0].asset.id, imported.group.clips[0].asset.id);
}

TEST_F(RigAuthoring, PreparedAdoptionDoesNotClaimNeighbouringAppearanceAssets) {
    const auto bot = context.gameRoot / "models/Relocated";
    fs::copy(fixture, bot, fs::copy_options::recursive);
    // A unique local instance -> texture relationship does not prove ownership.
    const AssetReference texture{NewUuid(), "game/models/Relocated/user.png"};
    Write(bot / "user.png", "user texture");
    ASSERT_TRUE(WriteAssetMetadata(bot / "user.png", {texture.id, CatalogAssetKind::Texture}, error));
    MaterialInstanceDefinition instance; instance.version = 2;
    instance.materialReference = AssetReference{NewUuid(), "game/user.material.json"};
    instance.material = instance.materialReference->path;
    instance.parameters.albedoReference = texture;
    instance.parameters.albedoTexture = texture.path;
    std::string text; ASSERT_TRUE(SerializeMaterialInstanceDefinition(instance, text, error)) << error;
    Write(bot / "user.material-instance.json", text);
    ASSERT_TRUE(WriteAssetMetadata(bot / "user.material-instance.json", {NewUuid(), CatalogAssetKind::MaterialInstance}, error));
    RigAssetGroup group;
    ASSERT_TRUE(ReadRigAssetGroup(context, bot, group, error)) << error;
    EXPECT_FALSE(group.texture); EXPECT_FALSE(group.materialInstance);
    const auto copy = DuplicateAnimatedRig(context, bot, context.gameRoot / "models/Copy", group.revision);
    ASSERT_EQ(copy.state, RigPublishState::Committed) << copy.error;
    EXPECT_FALSE(fs::exists(copy.group.directory / "user.png"));
    EXPECT_FALSE(fs::exists(copy.group.directory / "user.material-instance.json"));
    EXPECT_EQ(Text(bot / "user.material-instance.json"), text);
}

TEST_F(RigAuthoring, RejectsCorruptBinLengthBeforePublishing) {
    const auto source = EditedSource([](auto&, auto&) {});
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadBinaryFile(source, bytes, error));
    const std::size_t jsonLength = std::uint32_t(bytes[12]) | (std::uint32_t(bytes[13]) << 8) |
        (std::uint32_t(bytes[14]) << 16) | (std::uint32_t(bytes[15]) << 24);
    for (std::size_t i = 0; i < 4; ++i) bytes[20 + jsonLength + i] = 255;
    ASSERT_TRUE(WriteFileAtomic(source, {reinterpret_cast<const char*>(bytes.data()), bytes.size()}, error));
    RigInspection inspection;
    EXPECT_FALSE(InspectAnimatedGlb(source, inspection, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(RigAuthoring, ImageSignatureAloneDoesNotMakeBaseColorExtractable) {
    const auto source = EditedSource([](auto& json, auto& bin) {
        const std::vector<std::uint8_t> header{137,80,78,71,13,10,26,10};
        const auto view = json["bufferViews"].size();
        json["bufferViews"].push_back({{"buffer",0},{"byteOffset",bin.size()},{"byteLength",header.size()}});
        bin.insert(bin.end(), header.begin(), header.end());
        json["images"] = nlohmann::json::array({{{"bufferView",view},{"mimeType","image/png"}}});
        json["textures"] = nlohmann::json::array({{{"source",0}}});
        json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {{"index",0}};
    });
    const auto inspection = Inspect(source);
    EXPECT_TRUE(inspection.importable);
    EXPECT_FALSE(inspection.canExtractBaseColor);
    EXPECT_FALSE(inspection.diagnostics.empty());
}

#ifdef _WIN32
TEST_F(RigAuthoring, SharingViolationLeavesPublishedTreeAndUserEditsIntact) {
    const auto imported = Import(); ASSERT_EQ(imported.state, RigPublishState::Committed) << imported.error;
    Write(imported.group.directory / "notes.txt", "keep latest user bytes");
    RigAssetGroup group;
    ASSERT_TRUE(ReadRigAssetGroup(context, imported.group.directory, group, error)) << error;
    const auto manifest = Text(group.directory / "rig.import.json");
    const auto handle = CreateFileW(group.directory.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    RigReimportRequest request;
    request.directory = group.directory; request.sourceGlb = fixture / "model.glb";
    request.expectedSourceRevision = Inspect(request.sourceGlb).sourceRevision;
    request.expectedGroupRevision = group.revision;
    for (const auto& clip : group.clips) request.clips.push_back({clip.asset.id, clip.sourceName, false});
    const auto failed = ReimportAnimatedRig(context, request);
    CloseHandle(handle);
    EXPECT_EQ(failed.state, RigPublishState::Unchanged) << failed.error;
    EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(Text(group.directory / "rig.import.json"), manifest);
    EXPECT_EQ(Text(group.directory / "notes.txt"), "keep latest user bytes");
    RigAssetGroup after;
    ASSERT_TRUE(ReadRigAssetGroup(context, group.directory, after, error)) << error;
    EXPECT_EQ(after.revision, group.revision);
}
#endif
} // namespace
