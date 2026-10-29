#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetTable.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-asset-catalog-" + engine::NewUuid());
    Fixture() { std::filesystem::create_directories(root / "game/models"); }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

TEST(AssetCatalog, ReferencesAndMetadataValidateWithoutChangingOutput) {
    static_assert(sizeof(engine::AssetUuid) == 16);
    engine::AssetReference ref{engine::NewUuid(), "game/models/table/model.obj"};
    EXPECT_EQ(engine::AssetUuid(ref.id.ToString()), ref.id);
    EXPECT_FALSE(engine::AssetUuid::Parse("invalid"));
    std::string json, error;
    ASSERT_TRUE(engine::SerializeAssetReference(ref, json, error)) << error;
    EXPECT_NE(json.find(ref.id.ToString()), std::string::npos);
    engine::AssetReference read;
    ASSERT_TRUE(engine::ParseAssetReference(json, read, error)) << error;
    EXPECT_EQ(ref.id, read.id);
    EXPECT_EQ(ref.path, read.path);
    EXPECT_FALSE(engine::ParseAssetReference(R"({"id":"broken","path":"game/../bad"})", read, error));
    EXPECT_EQ(ref.id, read.id);
    EXPECT_FALSE(engine::ParseAssetReference(R"({"id":"one","id":"two","path":"game/a"})", read, error));
    engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Mesh, true};
    ASSERT_TRUE(engine::SerializeAssetMetadata(meta, json, error));
    engine::AssetMetadata loaded;
    ASSERT_TRUE(engine::ParseAssetMetadata(json, loaded, error));
    EXPECT_EQ(loaded.id, meta.id);
    EXPECT_TRUE(loaded.centerOnLoad);
}

TEST(AssetCatalog, RenamePreservesIdentityAndDuplicatesAreRejected) {
    Fixture fixture;
    const auto source = fixture.root / "game/models/table.obj";
    std::ofstream(source) << "model";
    std::size_t generated = 0;
    std::string error;
    ASSERT_TRUE(engine::GenerateMissingAssetMetadata(fixture.root / "game", generated, error)) << error;
    ASSERT_EQ(generated, 1u);
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(fixture.root / "game", {}, error)) << error;
    const auto* entry = catalog.FindPath("game/models/table.obj");
    ASSERT_NE(entry, nullptr);
    const auto uuid = entry->metadata.id;
    EXPECT_NE(catalog.Find(uuid, engine::CatalogAssetKind::Mesh), nullptr);
    EXPECT_EQ(catalog.Find(uuid, engine::CatalogAssetKind::Texture), nullptr);
    std::filesystem::rename(source, fixture.root / "game/models/renamed.obj");
    std::filesystem::rename(std::filesystem::path(source.string() + ".meta"), fixture.root / "game/models/renamed.obj.meta");
    ASSERT_TRUE(catalog.Scan(fixture.root / "game", {}, error)) << error;
    EXPECT_EQ(catalog.Find(uuid)->path, "game/models/renamed.obj");
    EXPECT_EQ(catalog.FindPath("game/models/table.obj"), nullptr);
    std::ofstream(fixture.root / "game/models/copy.obj") << "model";
    std::filesystem::copy_file(fixture.root / "game/models/renamed.obj.meta", fixture.root / "game/models/copy.obj.meta");
    EXPECT_FALSE(catalog.Scan(fixture.root / "game", {}, error));
    EXPECT_NE(error.find("duplicate asset UUID"), std::string::npos);
    EXPECT_EQ(catalog.Find(uuid)->path, "game/models/renamed.obj"); // failed scan is transactional
}

TEST(AssetCatalog, EvictedSlotsCannotResolveOldHandles) {
    engine::AssetTable<int> table;
    engine::AssetDesc first;
    first.name = "game/first";
    const auto handle = table.Acquire(std::move(first));
    table.Publish(handle, 42);
    table.EvictAll();
    EXPECT_EQ(table.Get(handle), nullptr);
    engine::AssetDesc next;
    next.name = "game/second";
    const auto newHandle = table.Acquire(std::move(next));
    EXPECT_NE(newHandle, handle);
    EXPECT_EQ(table.Get(handle), nullptr);
}

TEST(AssetCatalog, MissingMetadataDoesNotHideHealthyAssets) {
    Fixture fixture;
    std::ofstream(fixture.root / "game/models/healthy.obj") << "mesh";
    std::size_t generated = 0;
    std::string error;
    ASSERT_TRUE(engine::GenerateMissingAssetMetadata(fixture.root / "game", generated, error));
    std::ofstream(fixture.root / "game/models/missing.obj") << "mesh";
    engine::AssetCatalog catalog;
    EXPECT_FALSE(catalog.Scan(fixture.root / "game", {}, error));
    EXPECT_NE(error.find("missing metadata"), std::string::npos);
    EXPECT_NE(catalog.FindPath("game/models/healthy.obj"), nullptr);
    EXPECT_EQ(catalog.FindPath("game/models/missing.obj"), nullptr);
}
}
