#include <engine/assets/AssetPathResolver.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cwchar>
#include <utility>

#include <engine/core/ResourceID.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace engine {
namespace {

/// Model formats worth putting in a folder: tried in this order when a mesh name is a
/// directory. Assimp reads far more, these are the ones a DCC tool exports.
constexpr std::array<const wchar_t*, 7> ModelExtensions{
    L".fbx", L".gltf", L".glb", L".obj", L".dae", L".blend", L".ply"};

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path& GameRootOverride() {
    static std::filesystem::path root;
    return root;
}

bool HasExtension(const std::filesystem::path& path, const wchar_t* extension) {
    const std::filesystem::path ext = path.extension();
    if (ext.empty()) {
        return false;
    }
    return _wcsicmp(ext.c_str(), extension) == 0;
}

std::string NormalizedUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.lexically_normal().generic_u8string();
    return NormalizeName(std::string(text.begin(), text.end()));
}

/// "<prefix>/<path relative to root>", empty when `path` is not inside `root`.
std::string NameUnder(const std::string& path, const std::filesystem::path& root, const std::string_view prefix) {
    std::string base = NormalizedUtf8(root);
    if (!base.ends_with('/')) {
        base += '/';
    }
    if (!path.starts_with(base) || path.size() == base.size()) {
        return {};
    }
    return std::string(prefix) + '/' + path.substr(base.size());
}

}  // namespace

std::filesystem::path AssetPathResolver::ResolveMesh(const AssetDesc& desc) {
    std::filesystem::path path = ToAbsolute(desc.path);
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

std::filesystem::path AssetPathResolver::ResolveTexture(const AssetDesc& desc) {
    std::filesystem::path path = ToAbsolute(desc.path);
    if (IsImageFile(path)) {
        return path;
    }
    return path / L"texture.png";
}

std::filesystem::path AssetPathResolver::ResolveShader(const AssetDesc& desc) {
    std::filesystem::path path = ToAbsolute(desc.path);
    if (path.extension().empty()) {
        path += L".hlsl";
    }
    return path;
}

std::filesystem::path AssetPathResolver::DefaultPath(const std::string_view name, const AssetKind kind) {
    if (kind == AssetKind::Shader) {
        if (name.starts_with("engine/")) {
            return std::filesystem::path("engine") / "shaders" / std::filesystem::path(name.substr(7));
        }
        if (name.starts_with("game/")) {
            return std::filesystem::path("game") / "shaders" / std::filesystem::path(name.substr(5));
        }
        if (name.starts_with("shaders/")) {
            return std::filesystem::path("engine") / "shaders" / std::filesystem::path(name.substr(8));
        }
        return std::filesystem::path("game") / "shaders" / std::filesystem::path(name);
    }
    return std::filesystem::path(name);
}

const std::filesystem::path& AssetPathResolver::GetRoot() {
    static const std::filesystem::path root = ExecutableDirectory();
    return root;
}

void AssetPathResolver::SetGameRoot(std::filesystem::path directory) {
    GameRootOverride() = std::move(directory);
}

std::filesystem::path AssetPathResolver::GetGameRoot() {
    const std::filesystem::path& root = GameRootOverride();
    return root.empty() ? GetRoot() / "game" : root;
}

std::filesystem::path AssetPathResolver::ToAbsolute(const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return path;
    }
    if (!GameRootOverride().empty()) {
        auto it = path.begin();
        if (it != path.end() && _wcsicmp(it->c_str(), L"game") == 0) {
            std::filesystem::path result = GameRootOverride();
            for (++it; it != path.end(); ++it) {
                result /= *it;
            }
            return result;
        }
    }
    return GetRoot() / path;
}

std::string AssetPathResolver::ToName(const std::filesystem::path& absolute) {
    const std::string path = NormalizedUtf8(absolute);
    if (std::string name = NameUnder(path, GetGameRoot(), "game"); !name.empty()) {
        return name;
    }
    return NameUnder(path, GetRoot() / "engine", "engine");
}

bool AssetPathResolver::IsImageFile(const std::filesystem::path& path) {
    return HasExtension(path, L".png") || HasExtension(path, L".jpg") ||
           HasExtension(path, L".jpeg") || HasExtension(path, L".tif") ||
           HasExtension(path, L".tiff") || HasExtension(path, L".dds") ||
           HasExtension(path, L".bmp");
}

bool AssetPathResolver::IsModelFile(const std::filesystem::path& path) {
    return std::any_of(ModelExtensions.begin(), ModelExtensions.end(),
                       [&path](const wchar_t* extension) { return HasExtension(path, extension); });
}

}  // namespace engine
