#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetCooking.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/FileSystem.hpp>
#include "animation/AnimationData.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using namespace engine;
namespace fs = std::filesystem;

struct GameRootScope {
    fs::path previous = AssetPathResolver::GetGameRoot();
    explicit GameRootScope(const fs::path& root) { AssetPathResolver::SetGameRoot(root); }
    ~GameRootScope() { AssetPathResolver::SetGameRoot(previous); }
};

class AssetCooking : public ::testing::Test {
protected:
    fs::path root = fs::temp_directory_path() /
        ("sri-cooking-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path bot = root / "models" / "TestBot";
    GameRootScope gameRoot{root};
    AssetCatalog catalog;
    std::string error;

    void SetUp() override {
        ASSERT_TRUE(fs::create_directories(bot));
        fs::copy(fs::path(SRI_SOURCE_ROOT) / "example_game/models/TestBot", bot, fs::copy_options::recursive);
        fs::copy(fs::path(SRI_SOURCE_ROOT) / "example_game/models/Table", root / "models" / "Table",
                 fs::copy_options::recursive);
        ASSERT_TRUE(catalog.Scan(root, {}, error)) << error;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    CpuPayload Import(AssetKind kind, const fs::path& file, bool center = false) {
        AssetDesc desc;
        desc.path = file;
        desc.catalog = &catalog;
        desc.centerOnLoad = center;
        CpuPayload payload;
        const bool read = kind == AssetKind::Mesh ? AssimpMeshImporter{}.ReadCpu(desc, nullptr, payload, error)
                                                  : SkeletalAssetImporter{kind}.ReadCpu(desc, nullptr, payload, error);
        EXPECT_TRUE(read) << error;
        return payload;
    }

    // Cooks `file` beside itself, then loads it back through the cooked importer.
    CpuPayload CookAndLoad(AssetKind kind, const fs::path& file, bool center = false) {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(SerializeCooked(kind, Import(kind, file), bytes, error)) << error;
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        EXPECT_TRUE(WriteFileAtomic(CookedPath(file), text, error)) << error;
        std::unique_ptr<IAssetImporter> uploader;
        if (kind == AssetKind::Mesh) uploader = std::make_unique<AssimpMeshImporter>();
        else uploader = std::make_unique<SkeletalAssetImporter>(kind);
        AssetDesc desc;
        desc.path = file;
        desc.centerOnLoad = center;
        CpuPayload payload;
        EXPECT_TRUE(CookedAssetImporter(kind, std::move(uploader)).ReadCpu(desc, nullptr, payload, error)) << error;
        return payload;
    }

    std::vector<std::uint8_t> Cooked(AssetKind kind, const fs::path& file) {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(SerializeCooked(kind, Import(kind, file), bytes, error)) << error;
        return bytes;
    }
};

void ExpectSameMesh(const MeshCpu& actual, const MeshCpu& expected) {
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    EXPECT_EQ(std::memcmp(actual.vertices.data(), expected.vertices.data(),
                          actual.vertices.size() * sizeof(actual.vertices[0])), 0);
    EXPECT_EQ(actual.indices, expected.indices);
    ASSERT_EQ(actual.sections.size(), expected.sections.size());
    for (std::size_t i = 0; i < actual.sections.size(); ++i) {
        EXPECT_EQ(actual.sections[i].firstVertex, expected.sections[i].firstVertex);
        EXPECT_EQ(actual.sections[i].vertexCount, expected.sections[i].vertexCount);
        EXPECT_EQ(actual.sections[i].firstIndex, expected.sections[i].firstIndex);
        EXPECT_EQ(actual.sections[i].indexCount, expected.sections[i].indexCount);
        EXPECT_EQ(actual.sections[i].materialSlot, expected.sections[i].materialSlot);
    }
    EXPECT_EQ(actual.materialSlots, expected.materialSlots);
    EXPECT_EQ(actual.bounds, expected.bounds);
    ASSERT_TRUE(actual.geometry && expected.geometry);
    EXPECT_EQ(actual.geometry->indices, expected.geometry->indices);
    ASSERT_EQ(actual.geometry->positions.size(), expected.geometry->positions.size());
    EXPECT_EQ(std::memcmp(actual.geometry->positions.data(), expected.geometry->positions.data(),
                          actual.geometry->positions.size() * sizeof(actual.geometry->positions[0])), 0);
}

TEST_F(AssetCooking, CookedMeshLoadsLikeTheSource) {
    for (const auto& file : {root / "models/Table/model.obj", bot / "model.glb"}) {
        SCOPED_TRACE(file.string());
        ExpectSameMesh(CookAndLoad(AssetKind::Mesh, file).mesh, Import(AssetKind::Mesh, file).mesh);
    }
}

TEST_F(AssetCooking, CenteringIsAppliedWhenTheCookedMeshLoads) {
    const auto file = root / "models/Table/model.obj";
    ExpectSameMesh(CookAndLoad(AssetKind::Mesh, file, true).mesh, Import(AssetKind::Mesh, file, true).mesh);
}

TEST_F(AssetCooking, CookedSkeletalAssetsLoadLikeTheSources) {
    const auto skeleton = Import(AssetKind::Skeleton, bot / "model.skeleton.json").skeleton;
    const auto cookedSkeleton = CookAndLoad(AssetKind::Skeleton, bot / "model.skeleton.json").skeleton;
    EXPECT_EQ(cookedSkeleton.signature, skeleton.signature);
    EXPECT_EQ(cookedSkeleton.jointCount, skeleton.jointCount);
    ASSERT_TRUE(cookedSkeleton.data && skeleton.data);
    EXPECT_EQ(cookedSkeleton.data->nodePaths, skeleton.data->nodePaths);
    ASSERT_EQ(cookedSkeleton.data->skeleton.num_joints(), skeleton.data->skeleton.num_joints());
    for (int i = 0; i < skeleton.data->skeleton.num_joints(); ++i) {
        EXPECT_STREQ(cookedSkeleton.data->skeleton.joint_names()[i], skeleton.data->skeleton.joint_names()[i]);
        EXPECT_EQ(cookedSkeleton.data->skeleton.joint_parents()[i], skeleton.data->skeleton.joint_parents()[i]);
    }

    const auto clip = Import(AssetKind::AnimationClip, bot / "walk.animation.json").animationClip;
    const auto cookedClip = CookAndLoad(AssetKind::AnimationClip, bot / "walk.animation.json").animationClip;
    EXPECT_EQ(cookedClip.skeletonSignature, clip.skeletonSignature);
    EXPECT_FLOAT_EQ(cookedClip.duration, clip.duration);
    ASSERT_TRUE(cookedClip.data && clip.data);
    EXPECT_EQ(cookedClip.data->animation.num_tracks(), clip.data->animation.num_tracks());
    EXPECT_FLOAT_EQ(cookedClip.data->animation.duration(), clip.data->animation.duration());
    EXPECT_EQ(cookedClip.data->animation.size(), clip.data->animation.size());

    const auto mesh = Import(AssetKind::SkinnedMesh, bot / "model.skinned-mesh.json").skinnedMesh;
    const auto cookedMesh = CookAndLoad(AssetKind::SkinnedMesh, bot / "model.skinned-mesh.json").skinnedMesh;
    EXPECT_EQ(cookedMesh.bounds, mesh.bounds);
    EXPECT_EQ(cookedMesh.skeletonSignature, mesh.skeletonSignature);
    EXPECT_EQ(cookedMesh.skeletonReference, mesh.skeletonReference);
    ASSERT_EQ(cookedMesh.sections.size(), mesh.sections.size());
    for (std::size_t i = 0; i < mesh.sections.size(); ++i) {
        const auto& a = cookedMesh.sections[i];
        const auto& b = mesh.sections[i];
        ASSERT_EQ(a.vertices.size(), b.vertices.size());
        EXPECT_EQ(std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(SkinnedVertex)), 0);
        EXPECT_EQ(a.indices, b.indices);
        EXPECT_EQ(a.materialSlot, b.materialSlot);
        ASSERT_TRUE(a.binding && b.binding);
        EXPECT_EQ(a.binding->joints, b.binding->joints);
        EXPECT_EQ(a.binding->inverseBind.size(), b.binding->inverseBind.size());
        EXPECT_EQ(a.binding->restPalette.size(), b.binding->restPalette.size());
        EXPECT_EQ(a.binding->jointBounds, b.binding->jointBounds);
    }
}

TEST_F(AssetCooking, RejectsDamagedOrForeignCookedData) {
    const auto bytes = Cooked(AssetKind::Mesh, root / "models/Table/model.obj");
    CpuPayload payload;
    payload.kind = CpuPayload::Kind::Texture; // untouched on failure
    const auto expectFailure = [&](std::vector<std::uint8_t> data, AssetKind kind, const char* reason) {
        SCOPED_TRACE(reason);
        EXPECT_FALSE(DeserializeCooked(kind, data, payload, error));
        EXPECT_NE(error.find(reason), std::string::npos) << error;
        EXPECT_EQ(payload.kind, CpuPayload::Kind::Texture);
    };
    ASSERT_TRUE(DeserializeCooked(AssetKind::Mesh, bytes, payload, error)) << error;
    payload.kind = CpuPayload::Kind::Texture;

    expectFailure({bytes.begin(), bytes.begin() + 10}, AssetKind::Mesh, "too short");
    expectFailure(bytes, AssetKind::Skeleton, "wrong kind");
    auto data = bytes;
    data[0] = 'X';
    expectFailure(data, AssetKind::Mesh, "not a cooked asset");
    data = bytes;
    ++data[8];
    expectFailure(data, AssetKind::Mesh, "different engine version");
    data = bytes;
    data.back() ^= 0x40;
    expectFailure(data, AssetKind::Mesh, "hash mismatch");
    data = bytes;
    data.pop_back();
    expectFailure(data, AssetKind::Mesh, "truncated");
}

TEST_F(AssetCooking, RejectsAMeshWhoseIndicesLeaveTheirSection) {
    auto payload = Import(AssetKind::Mesh, root / "models/Table/model.obj");
    payload.mesh.indices.back() = static_cast<std::uint32_t>(payload.mesh.vertices.size());
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(SerializeCooked(AssetKind::Mesh, payload, bytes, error)) << error;
    CpuPayload loaded;
    EXPECT_FALSE(DeserializeCooked(AssetKind::Mesh, bytes, loaded, error));
    EXPECT_NE(error.find("outside its section"), std::string::npos) << error;
}

struct ManifestScope {
    explicit ManifestScope(const fs::path& manifest) { AssetPathResolver::SetManifest(manifest); }
    ~ManifestScope() { AssetPathResolver::SetManifest({}); }
};

TEST_F(AssetCooking, ShaderLibraryKeysIgnoreDefineOrderAndUseLogicalNames) {
    const ShaderCompileOptions ab{.defines = {{"A", "1"}, {"B", "2"}}};
    const ShaderCompileOptions ba{.defines = {{"B", "2"}, {"A", "1"}}};
    const auto key = ShaderLibrary::Key(root / "shaders/x.hlsl", ShaderType::Pixel, ab);
    EXPECT_EQ(key, ShaderLibrary::Key(root / "shaders/x.hlsl", ShaderType::Pixel, ba));
    EXPECT_EQ(key, ShaderLibrary::Key("game/shaders/x.hlsl", ShaderType::Pixel, ab));
    EXPECT_NE(key, ShaderLibrary::Key(root / "shaders/x.hlsl", ShaderType::Vertex, ab));
    EXPECT_NE(key, ShaderLibrary::Key(root / "shaders/x.hlsl", ShaderType::Pixel, {.defines = {{"A", "1"}}}));
    EXPECT_NE(key, ShaderLibrary::Key(root / "shaders/x.hlsl", ShaderType::Pixel, {.entryPoint = "Other", .defines = ab.defines}));
    EXPECT_NE(key.find("game/shaders/x.hlsl"), std::string::npos) << key;
}

TEST_F(AssetCooking, ShaderLibraryRoundTripsAndRejectsDamage) {
    ShaderLibrary library;
    library.Add("one", {1, 2, 3});
    library.Add("two", {4});
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(library.Serialize(bytes, error)) << error;
    ShaderLibrary loaded;
    ASSERT_TRUE(loaded.Deserialize(bytes, error)) << error;
    ASSERT_EQ(loaded.Size(), 2u);
    ASSERT_NE(loaded.Find("one"), nullptr);
    EXPECT_EQ(*loaded.Find("one"), (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_EQ(loaded.Find("three"), nullptr);
    bytes.back() ^= 1;
    EXPECT_FALSE(loaded.Deserialize(bytes, error));
    EXPECT_EQ(loaded.Size(), 2u);
}

TEST_F(AssetCooking, CookedBuildReadsShadersFromTheLibraryOnly) {
    const auto file = root / "shaders" / "tiny.hlsl";
    fs::create_directories(file.parent_path());
    ASSERT_TRUE(WriteFileAtomic(file, "float4 VSMain(float4 p : POSITION) : SV_Position { return p * SCALE; }\n",
                                error)) << error;
    const ShaderCompileOptions options{.defines = {{"SCALE", "2"}}};
    const auto bytecode = ShaderContext::CompileForShipping(file, ShaderType::Vertex, options, error);
    ASSERT_TRUE(bytecode) << error;
    ShaderLibrary library;
    library.Add(ShaderLibrary::Key(file, ShaderType::Vertex, options), *bytecode);
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(library.Serialize(bytes, error)) << error;
    const auto cooked = root / "cooked";
    fs::create_directories(cooked);
    ASSERT_TRUE(WriteFileAtomic(cooked / ShaderLibraryName,
                                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), error));
    fs::remove(file); // a cooked build has no HLSL

    ManifestScope manifest(cooked / "manifest.json");
    const auto compiled = ShaderContext::CompileCpu(file, ShaderType::Vertex, options);
    ASSERT_TRUE(compiled.reflection);
    EXPECT_EQ(compiled.desc.InputParameters, 1u);
    EXPECT_EQ(compiled.blob->GetBufferSize(), bytecode->size());
    EXPECT_THROW((void)ShaderContext::CompileCpu(file, ShaderType::Vertex, {.defines = {{"SCALE", "3"}}}),
                 RenderingException);
}

TEST_F(AssetCooking, ManifestRestoresTheCatalogUnderNewRoots) {
    std::string manifest;
    ASSERT_TRUE(catalog.SerializeManifest(manifest, error)) << error;
    AssetCatalog loaded;
    const fs::path shipped = root / "shipped";
    ASSERT_TRUE(loaded.LoadManifest(manifest, shipped / "game", shipped / "engine", error)) << error;
    ASSERT_EQ(loaded.Size(), catalog.Size());
    for (const auto kind : {CatalogAssetKind::Mesh, CatalogAssetKind::Texture, CatalogAssetKind::Skeleton,
                            CatalogAssetKind::AnimationClip, CatalogAssetKind::SkinnedMesh}) {
        const auto expected = catalog.List(kind);
        const auto actual = loaded.List(kind);
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            EXPECT_EQ(actual[i]->path, expected[i]->path);
            EXPECT_EQ(actual[i]->metadata.id, expected[i]->metadata.id);
            EXPECT_EQ(actual[i]->metadata.centerOnLoad, expected[i]->metadata.centerOnLoad);
            EXPECT_EQ(actual[i]->metadata.materials, expected[i]->metadata.materials);
            EXPECT_EQ(actual[i]->source, shipped / "game" / fs::path(actual[i]->path.substr(5)));
        }
    }
}

TEST_F(AssetCooking, ManifestFromAnotherEngineVersionIsRejected) {
    std::string manifest;
    ASSERT_TRUE(catalog.SerializeManifest(manifest, error)) << error;
    const std::string current = "\"cookedFormat\": " + std::to_string(CookedFormatVersion);
    const auto at = manifest.find(current);
    ASSERT_NE(at, std::string::npos) << manifest.substr(0, 200);
    manifest.replace(at, current.size(), "\"cookedFormat\": 999");
    AssetCatalog loaded;
    EXPECT_FALSE(loaded.LoadManifest(manifest, root, root, error));
    EXPECT_NE(error.find("different engine version"), std::string::npos) << error;
}

} // namespace
