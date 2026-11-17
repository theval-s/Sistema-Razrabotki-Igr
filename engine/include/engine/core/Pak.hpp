#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <engine/core/Content.hpp>
#include <engine/engine_export.hpp>

// A .sripak is a plain zip archive (stored or deflated entries, zip64 when large) so any zip
// tool can open it; the extension only says what it is for.

namespace engine {

/// The pack sri-build writes beside the game executable; the runtime mounts it over that folder.
inline constexpr const wchar_t* PakFileName = L"game.sripak";

/// Reads a pack through a read-only memory mapping. Lookups ignore ASCII case; whole-file reads
/// check the CRC. Streams of stored entries read the mapping directly. Thread-safe.
class ENGINE_API PakSource final : public content::Source {
public:
    [[nodiscard]] static std::shared_ptr<PakSource> Load(const std::filesystem::path& file, std::string& error);
    ~PakSource() override;

    bool IsFile(std::string_view path) const override;
    bool IsDirectory(std::string_view path) const override;
    bool Read(std::string_view path, std::vector<std::uint8_t>& data, std::string& error,
              std::uint64_t maxSize) const override;
    std::unique_ptr<content::Stream> Open(std::string_view path, std::string& error) const override;

    /// Every file's path as written, sorted.
    [[nodiscard]] std::vector<std::string> Files() const;

    struct Impl;

private:
    explicit PakSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Collects files and writes them as one pack, sorted by path, with fixed timestamps, so the
/// same input always gives the same bytes.
class ENGINE_API PakWriter {
public:
    /// `path` is generic UTF-8 relative to where the pack is mounted. `compress` allows deflate,
    /// which is kept only when it saves space; leave it off for streamed or compressed formats.
    void AddFile(std::string path, std::filesystem::path source, bool compress);
    void AddBytes(std::string path, std::vector<std::uint8_t> bytes, bool compress);
    [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
    /// Always write zip64 records, for testing the large-pack layout.
    void ForceZip64(bool force) noexcept { forceZip64_ = force; }
    /// Writes through a temporary file beside `file`; on failure `file` is left untouched.
    [[nodiscard]] bool Write(const std::filesystem::path& file, std::string& error) const;

private:
    struct Entry {
        std::string path;
        std::filesystem::path source; // empty when `bytes` holds the contents
        std::vector<std::uint8_t> bytes;
        bool compress = false;
    };
    std::vector<Entry> entries_;
    bool forceZip64_ = false;
};

} // namespace engine
