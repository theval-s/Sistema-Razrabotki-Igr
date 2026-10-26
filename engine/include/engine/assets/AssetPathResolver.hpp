#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <engine/assets/AssetCommon.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace engine {

/// Maps a logical asset name to a file on disk.
///
/// The root is the directory of the executable, never the current working directory:
/// double-clicking an exe or launching it from another tool would otherwise change which
/// assets are found.
class AssetPathResolver {
public:
    [[nodiscard]] static std::filesystem::path Utf8Path(std::string_view name) {
        return std::filesystem::path(std::u8string(name.begin(), name.end()));
    }
    /// Mesh assets address a folder that holds model.<ext> (and optionally texture.png);
    /// a name ending in a model extension addresses that file directly. Assimp chooses the
    /// reader from the extension, so the file format itself is not fixed to .obj.
    [[nodiscard]] static std::filesystem::path ResolveMesh(const AssetDesc& desc) {
        std::filesystem::path path = MakeAbsolute(desc.path);
        if (IsModelFile(path)) {
            return path;
        }
        for (const wchar_t* extension : ModelExtensions) {
            std::filesystem::path candidate = path / (std::wstring(L"model") + extension);
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return path / L"model.obj";  // nothing found: name the conventional file in the error
    }

    /// A name ending in a known image extension addresses that file; anything else is a
    /// folder holding texture.png, which is what the legacy model loader used.
    [[nodiscard]] static std::filesystem::path ResolveTexture(const AssetDesc& desc) {
        std::filesystem::path path = MakeAbsolute(desc.path);
        if (IsImageFile(path)) {
            return path;
        }
        return path / L"texture.png";
    }

    /// Names are game/..., engine/... or the legacy shaders/... (engine).
    /// The executable directory is the common root for files and generated source locations.
    [[nodiscard]] static std::filesystem::path ResolveShader(const AssetDesc& desc) {
        std::filesystem::path path = MakeAbsolute(desc.path);
        if (path.extension().empty()) {
            path += L".hlsl";
        }
        return path;
    }

    [[nodiscard]] static std::filesystem::path ResolveMaterial(const AssetDesc& desc, bool instance = false) {
        auto path = MakeAbsolute(desc.path);
        if (path.extension().empty()) path += instance ? L".material-instance.json" : L".material.json";
        return path;
    }

    /// Default on-disk location for a kind when the caller gave only a logical name.
    [[nodiscard]] static std::filesystem::path DefaultPath(std::string_view name, AssetKind kind) {
        if (kind == AssetKind::Shader) {
            if (name.starts_with("engine/")) {
                return std::filesystem::path("engine") / "shaders" / Utf8Path(name.substr(7));
            }
            if (name.starts_with("game/")) {
                return std::filesystem::path("game") / "shaders" / Utf8Path(name.substr(5));
            }
            if (name.starts_with("shaders/")) {
                return std::filesystem::path("engine") / "shaders" / Utf8Path(name.substr(8));
            }
            return std::filesystem::path("game") / "shaders" / Utf8Path(name);
        }
        return Utf8Path(name);
    }

    [[nodiscard]] static const std::filesystem::path& GetRoot() {
        return Root();
    }

private:
    [[nodiscard]] static const std::filesystem::path& Root() {
        static const std::filesystem::path root = ExecutableDirectory();
        return root;
    }

    [[nodiscard]] static std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(buffer).parent_path();
        }
#endif
        return std::filesystem::current_path();
    }

    [[nodiscard]] static std::filesystem::path MakeAbsolute(const std::filesystem::path& path) {
        if (path.is_absolute()) {
            return path;
        }
        return Root() / path;
    }

    [[nodiscard]] static bool HasExtension(const std::filesystem::path& path, const wchar_t* extension) {
        const std::filesystem::path ext = path.extension();
        if (ext.empty()) {
            return false;
        }
        return _wcsicmp(ext.c_str(), extension) == 0;
    }

    [[nodiscard]] static bool IsImageFile(const std::filesystem::path& path) {
        return HasExtension(path, L".png") || HasExtension(path, L".jpg") ||
               HasExtension(path, L".jpeg") || HasExtension(path, L".tif") ||
               HasExtension(path, L".tiff") || HasExtension(path, L".dds") ||
               HasExtension(path, L".bmp");
    }

    /// Model formats worth putting in a folder: tried in this order when a mesh name is a
    /// directory. Assimp reads far more, these are the ones a DCC tool exports.
    static constexpr std::array<const wchar_t*, 7> ModelExtensions{
        L".fbx", L".gltf", L".glb", L".obj", L".dae", L".blend", L".ply"};

    [[nodiscard]] static bool IsModelFile(const std::filesystem::path& path) {
        return std::any_of(ModelExtensions.begin(), ModelExtensions.end(),
                           [&path](const wchar_t* extension) { return HasExtension(path, extension); });
    }
};

}  // namespace engine
