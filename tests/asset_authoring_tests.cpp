#include <engine/assets/AssetAuthoring.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-asset-authoring-" + engine::NewUuid());
    std::filesystem::path game = root / "game";
    Fixture() { std::filesystem::create_directories(game / "models"); }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    /// A source file with a fresh sidecar.
    engine::AssetUuid Add(const std::filesystem::path& file, std::string_view text = "x") const {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
        engine::AssetMetadata meta{engine::NewUuid(), *engine::CatalogKindForFile(file)};
        std::string error;
        EXPECT_TRUE(engine::WriteAssetMetadata(file, meta, error)) << error;
        return meta.id;
    }

    [[nodiscard]] bool Scans(engine::AssetCatalog& catalog) const {
        std::string error;
        const bool clean = catalog.Scan(game, {}, error);
        EXPECT_TRUE(clean) << error;
        return clean;
    }
};

engine::AssetUuid IdOf(const std::filesystem::path& source) {
    engine::AssetMetadata meta;
    std::string error;
    EXPECT_TRUE(engine::ReadAssetMetadata(source, meta, error)) << error;
    return meta.id;
}

TEST(AssetAuthoring, KindDetectionUsesDoubleExtensions) {
    using engine::CatalogAssetKind;
    EXPECT_EQ(engine::CatalogKindForFile("a/stone.material-instance.json"), CatalogAssetKind::MaterialInstance);
    EXPECT_EQ(engine::CatalogKindForFile("a/stone.material.json"), CatalogAssetKind::Material);
    EXPECT_EQ(engine::CatalogKindForFile("a/start.scene.json"), CatalogAssetKind::Scene);
    EXPECT_EQ(engine::CatalogKindForFile("a/Model.FBX"), CatalogAssetKind::Mesh);
    EXPECT_EQ(engine::CatalogKindForFile("a/texture.png"), CatalogAssetKind::Texture);
    EXPECT_FALSE(engine::CatalogKindForFile("a/notes.txt"));
    EXPECT_FALSE(engine::CatalogKindForFile("a/plain.json"));
}

TEST(AssetAuthoring, WriteMetadataRefusesOverwriteAndWrongKind) {
    Fixture fixture;
    const auto texture = fixture.game / "texture.png";
    const auto id = fixture.Add(texture);
    std::string error;
    EXPECT_FALSE(engine::WriteAssetMetadata(texture, {engine::NewUuid(), engine::CatalogAssetKind::Texture}, error));
    EXPECT_EQ(IdOf(texture), id);
    const auto mesh = fixture.game / "model.obj";
    std::ofstream(mesh) << "o empty";
    EXPECT_FALSE(engine::WriteAssetMetadata(mesh, {engine::NewUuid(), engine::CatalogAssetKind::Texture}, error));
    EXPECT_FALSE(std::filesystem::exists(engine::AssetMetadataPath(mesh)));
}

TEST(AssetAuthoring, MoveKeepsIdentityAndCatalogValid) {
    Fixture fixture;
    const auto id = fixture.Add(fixture.game / "models/table/model.obj");
    std::string error;
    ASSERT_TRUE(engine::MoveAsset(fixture.game / "models/table/model.obj", fixture.game / "models/table/desk.obj", error))
        << error;
    ASSERT_TRUE(engine::MoveAsset(fixture.game / "models/table", fixture.game / "furniture", error)) << error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(fixture.Scans(catalog));
    const auto* entry = catalog.Find(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->path, "game/furniture/desk.obj");
}

TEST(AssetAuthoring, CaseOnlyRenameIsAllowed) {
    Fixture fixture;
    const auto id = fixture.Add(fixture.game / "stone.png");
    std::string error;
    ASSERT_TRUE(engine::MoveAsset(fixture.game / "stone.png", fixture.game / "Stone.png", error)) << error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(fixture.Scans(catalog));
    ASSERT_NE(catalog.Find(id), nullptr);
    EXPECT_EQ(catalog.Find(id)->path, "game/Stone.png");
}

TEST(AssetAuthoring, MoveFailsWithoutChanges) {
    Fixture fixture;
    const auto a = fixture.game / "a.png";
    const auto b = fixture.game / "b.png";
    const auto id = fixture.Add(a, "a");
    fixture.Add(b, "b");
    std::string error;
    EXPECT_FALSE(engine::MoveAsset(a, b, error));
    EXPECT_FALSE(engine::MoveAsset(a, fixture.game / "a.hlsl", error));  // kind change
    EXPECT_FALSE(engine::MoveAsset(fixture.game / "models", fixture.game / "models/inner", error));
    // A stray sidecar at the destination must not be adopted.
    const auto loose = fixture.game / "loose.png";
    std::ofstream(loose) << "x";
    std::ofstream(engine::AssetMetadataPath(fixture.game / "taken.png")) << "{}";
    EXPECT_FALSE(engine::MoveAsset(loose, fixture.game / "taken.png", error));
    EXPECT_TRUE(std::filesystem::exists(loose));
    EXPECT_EQ(IdOf(a), id);
    EXPECT_TRUE(std::filesystem::exists(fixture.game / "models"));
}

TEST(AssetAuthoring, CopyGivesNewIdentitiesAndKeepsImportSettings) {
    Fixture fixture;
    const auto mesh = fixture.game / "models/table/model.obj";
    fixture.Add(mesh);
    engine::AssetMetadata centered{engine::NewUuid(), engine::CatalogAssetKind::Mesh, true};
    std::filesystem::remove(engine::AssetMetadataPath(mesh));
    std::string error;
    ASSERT_TRUE(engine::WriteAssetMetadata(mesh, centered, error)) << error;
    fixture.Add(fixture.game / "models/table/texture.png");
    std::ofstream(fixture.game / "models/table/readme.txt") << "not an asset";

    ASSERT_TRUE(engine::CopyAsset(mesh, fixture.game / "models/table/copy.obj", error)) << error;
    ASSERT_TRUE(engine::CopyAsset(fixture.game / "models/table", fixture.game / "models/table2", error)) << error;
    EXPECT_TRUE(std::filesystem::exists(fixture.game / "models/table2/readme.txt"));

    engine::AssetCatalog catalog;
    ASSERT_TRUE(fixture.Scans(catalog));  // duplicate UUIDs would fail the scan
    EXPECT_EQ(catalog.Size(), 6u);  // model, texture and copy, twice
    const auto copy = IdOf(fixture.game / "models/table/copy.obj");
    EXPECT_NE(copy, centered.id);
    EXPECT_TRUE(catalog.Find(copy)->metadata.centerOnLoad);
    EXPECT_TRUE(catalog.FindPath("game/models/table2/model.obj")->metadata.centerOnLoad);

    EXPECT_FALSE(engine::CopyAsset(mesh, fixture.game / "models/table/copy.obj", error));
    EXPECT_FALSE(engine::CopyAsset(fixture.game / "models", fixture.game / "models/nested", error));
    EXPECT_FALSE(std::filesystem::exists(fixture.game / "models/nested"));
}

TEST(AssetAuthoring, FindReferencesSearchesJsonOnly) {
    Fixture fixture;
    const auto texture = fixture.Add(fixture.game / "stone.png");
    const auto instance = fixture.game / "materials/stone.material-instance.json";
    fixture.Add(instance, R"({"parameters":{"albedoTexture":{"id":")" + texture.ToString() + R"(","path":"game/stone.png"}}})");
    std::ofstream(fixture.game / "notes.txt") << texture.ToString();

    const auto found = engine::FindAssetReferences(fixture.game, texture);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(std::filesystem::equivalent(found.front(), instance));
    EXPECT_TRUE(engine::FindAssetReferences(fixture.game, IdOf(instance)).empty());
}

TEST(AssetAuthoring, RescanPicksUpNewAssets) {
    Fixture fixture;
    engine::AssetPathResolver::SetGameRoot(fixture.game);
    engine::Engine engine;
    std::string error;
    // Not initialized: nothing is in flight, so the rescan applies at once.
    EXPECT_TRUE(engine.RescanAssetCatalogs(error)) << error;
    const auto id = fixture.Add(fixture.game / "fresh.png");
    EXPECT_EQ(engine.GetSceneAssets().GetCatalog().Find(id), nullptr);
    EXPECT_TRUE(engine.RescanAssetCatalogs(error)) << error;
    EXPECT_NE(engine.GetSceneAssets().GetCatalog().Find(id), nullptr);
    EXPECT_NE(engine.GetAssets().GetCatalog().Find(id), nullptr);

    // An orphaned sidecar fails the scan but keeps what was there.
    std::filesystem::remove(fixture.game / "fresh.png");
    EXPECT_FALSE(engine.RescanAssetCatalogs(error));
    EXPECT_NE(error.find("orphaned"), std::string::npos);
    EXPECT_EQ(engine.GetSceneAssets().GetCatalogError(), error);
    EXPECT_NE(engine.GetSceneAssets().GetCatalog().Find(id), nullptr);
    engine::AssetPathResolver::SetGameRoot({});
}

}  // namespace
