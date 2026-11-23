#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <engine/assets/AssetCommon.hpp>
#include <engine/engine_export.hpp>

namespace engine {

class AssetCatalog;
struct CpuPayload;

/// Bumped whenever a cooked layout changes; builds from another version are rejected.
inline constexpr std::uint32_t CookedFormatVersion = 1;
/// Shader libraries also depend on engine shader interfaces, not just the file
/// encoding. Version 2 requires the four-target material ABI and 32-byte b2.
inline constexpr std::uint32_t CookedShaderFormatVersion = 2;

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

/// Precompiled bytecode of every shader variant a cooked build uses, written beside its manifest.
class ENGINE_API ShaderLibrary {
public:
    /// Logical file name, stage, entry point and defines; define order does not matter.
    [[nodiscard]] static std::string Key(const std::filesystem::path& file, ShaderType stage,
                                         const ShaderCompileOptions& options);
    void Add(std::string key, std::vector<std::uint8_t> bytecode);
    [[nodiscard]] const std::vector<std::uint8_t>* Find(const std::string& key) const;
    [[nodiscard]] std::size_t Size() const noexcept { return shaders_.size(); }
    [[nodiscard]] bool Serialize(std::vector<std::uint8_t>& bytes, std::string& error) const;
    /// On failure the library is unchanged.
    [[nodiscard]] bool Deserialize(std::span<const std::uint8_t> bytes, std::string& error);

private:
    std::map<std::string, std::vector<std::uint8_t>> shaders_; // sorted, so builds are reproducible
};

inline constexpr const wchar_t* ShaderLibraryName = L"shaders.cooked";

/// The library beside the active manifest, loaded once per manifest. Null in source mode or
/// when it cannot be read (`error` says why). Thread-safe.
[[nodiscard]] ENGINE_API std::shared_ptr<const ShaderLibrary> CookedShaderLibrary(std::string& error);

/// Precompiles the engine's render pass variants and every hook of the catalog's materials.
[[nodiscard]] ENGINE_API bool CookShaderLibrary(const AssetCatalog& catalog, ShaderLibrary& library,
                                                std::vector<std::string>& errors);

} // namespace engine
