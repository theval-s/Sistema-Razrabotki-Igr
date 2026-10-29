#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace engine {

/// God I hate windows strings

/// UTF-8 text of a path, with native separators
[[nodiscard]] inline std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

/// UTF-8 text of a path with '/' separators. For names and keys compared as text.
[[nodiscard]] inline std::string ToGenericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

/// Path from UTF-8 text
[[nodiscard]] inline std::filesystem::path FromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace engine
