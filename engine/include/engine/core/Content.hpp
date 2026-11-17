#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <engine/engine_export.hpp>

// A shipped game reads its content from a pack. A source mounted over a directory answers for
// every path under it, and whatever it does not have falls through to disk. ReadTextFile and
// ReadBinaryFile already go through mounts; the rest of the engine uses the checks here.

namespace engine::content {

/// A readable, seekable view of one file.
class ENGINE_API Stream {
public:
    virtual ~Stream() = default;
    virtual std::size_t Read(void* data, std::size_t size) = 0;
    /// `origin` is SEEK_SET, SEEK_CUR or SEEK_END; positions outside [0, Size()] fail.
    virtual bool Seek(std::int64_t offset, int origin) = 0;
    [[nodiscard]] virtual std::uint64_t Tell() const = 0;
    [[nodiscard]] virtual std::uint64_t Size() const = 0;
};

/// A read-only file tree such as a .sripak. Paths are normalized generic UTF-8 relative to its
/// root ("" is the root); like the Windows file system, lookups ignore ASCII case. Thread-safe.
class ENGINE_API Source {
public:
    virtual ~Source() = default;
    [[nodiscard]] virtual bool IsFile(std::string_view path) const = 0;
    [[nodiscard]] virtual bool IsDirectory(std::string_view path) const = 0;
    [[nodiscard]] virtual bool Read(std::string_view path, std::vector<std::uint8_t>& data, std::string& error,
                                    std::uint64_t maxSize) const = 0;
    [[nodiscard]] virtual std::unique_ptr<Stream> Open(std::string_view path, std::string& error) const = 0;
};

/// The lookup key Source implementations use, so they all fold case the same way.
[[nodiscard]] ENGINE_API std::string FoldCase(std::string_view path);

/// Replaces whatever was mounted at `root`.
ENGINE_API void Mount(const std::filesystem::path& root, std::shared_ptr<const Source> source);
ENGINE_API void Unmount(const std::filesystem::path& root);

[[nodiscard]] ENGINE_API bool IsFile(const std::filesystem::path& file);
[[nodiscard]] ENGINE_API bool IsDirectory(const std::filesystem::path& directory);
/// Fails with a message naming the file.
[[nodiscard]] ENGINE_API std::unique_ptr<Stream> Open(const std::filesystem::path& file, std::string& error);

/// A stream over bytes that `owner` keeps alive.
[[nodiscard]] ENGINE_API std::unique_ptr<Stream> OpenMemory(std::span<const std::uint8_t> bytes,
                                                            std::shared_ptr<const void> owner);

} // namespace engine::content
