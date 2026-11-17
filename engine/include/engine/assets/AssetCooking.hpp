#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <engine/assets/AssetCommon.hpp>
#include <engine/engine_export.hpp>

namespace engine {

struct CpuPayload;

/// Bumped whenever a cooked layout changes; builds from another version are rejected.
inline constexpr std::uint32_t CookedFormatVersion = 1;

/// Kinds sri-build stores as a cooked payload rather than a copy of the source.
[[nodiscard]] ENGINE_API bool IsCookedKind(AssetKind kind);

/// <source>.cooked: the cooked file sits where its source would be.
[[nodiscard]] ENGINE_API std::filesystem::path CookedPath(const std::filesystem::path& source);

/// Writes the CPU payload of a cooked kind. Meshes must be imported with centerOnLoad=false:
/// centering and the physics geometry are applied when the cooked mesh is loaded.
[[nodiscard]] ENGINE_API bool SerializeCooked(AssetKind kind, const CpuPayload& payload,
                                              std::vector<std::uint8_t>& bytes, std::string& error);
/// Checks the header, version and hash before decoding. On failure `payload` is unchanged.
[[nodiscard]] ENGINE_API bool DeserializeCooked(AssetKind kind, std::span<const std::uint8_t> bytes,
                                                CpuPayload& payload, std::string& error);

} // namespace engine
