#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/assets/ModelImport.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "gltf_fixture.hpp"

namespace {
using namespace engine;
namespace fs = std::filesystem;

const std::string Png = std::string("\x89PNG\r\n\x1a\n", 8) + "never decoded here";

std::string BaseColor(int texture, int uvSet = 0) {
    return std::format(R"("pbrMetallicRoughness":{{"baseColorTexture":{{"index":{},"texCoord":{}}}}})", texture, uvSet);
}

class ModelImportTest : public ::testing::Test {
protected:
    fs::path root = fs::temp_directory_path() / ("sri-model-import-" + NewUuid());
    fs::path content = root / "content";      // the game root
    fs::path downloads = root / "downloads";  // where models come from
    fs::path engineRoot = root / "engine";
    fs::path previousRoot = AssetPathResolver::GetGameRoot();
    AssetReference base{NewUuid(), "engine/primitives/default.material.json"};
    std::vector<std::string> warnings;
    std::string error;

    void SetUp() override {
        fs::create_directories(content);
        fs::create_directories(downloads);
        fs::create_directories(engineRoot);
        AssetPathResolver::SetGameRoot(content);
    }
    void TearDown() override {
        AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    /// Two slots: Gold samples an embedded image through UV set 1, Gem has no texture.
    static test::GltfFixture Token() {
        test::GltfFixture model;
        const int gold = model.AddImageBytes("Gold Color", "image/png", Png);
        model.AddMaterial("Gold", BaseColor(gold, 1));
        model.AddMaterial("Gem");
        model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 1}}, 0});
        model.AddPrimitive({{0, 0, 1, 1, 0, 1, 0, 1, 1}, {{0, 0, 1, 0, 0, 1}}, 1});
        return model;
    }
    static std::string Bytes(const fs::path& file) {
        std::ifstream input(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    static void WriteBytes(const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    }
    AssetMetadata Meta(const fs::path& file) {
        AssetMetadata meta;
        EXPECT_TRUE(ReadAssetMetadata(file, meta, error)) << error;
        return meta;
    }
    MaterialInstanceDefinition Instance(const fs::path& file) {
        MaterialInstanceDefinition definition;
        std::string text;
        EXPECT_TRUE(ReadTextFile(file, text, error)) << error;
        EXPECT_TRUE(ParseMaterialInstanceDefinition(text, definition, error)) << error;
        return definition;
    }
    std::vector<std::string> Listing(const fs::path& folder) {
        std::vector<std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            files.push_back(ToGenericUtf8(entry.path().lexically_relative(folder)));
        }
        std::ranges::sort(files);
        return files;
    }
    void ExpectCleanCatalog() {
        AssetCatalog catalog;
        EXPECT_TRUE(catalog.Scan(content, engineRoot, error)) << error;
    }
};

TEST_F(ModelImportTest, InspectionListsSlotsTheirTexturesAndUvSets) {
    Token().WriteGlb(downloads / "token.glb");
    ModelInfo info;
    ASSERT_TRUE(InspectModel(downloads / "token.glb", info, error)) << error;
    ASSERT_EQ(info.materialSlots.size(), 2u);
    EXPECT_EQ(info.materialSlots[0].name, "Gold");
    EXPECT_EQ(info.materialSlots[0].texture, 0u);
    EXPECT_EQ(info.materialSlots[0].uvChannel, 1u);
    EXPECT_EQ(info.materialSlots[1].name, "Gem");
    EXPECT_FALSE(info.materialSlots[1].texture);
    ASSERT_EQ(info.textures.size(), 1u);
    EXPECT_EQ(info.textures[0].name, "Gold Color.png");
    EXPECT_EQ(std::string(info.textures[0].bytes.begin(), info.textures[0].bytes.end()), Png);
    EXPECT_TRUE(info.textures[0].file.empty());
    EXPECT_TRUE(info.textures[0].problem.empty());
    EXPECT_EQ(info.vertices, 6u);
    EXPECT_EQ(info.triangles, 2u);
    EXPECT_TRUE(info.companions.empty());
}

TEST_F(ModelImportTest, ImportWritesTexturesMaterialsAndTheirSlotsInTheMeta) {
    Token().WriteGlb(downloads / "token.glb");
    const fs::path folder = content / "props" / "Token";
    ASSERT_TRUE(ImportModel(downloads / "token.glb", folder, true, {true, base, {}}, warnings, error)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(Listing(folder), (std::vector<std::string>{
        "materials", "materials/Gem.material-instance.json", "materials/Gem.material-instance.json.meta",
        "materials/Gold.material-instance.json", "materials/Gold.material-instance.json.meta", "model.glb",
        "model.glb.meta", "textures", "textures/Gold Color.png", "textures/Gold Color.png.meta"}));
    EXPECT_EQ(Bytes(folder / "textures/Gold Color.png"), Png);

    const AssetMetadata mesh = Meta(folder / "model.glb");
    EXPECT_EQ(mesh.kind, CatalogAssetKind::Mesh);
    EXPECT_TRUE(mesh.centerOnLoad);
    ASSERT_EQ(mesh.materials.size(), 2u);
    const AssetReference gold{Meta(folder / "materials/Gold.material-instance.json").id,
                              "game/props/Token/materials/Gold.material-instance.json"};
    EXPECT_EQ(mesh.materials[0], gold);
    EXPECT_EQ(mesh.materials[1]->path, "game/props/Token/materials/Gem.material-instance.json");

    const auto goldInstance = Instance(folder / "materials/Gold.material-instance.json");
    EXPECT_EQ(goldInstance.materialReference, base);
    const AssetReference texture{Meta(folder / "textures/Gold Color.png").id, "game/props/Token/textures/Gold Color.png"};
    EXPECT_EQ(goldInstance.parameters.albedoReference, texture);
    EXPECT_FALSE(Instance(folder / "materials/Gem.material-instance.json").parameters.albedoTexture);
    ExpectCleanCatalog();
}

TEST_F(ModelImportTest, ExtractingAgainReusesWhatIsThere) {
    Token().WriteGlb(downloads / "token.glb");
    const fs::path folder = content / "Token";
    ASSERT_TRUE(ImportModel(downloads / "token.glb", folder, false, {true, base, {}}, warnings, error)) << error;
    const auto listing = Listing(folder);
    const auto meta = Bytes(AssetMetadataPath(folder / "model.glb"));
    ASSERT_TRUE(ExtractModelMaterials(folder / "model.glb", {true, base, {}}, warnings, error)) << error;
    EXPECT_EQ(Listing(folder), listing);
    EXPECT_EQ(Bytes(AssetMetadataPath(folder / "model.glb")), meta);
}

TEST_F(ModelImportTest, GltfBringsItsFilesAndUsesItsImagesInPlace) {
    test::GltfFixture model;
    const int gold = model.AddImageUri("maps/gold%20leaf.png");
    model.AddMaterial("Gold", BaseColor(gold));
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 0, 1, 0, 0, 1}}, 0});
    model.WriteGltf(downloads / "token.gltf", "token-data.bin");
    WriteBytes(downloads / "maps/gold leaf.png", Png);

    ModelInfo info;
    ASSERT_TRUE(InspectModel(downloads / "token.gltf", info, error)) << error;
    EXPECT_EQ(info.companions, (std::vector<fs::path>{downloads / "token-data.bin", downloads / "maps/gold leaf.png"}));

    const fs::path folder = content / "Token";
    ASSERT_TRUE(ImportModel(downloads / "token.gltf", folder, false, {true, base, {}}, warnings, error)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(Listing(folder), (std::vector<std::string>{
        "maps", "maps/gold leaf.png", "maps/gold leaf.png.meta", "materials", "materials/Gold.material-instance.json",
        "materials/Gold.material-instance.json.meta", "model.gltf", "model.gltf.meta", "token-data.bin"}));
    const AssetReference texture{Meta(folder / "maps/gold leaf.png").id, "game/Token/maps/gold leaf.png"};
    EXPECT_EQ(Instance(folder / "materials/Gold.material-instance.json").parameters.albedoReference, texture);
    ExpectCleanCatalog();
}

TEST_F(ModelImportTest, ObjBringsItsMtlAndTheTexturesItNames) {
    WriteBytes(downloads / "token.obj", "mtllib token.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n"
                                        "usemtl Gold\nf 1/1 2/2 3/3\n");
    WriteBytes(downloads / "token.mtl", "newmtl Gold\nmap_Kd gold.png\n");
    WriteBytes(downloads / "gold.png", Png);
    const fs::path folder = content / "Token";
    ASSERT_TRUE(ImportModel(downloads / "token.obj", folder, false, {true, base, {}}, warnings, error)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(Listing(folder), (std::vector<std::string>{
        "materials", "materials/Gold.material-instance.json", "materials/Gold.material-instance.json.meta",
        "model.obj", "model.obj.meta", "textures", "textures/gold.png", "textures/gold.png.meta", "token.mtl"}));
    EXPECT_EQ(Instance(folder / "materials/Gold.material-instance.json").parameters.albedoTexture,
              "game/Token/textures/gold.png");
}

TEST_F(ModelImportTest, GltfLoadingABufferFromOutsideItsFolderIsRefused) {
    test::GltfFixture model;
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {}, -1});
    model.WriteGltf(downloads / "token.gltf", "../token.bin");
    const fs::path folder = content / "Token";
    EXPECT_FALSE(ImportModel(downloads / "token.gltf", folder, false, {true, base, {}}, warnings, error));
    EXPECT_NE(error.find("outside"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(folder));
}

TEST_F(ModelImportTest, LooseModelExtractsBesideItAndMissingTexturesWarn) {
    // The Explorer case: a model copied into the project as it was.
    test::GltfFixture model;
    const int gold = model.AddImageUri("missing.png");
    model.AddMaterial("Gold", BaseColor(gold));
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 0, 1, 0, 0, 1}}, 0});
    const fs::path file = content / "loose" / "statue.gltf";
    fs::create_directories(file.parent_path());
    model.WriteGltf(file, "statue.bin");
    ASSERT_TRUE(WriteAssetMetadata(file, {NewUuid(), CatalogAssetKind::Mesh}, error)) << error;

    EXPECT_EQ(ModelMaterialsFolder(file), content / "loose" / "statue");
    EXPECT_EQ(ModelMaterialsFolder(content / "Token" / "Model.glb"), content / "Token");
    ASSERT_TRUE(ExtractModelMaterials(file, {true, base, {}}, warnings, error)) << error;
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("missing.png"), std::string::npos) << warnings[0];
    const fs::path instance = content / "loose/statue/materials/Gold.material-instance.json";
    EXPECT_FALSE(Instance(instance).parameters.albedoTexture);
    EXPECT_EQ(Meta(file).materials.at(0)->id, Meta(instance).id);
}

TEST_F(ModelImportTest, FallbackTextureGoesToSlotsWithoutOne) {
    Token().WriteGlb(downloads / "token.glb");
    WriteBytes(downloads / "paint.jpg", "jpeg bytes");
    const fs::path folder = content / "Token";
    ASSERT_TRUE(ImportModel(downloads / "token.glb", folder, false, {true, base, downloads / "paint.jpg"}, warnings,
                            error)) << error;
    EXPECT_EQ(Bytes(folder / "textures/paint.jpg"), "jpeg bytes");
    EXPECT_EQ(Instance(folder / "materials/Gem.material-instance.json").parameters.albedoTexture,
              "game/Token/textures/paint.jpg");
    EXPECT_EQ(Instance(folder / "materials/Gold.material-instance.json").parameters.albedoTexture,
              "game/Token/textures/Gold Color.png");
}

TEST_F(ModelImportTest, SlotNamesBecomeSafeDistinctFileNames) {
    test::GltfFixture model;
    for (const char* name : {"a:b", "A:B", "con", " trailing. "}) model.AddMaterial(name);
    for (int material = 0; material < 4; ++material) {
        const float z = static_cast<float>(material);
        model.AddPrimitive({{0, 0, z, 1, 0, z, 0, 1, z}, {{0, 0, 1, 0, 0, 1}}, material});
    }
    model.WriteGlb(downloads / "names.glb");
    const fs::path folder = content / "Names";
    ASSERT_TRUE(ImportModel(downloads / "names.glb", folder, false, {true, base, {}}, warnings, error)) << error;
    for (const char* file : {"a_b", "A_B (2)", "con_", "trailing"}) {
        EXPECT_TRUE(fs::exists(folder / "materials" / (std::string(file) + ".material-instance.json"))) << file;
    }
    ExpectCleanCatalog();
}

TEST_F(ModelImportTest, FailedImportLeavesNothingBehind) {
    Token().WriteGlb(downloads / "token.glb");
    WriteBytes(downloads / "notes.txt", "not an image");
    const fs::path folder = content / "Token";
    EXPECT_FALSE(ImportModel(downloads / "token.glb", folder, false, {true, base, downloads / "notes.txt"}, warnings,
                             error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(folder));
    EXPECT_FALSE(ImportModel(downloads / "absent.glb", folder, false, {true, base, {}}, warnings, error));
    EXPECT_FALSE(fs::exists(folder));
}
} // namespace
