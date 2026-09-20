#pragma once
#include <cstdint>
#include <string_view>

namespace engine {

//Simple string hash for resource names for shader

//todo: hash collisions resolution and nice error require us to build a bimap

constexpr std::uint32_t FNV_PRIME = 16777619u;
constexpr std::uint32_t FNV_OFFSET_BASIS = 2166136261u;

constexpr std::uint32_t fnv1a_32(const std::string_view str) noexcept {
    std::uint32_t hash = FNV_OFFSET_BASIS;

    for (const char c : str) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= FNV_PRIME;
    }

    return hash;
}

using ResourceID = std::uint32_t;

constexpr ResourceID operator""_rid(
    const char* str,
    const size_t len) {
    return fnv1a_32(std::string_view(str, len));
}

}
