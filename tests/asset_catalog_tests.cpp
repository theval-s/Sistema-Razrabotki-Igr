#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetTable.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

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

TEST(AssetCatalog, MeshMetadataKeepsDefaultMaterialPerSlot) {
    const engine::AssetReference gold{engine::NewUuid(), "game/models/token/materials/Gold.material-instance.json"};
    engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Mesh, false, {gold, std::nullopt}};
    std::string json, error;
    ASSERT_TRUE(engine::SerializeAssetMetadata(meta, json, error)) << error;
    engine::AssetMetadata loaded;
    ASSERT_TRUE(engine::ParseAssetMetadata(json, loaded, error)) << error;
    ASSERT_EQ(loaded.materials.size(), 2u);
    EXPECT_EQ(loaded.materials[0], gold);
    EXPECT_FALSE(loaded.materials[1]);

    // Metas from before material slots keep their exact text.
    meta.materials.clear();
    ASSERT_TRUE(engine::SerializeAssetMetadata(meta, json, error)) << error;
    EXPECT_EQ(json.find("materials"), std::string::npos);

    const std::string id = engine::NewUuid();
    for (const std::string invalid : {
             R"({"version":1,"id":")" + id + R"(","kind":"texture","materials":[]})",
             R"({"version":1,"id":")" + id + R"(","kind":"mesh","materials":{}})",
             R"({"version":1,"id":")" + id + R"(","kind":"mesh","materials":["game/a.material-instance.json"]})",
             R"({"version":1,"id":")" + id + R"(","kind":"mesh","materials":[{"id":"broken","path":"game/a"}]})"}) {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(engine::ParseAssetMetadata(invalid, loaded, error));
    }
}

TEST(AssetCatalog, UiAssetsGenerateMetadataAndRoundTripThroughCatalog) {
    Fixture fixture;
    using engine::CatalogAssetKind;
    struct Example { const char* name; CatalogAssetKind kind; const char* codec; };
    const Example examples[] = {
        {"menu.RML", CatalogAssetKind::UiDocument, "ui_document"},
        {"theme.rcss", CatalogAssetKind::UiStylesheet, "ui_stylesheet"},
        {"item.rmlt", CatalogAssetKind::UiTemplate, "ui_template"},
        {"body.ttf", CatalogAssetKind::Font, "font"},
        {"title.OTF", CatalogAssetKind::Font, "font"},
    };
    for (const auto& example : examples) {
        const auto file = fixture.root / "game/models" / example.name;
        std::ofstream(file) << "content";
        EXPECT_EQ(engine::CatalogKindForFile(file), example.kind);
    }
    std::size_t generated = 0;
    std::string error;
    ASSERT_TRUE(engine::GenerateMissingAssetMetadata(fixture.root / "game", generated, error)) << error;
    EXPECT_EQ(generated, std::size(examples));
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(fixture.root / "game", {}, error)) << error;
    for (const auto& example : examples) {
        const auto* entry = catalog.FindPath(std::string("game/models/") + example.name);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->metadata.kind, example.kind);
        EXPECT_NE(catalog.Find(entry->metadata.id, example.kind), nullptr);
        std::string json;
        ASSERT_TRUE(engine::SerializeAssetMetadata(entry->metadata, json, error)) << error;
        EXPECT_NE(json.find(example.codec), std::string::npos);
        engine::AssetMetadata parsed;
        ASSERT_TRUE(engine::ParseAssetMetadata(json, parsed, error)) << error;
        EXPECT_EQ(parsed.id, entry->metadata.id);
        EXPECT_EQ(parsed.kind, example.kind);
    }
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

TEST(AssetCatalog, AudioClipsGenerateMetadataResolveByUuidAndPreserveIdentityOnRename) {
    Fixture fixture;
    using engine::CatalogAssetKind;
    const char* names[] = {"effect.WAV", "ambience.flac", "music.Mp3"};
    for (const auto* name : names) {
        const auto file = fixture.root / "game/models" / name;
        std::ofstream(file) << "catalog fixture; decoding is not required";
        EXPECT_EQ(engine::CatalogKindForFile(file), CatalogAssetKind::AudioClip);
    }
    EXPECT_FALSE(engine::CatalogKindForFile("unsupported.ogg"));
    std::size_t generated = 0;
    std::string error;
    ASSERT_TRUE(engine::GenerateMissingAssetMetadata(fixture.root / "game", generated, error)) << error;
    EXPECT_EQ(generated, 3u);
    ASSERT_TRUE(engine::GenerateMissingAssetMetadata(fixture.root / "game", generated, error)) << error;
    EXPECT_EQ(generated, 0u);
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(fixture.root / "game", {}, error)) << error;
    EXPECT_EQ(catalog.List(CatalogAssetKind::AudioClip).size(), 3u);
    for (const auto* name : names) {
        const auto* entry = catalog.FindPath(std::string("game/models/") + name);
        ASSERT_NE(entry, nullptr);
        EXPECT_NE(catalog.Find(entry->metadata.id, CatalogAssetKind::AudioClip), nullptr);
        EXPECT_EQ(catalog.Find(entry->metadata.id, CatalogAssetKind::Texture), nullptr);
        std::string json;
        ASSERT_TRUE(engine::SerializeAssetMetadata(entry->metadata, json, error)) << error;
        EXPECT_NE(json.find("\"audioClip\""), std::string::npos);
        engine::AssetMetadata parsed;
        ASSERT_TRUE(engine::ParseAssetMetadata(json, parsed, error)) << error;
        EXPECT_EQ(parsed.id, entry->metadata.id);
        EXPECT_EQ(parsed.kind, CatalogAssetKind::AudioClip);
        json.insert(json.find('{') + 1, "\"centerOnLoad\": false,");
        EXPECT_FALSE(engine::ParseAssetMetadata(json, parsed, error));
        EXPECT_EQ(parsed.kind, CatalogAssetKind::AudioClip);
    }
    const auto id = catalog.FindPath("game/models/effect.WAV")->metadata.id;
    const auto before = fixture.root / "game/models/effect.WAV";
    const auto after = fixture.root / "game/models/renamed.wav";
    std::filesystem::rename(before, after);
    std::filesystem::rename(engine::AssetMetadataPath(before), engine::AssetMetadataPath(after));
    ASSERT_TRUE(catalog.Scan(fixture.root / "game", {}, error)) << error;
    ASSERT_NE(catalog.Find(id, CatalogAssetKind::AudioClip), nullptr);
    EXPECT_EQ(catalog.Find(id)->path, "game/models/renamed.wav");
    EXPECT_EQ(catalog.FindPath("game/models/effect.WAV"), nullptr);
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

TEST(AssetCatalog, ListReturnsOneKindSortedByPath) {
    Fixture fixture;
    std::ofstream(fixture.root / "game/models/b.png") << "image";
    std::ofstream(fixture.root / "game/models/a.png") << "image";
    std::ofstream(fixture.root / "game/models/table.obj") << "mesh";
    std::size_t generated = 0;
    std::string error;
    ASSERT_TRUE(engine::GenerateMissingAssetMetadata(fixture.root / "game", generated, error)) << error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(fixture.root / "game", {}, error)) << error;
    const auto textures = catalog.List(engine::CatalogAssetKind::Texture);
    ASSERT_EQ(textures.size(), 2u);
    EXPECT_EQ(textures[0]->path, "game/models/a.png");
    EXPECT_EQ(textures[1]->path, "game/models/b.png");
    EXPECT_TRUE(catalog.List(engine::CatalogAssetKind::Scene).empty());
}
}
