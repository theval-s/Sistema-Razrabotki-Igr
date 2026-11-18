#include <engine/core/Content.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/scripting/LuaRuntime.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "content_fixture.hpp"

namespace fs = std::filesystem;
using namespace engine;

namespace {

struct Content : testing::Test {
    fs::path base = fs::temp_directory_path() / ("sri-content-" + NewUuid());
    fs::path root = base / "game";
    std::shared_ptr<MemorySource> pack = std::make_shared<MemorySource>();
    std::string error;

    void SetUp() override { fs::create_directories(root); }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(base, ignored);
    }
    void Disk(const fs::path& file, std::string_view text) {
        fs::create_directories(file.parent_path());
        ASSERT_TRUE(WriteFileAtomic(file, text, error)) << error;
    }
    std::string Text(const fs::path& file) {
        std::string text;
        EXPECT_TRUE(ReadTextFile(file, text, error)) << error;
        return text;
    }
};

TEST_F(Content, MountShadowsDiskAndMissingFilesFallThrough) {
    Disk(root / "a.txt", "disk a");
    Disk(root / "loose.json", "loose");
    pack->Add("a.txt", "pack a");
    pack->Add("sub/b.txt", "pack b");
    {
        MountScope mount(root, pack);
        EXPECT_EQ(Text(root / "a.txt"), "pack a");
        EXPECT_EQ(Text(root / "sub/b.txt"), "pack b");
        EXPECT_EQ(Text(root / "loose.json"), "loose");
        EXPECT_TRUE(content::IsFile(root / "sub/b.txt"));
        EXPECT_TRUE(content::IsDirectory(root / "sub"));
        EXPECT_FALSE(content::IsFile(root / "sub"));
        EXPECT_FALSE(content::IsFile(root / "missing.txt"));
        std::string text;
        EXPECT_FALSE(ReadTextFile(root / "missing.txt", text, error));
    }
    EXPECT_EQ(Text(root / "a.txt"), "disk a");
    EXPECT_FALSE(content::IsFile(root / "sub/b.txt"));
}

TEST_F(Content, LookupsIgnoreCaseNormalizeDotsAndRespectTheRootBoundary) {
    pack->Add("Folder/File.txt", "packed");
    MountScope mount(root, pack);
    EXPECT_EQ(Text(root / "FOLDER/file.TXT"), "packed");
    EXPECT_EQ(Text(root / "other/../Folder/./File.txt"), "packed");
    EXPECT_TRUE(content::IsDirectory(root / "folder"));
    // A sibling whose name merely starts with the root's is not under it.
    EXPECT_FALSE(content::IsFile(base / "game2/Folder/File.txt"));
    EXPECT_FALSE(content::IsFile(root / ".." / "Folder/File.txt"));
}

TEST_F(Content, PackedReadsKeepTheSizeLimitAndDropTheTextBom) {
    pack->Add("bom.json", "\xEF\xBB\xBF{}");
    MountScope mount(root, pack);
    EXPECT_EQ(Text(root / "bom.json"), "{}");
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(ReadBinaryFile(root / "bom.json", bytes, error)) << error;
    EXPECT_EQ(bytes.size(), 5u);
    EXPECT_FALSE(ReadBinaryFile(root / "bom.json", bytes, error, 4));
    EXPECT_NE(error.find("bom.json"), std::string::npos) << error;
}

TEST_F(Content, DiskAndPackedStreamsSeekWithinTheFile) {
    Disk(base / "disk.bin", "0123456789");
    pack->Add("packed.bin", "0123456789");
    MountScope mount(root, pack);
    for (const auto& file : {base / "disk.bin", root / "packed.bin"}) {
        SCOPED_TRACE(file.string());
        auto stream = content::Open(file, error);
        ASSERT_TRUE(stream) << error;
        EXPECT_EQ(stream->Size(), 10u);
        char buffer[4]{};
        EXPECT_EQ(stream->Read(buffer, 3), 3u);
        EXPECT_EQ(std::string(buffer, 3), "012");
        EXPECT_EQ(stream->Tell(), 3u);
        ASSERT_TRUE(stream->Seek(-2, SEEK_END));
        EXPECT_EQ(stream->Read(buffer, 4), 2u);
        EXPECT_EQ(std::string(buffer, 2), "89");
        EXPECT_EQ(stream->Read(buffer, 4), 0u);
        EXPECT_FALSE(stream->Seek(1, SEEK_END));
        EXPECT_FALSE(stream->Seek(-1, SEEK_SET));
        ASSERT_TRUE(stream->Seek(4, SEEK_SET));
        ASSERT_TRUE(stream->Seek(-1, SEEK_CUR));
        EXPECT_EQ(stream->Read(buffer, 1), 1u);
        EXPECT_EQ(buffer[0], '3');
    }
    EXPECT_FALSE(content::Open(root / "missing.bin", error));
    EXPECT_NE(error.find("missing.bin"), std::string::npos) << error;
}

TEST_F(Content, LuaRequiresAndRunsScriptsFromAPackWithNoFolderOnDisk) {
    const fs::path packed = base / "packed-game"; // never created
    pack->Add("shared/settings.lua", "return {value = 42}");
    pack->Add("behavior.lua", "local settings = require('shared.settings')\nfunction OnStart() Seen = settings.value end");
    MountScope mount(packed, pack);
    LuaRuntime runtime;
    ASSERT_TRUE(runtime.Initialize(packed, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(require('shared.settings').value == 42)", "packed", error)) << error;
    InstanceId instance;
    ASSERT_TRUE(runtime.CreateInstance({packed / "behavior.lua", NewUuid(), NewUuid()}, instance, error)) << error;
    EXPECT_TRUE(runtime.StartInstance(instance, error)) << error;
    EXPECT_FALSE(runtime.ExecuteString("require('shared.missing')", "packed", error));
}

} // namespace
