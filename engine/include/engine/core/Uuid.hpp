#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <engine/engine_export.hpp>

namespace engine {

// Persistent IDs use canonical lowercase UUID text on disk and 16 bytes in memory.
// Zero bytes are the default value and IsValid() is false for them. Entity IDs
// nevertheless accept the canonical zero UUID for compatibility with scene v1.
[[nodiscard]] ENGINE_API bool IsUuid(std::string_view text) noexcept;
[[nodiscard]] ENGINE_API std::string NewUuid();
[[nodiscard]] ENGINE_API std::optional<std::array<std::uint8_t, 16>> ParseUuidBytes(std::string_view text) noexcept;
[[nodiscard]] ENGINE_API std::string FormatUuidBytes(const std::array<std::uint8_t, 16>& bytes);

template <class Tag>
class Uuid {
public:
    Uuid() = default;
    Uuid(std::string_view text) noexcept {
        if (auto parsed = ParseUuidBytes(text)) bytes_ = *parsed;
    }
    Uuid(const std::string& text) noexcept : Uuid(std::string_view(text)) {}
    Uuid(const char* text) noexcept : Uuid(std::string_view(text)) {}
    [[nodiscard]] static std::optional<Uuid> Parse(std::string_view text) noexcept {
        auto bytes = ParseUuidBytes(text);
        if (!bytes) return std::nullopt;
        Uuid id;
        id.bytes_ = *bytes;
        return id;
    }
    [[nodiscard]] std::string ToString() const { return FormatUuidBytes(bytes_); }
    [[nodiscard]] bool IsValid() const noexcept {
        for (auto byte : bytes_) if (byte != 0) return true;
        return false;
    }
    [[nodiscard]] bool operator==(const Uuid&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const Uuid&) const noexcept = default;
    [[nodiscard]] const std::array<std::uint8_t, 16>& Bytes() const noexcept { return bytes_; }
private:
    std::array<std::uint8_t, 16> bytes_{};
};

template <class Tag>
struct UuidHash {
    [[nodiscard]] std::size_t operator()(const Uuid<Tag>& id) const noexcept {
        std::size_t hash = 14695981039346656037ull;
        for (const auto byte : id.Bytes()) hash = (hash ^ byte) * 1099511628211ull;
        return hash;
    }
};

struct AssetUuidTag;
struct EntityUuidTag;
using AssetUuid = Uuid<AssetUuidTag>;
using EntityUuid = Uuid<EntityUuidTag>;
using AssetUuidHash = UuidHash<AssetUuidTag>;
using EntityUuidHash = UuidHash<EntityUuidTag>;

} // namespace engine
