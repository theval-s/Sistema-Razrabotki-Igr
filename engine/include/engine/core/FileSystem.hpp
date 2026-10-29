#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <engine/engine_export.hpp>

// Reading/writing the whole files for now

namespace engine {

/// Default upper bound for files parsed as text in one go: JSON documents, shaders.
inline constexpr std::uintmax_t MaxTextFileSize = 64 * 1024 * 1024;

/// Reads a whole file as text and drops a leading UTF-8 byte order mark (windows amirite)
[[nodiscard]] ENGINE_API bool ReadTextFile(const std::filesystem::path& file, std::string& text, std::string& error,
                                           std::uintmax_t maxSize = MaxTextFileSize);

[[nodiscard]] ENGINE_API bool ReadBinaryFile(const std::filesystem::path& file, std::vector<std::uint8_t>& data,
                                             std::string& error,
                                             std::uintmax_t maxSize = std::numeric_limits<std::uintmax_t>::max());

/// Creates or replaces `file` through a temporary file beside it, so a reader (or a crash)
/// sees either the old contents or the new ones, never a partial write.
[[nodiscard]] ENGINE_API bool WriteFileAtomic(const std::filesystem::path& file, std::string_view data,
                                              std::string& error);

/// Creates `file`; fails if anything already exists there. Checked just before opening.
[[nodiscard]] ENGINE_API bool CreateNewFile(const std::filesystem::path& file, std::string_view data,
                                            std::string& error);

}  // namespace engine
