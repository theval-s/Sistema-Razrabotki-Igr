#include <engine/assets/AssetCatalog.hpp>
#include <engine/core/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-file-system-" + engine::NewUuid());
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

TEST(FileSystem, TextRoundTripStripsBomAndHandlesUnicodePaths) {
    Fixture fixture;
    const auto file = fixture.root / std::filesystem::path(u8"сцена.scene.json");
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(file, "\xEF\xBB\xBF{}\n", error)) << error;
    std::string text = "unchanged";
    ASSERT_TRUE(engine::ReadTextFile(file, text, error)) << error;
    EXPECT_EQ(text, "{}\n");
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(engine::ReadBinaryFile(file, bytes, error)) << error;
    EXPECT_EQ(bytes.size(), 6u);  // binary reads keep the BOM
}

TEST(FileSystem, ReadFailuresLeaveOutputUnchanged) {
    Fixture fixture;
    std::string text = "unchanged", error;
    EXPECT_FALSE(engine::ReadTextFile(fixture.root / "missing.json", text, error));
    EXPECT_NE(error.find("missing.json"), std::string::npos);
    EXPECT_FALSE(engine::ReadTextFile(fixture.root, text, error));  // a directory
    ASSERT_TRUE(engine::CreateNewFile(fixture.root / "big.json", "0123456789", error)) << error;
    EXPECT_FALSE(engine::ReadTextFile(fixture.root / "big.json", text, error, 9));
    EXPECT_EQ(text, "unchanged");
}

TEST(FileSystem, CreateNewFileNeverReplaces) {
    Fixture fixture;
    const auto file = fixture.root / "a.txt";
    std::string error, text;
    ASSERT_TRUE(engine::CreateNewFile(file, "first", error)) << error;
    EXPECT_FALSE(engine::CreateNewFile(file, "second", error));
    EXPECT_NE(error.find("already exists"), std::string::npos);
    ASSERT_TRUE(engine::ReadTextFile(file, text, error));
    EXPECT_EQ(text, "first");
    EXPECT_FALSE(engine::CreateNewFile(fixture.root / "no/such/dir/a.txt", "x", error));
}

TEST(FileSystem, AtomicWriteCreatesAndReplacesWithoutLeftovers) {
    Fixture fixture;
    const auto file = fixture.root / "doc.json";
    std::string error, text;
    ASSERT_TRUE(engine::WriteFileAtomic(file, "one", error)) << error;
    ASSERT_TRUE(engine::WriteFileAtomic(file, "two", error)) << error;
    ASSERT_TRUE(engine::ReadTextFile(file, text, error));
    EXPECT_EQ(text, "two");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(fixture.root), {}), 1);
    EXPECT_FALSE(engine::WriteFileAtomic(fixture.root / "no/such/dir/doc.json", "x", error));
}

}  // namespace
