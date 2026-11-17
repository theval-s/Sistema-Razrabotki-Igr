#include <engine/core/Content.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Pak.hpp>
#include <engine/core/Uuid.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "content_fixture.hpp"

namespace fs = std::filesystem;
using namespace engine;

namespace {

std::vector<std::uint8_t> Bytes(std::string_view text) { return {text.begin(), text.end()}; }

std::vector<std::uint8_t> Repetitive(std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>("pack me "[i % 8]);
    return bytes;
}

std::vector<std::uint8_t> Noise(std::size_t size, std::string_view marker = {}) {
    std::mt19937 random(7);
    std::vector<std::uint8_t> bytes(marker.begin(), marker.end());
    while (bytes.size() < size) bytes.push_back(static_cast<std::uint8_t>(random()));
    return bytes;
}

struct Pak : testing::Test {
    fs::path root = fs::temp_directory_path() / ("sri-pak-" + NewUuid());
    fs::path file = root / "game.sripak";
    std::string error;

    void SetUp() override { fs::create_directories(root); }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    std::vector<std::uint8_t> Contents(const PakSource& pak, std::string_view path) {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(pak.Read(path, bytes, error, std::numeric_limits<std::uint64_t>::max())) << path << ": " << error;
        return bytes;
    }
    std::vector<std::uint8_t> FileBytes(const fs::path& path) {
        std::vector<std::uint8_t> bytes;
        EXPECT_TRUE(ReadBinaryFile(path, bytes, error)) << error;
        return bytes;
    }
    void Overwrite(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
        ASSERT_TRUE(WriteFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), error))
            << error;
    }
};

TEST_F(Pak, RoundTripsStoredAndDeflatedFilesWithFolders) {
    ASSERT_TRUE(WriteFileAtomic(root / "source.bin", "from disk", error)) << error;
    PakWriter writer;
    writer.AddBytes("game/scripts/main.lua", Repetitive(10000), true);
    writer.AddBytes("game/sounds/tone.wav", Repetitive(10000), false);
    writer.AddBytes("manifest.json", Bytes("{}"), true);
    writer.AddBytes("empty.txt", {}, true);
    writer.AddFile("game/Data/Source.bin", root / "source.bin", true);
    ASSERT_TRUE(writer.Write(file, error)) << error;
    EXPECT_LT(fs::file_size(file), 15000u); // main.lua was deflated, tone.wav was not

    const auto pak = PakSource::Load(file, error);
    ASSERT_TRUE(pak) << error;
    EXPECT_EQ(pak->Files(), (std::vector<std::string>{"empty.txt", "game/Data/Source.bin", "game/scripts/main.lua",
                                                       "game/sounds/tone.wav", "manifest.json"}));
    EXPECT_EQ(Contents(*pak, "game/scripts/main.lua"), Repetitive(10000));
    EXPECT_EQ(Contents(*pak, "game/sounds/tone.wav"), Repetitive(10000));
    EXPECT_EQ(Contents(*pak, "GAME/data/source.BIN"), Bytes("from disk"));
    EXPECT_EQ(Contents(*pak, "manifest.json"), Bytes("{}"));
    EXPECT_TRUE(Contents(*pak, "empty.txt").empty());
    EXPECT_TRUE(pak->IsDirectory(""));
    EXPECT_TRUE(pak->IsDirectory("game"));
    EXPECT_TRUE(pak->IsDirectory("Game/Data"));
    EXPECT_FALSE(pak->IsDirectory("gam"));
    EXPECT_FALSE(pak->IsDirectory("game/scripts/main.lua"));
    EXPECT_FALSE(pak->IsFile("game/scripts"));
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(pak->Read("game/scripts/main.lua", bytes, error, 9999));
    EXPECT_FALSE(pak->Read("missing.txt", bytes, error, 100));

    for (const auto* path : {"game/scripts/main.lua", "game/sounds/tone.wav"}) {
        SCOPED_TRACE(path);
        auto stream = pak->Open(path, error);
        ASSERT_TRUE(stream) << error;
        ASSERT_EQ(stream->Size(), 10000u);
        ASSERT_TRUE(stream->Seek(-8, SEEK_END));
        char tail[8]{};
        ASSERT_EQ(stream->Read(tail, 8), 8u);
        EXPECT_EQ(std::string(tail, 8), "pack me ");
    }
}

TEST_F(Pak, TheSameInputWritesTheSameBytes) {
    PakWriter first, second;
    first.AddBytes("b.txt", Repetitive(500), true);
    first.AddBytes("a/c.txt", Bytes("c"), true);
    second.AddBytes("a/c.txt", Bytes("c"), true);
    second.AddBytes("b.txt", Repetitive(500), true);
    ASSERT_TRUE(first.Write(root / "first.sripak", error)) << error;
    ASSERT_TRUE(second.Write(root / "second.sripak", error)) << error;
    EXPECT_EQ(FileBytes(root / "first.sripak"), FileBytes(root / "second.sripak"));
}

TEST_F(Pak, Zip64LayoutReadsBack) {
    PakWriter writer;
    writer.ForceZip64(true);
    writer.AddBytes("big/packed.txt", Repetitive(5000), true);
    writer.AddBytes("big/stored.bin", Noise(3000), false);
    ASSERT_TRUE(writer.Write(file, error)) << error;
    const auto pak = PakSource::Load(file, error);
    ASSERT_TRUE(pak) << error;
    EXPECT_EQ(Contents(*pak, "big/packed.txt"), Repetitive(5000));
    EXPECT_EQ(Contents(*pak, "big/stored.bin"), Noise(3000));
}

TEST_F(Pak, DamageIsDetected) {
    PakWriter writer;
    writer.AddBytes("packed.txt", Repetitive(10000), true);
    writer.AddBytes("stored.bin", Noise(1000, "STORED-MARKER"), true); // noise does not deflate
    ASSERT_TRUE(writer.Write(file, error)) << error;
    const auto good = FileBytes(file);

    auto damaged = good;
    const std::string marker = "STORED-MARKER";
    const auto at = std::search(damaged.begin(), damaged.end(), marker.begin(), marker.end());
    ASSERT_NE(at, damaged.end());
    at[marker.size() + 10] ^= 0x40;
    damaged[30 + std::string("packed.txt").size() + 5] ^= 0x40; // inside the deflated data, which comes first
    Overwrite(file, damaged);
    {
        const auto pak = PakSource::Load(file, error);
        ASSERT_TRUE(pak) << error; // the directory is intact
        std::vector<std::uint8_t> bytes;
        EXPECT_FALSE(pak->Read("stored.bin", bytes, error, 1 << 20));
        EXPECT_NE(error.find("CRC"), std::string::npos) << error;
        EXPECT_FALSE(pak->Read("packed.txt", bytes, error, 1 << 20));
        EXPECT_FALSE(pak->Open("packed.txt", error));
    }

    Overwrite(file, std::vector<std::uint8_t>(good.begin(), good.begin() + good.size() / 2));
    EXPECT_FALSE(PakSource::Load(file, error));
    Overwrite(file, Bytes("definitely not a zip archive"));
    EXPECT_FALSE(PakSource::Load(file, error));
    EXPECT_NE(error.find("not a .sripak"), std::string::npos) << error;
    EXPECT_FALSE(PakSource::Load(root / "missing.sripak", error));
}

TEST_F(Pak, UnsafeOrDuplicateNamesAreRejected) {
    for (const auto* name : {"../evil.txt", "a/../../b", "a\\b", "/absolute", "a//b", "C:/x", "", "trailing/"}) {
        SCOPED_TRACE(name);
        PakWriter writer;
        writer.AddBytes(name, Bytes("x"), false);
        EXPECT_FALSE(writer.Write(file, error));
        EXPECT_FALSE(fs::exists(file));
    }
    PakWriter duplicates;
    duplicates.AddBytes("Folder/A.txt", Bytes("1"), false);
    duplicates.AddBytes("folder/a.TXT", Bytes("2"), false);
    EXPECT_FALSE(duplicates.Write(file, error));

    // A pack from another tool may still carry one; patch a safe name into an unsafe one.
    PakWriter writer;
    writer.AddBytes("aa/evil.txt", Bytes("x"), false);
    ASSERT_TRUE(writer.Write(file, error)) << error;
    auto bytes = FileBytes(file);
    const std::string from = "aa/evil.txt", to = "../evil.txt";
    for (auto at = std::search(bytes.begin(), bytes.end(), from.begin(), from.end()); at != bytes.end();
         at = std::search(at, bytes.end(), from.begin(), from.end())) {
        at = std::copy(to.begin(), to.end(), at);
    }
    Overwrite(file, bytes);
    EXPECT_FALSE(PakSource::Load(file, error));
    EXPECT_NE(error.find("unsafe"), std::string::npos) << error;
}

TEST_F(Pak, MountedPackServesTheContentLayerAndOutlivesItsMount) {
    PakWriter writer;
    writer.AddBytes("game/a.txt", Bytes("packed"), false);
    ASSERT_TRUE(writer.Write(file, error)) << error;
    auto pak = PakSource::Load(file, error);
    ASSERT_TRUE(pak) << error;
    const fs::path mounted = root / "mounted";
    std::unique_ptr<content::Stream> stream;
    {
        MountScope mount(mounted, std::move(pak));
        std::string text;
        ASSERT_TRUE(ReadTextFile(mounted / "game/a.txt", text, error)) << error;
        EXPECT_EQ(text, "packed");
        EXPECT_TRUE(content::IsDirectory(mounted / "game"));
        stream = content::Open(mounted / "game/a.txt", error);
        ASSERT_TRUE(stream) << error;
    }
    EXPECT_FALSE(content::IsFile(mounted / "game/a.txt"));
    char buffer[6]{};
    ASSERT_EQ(stream->Read(buffer, 6), 6u); // the stream keeps the mapping alive
    EXPECT_EQ(std::string(buffer, 6), "packed");
}

} // namespace
