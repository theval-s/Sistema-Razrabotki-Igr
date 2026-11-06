#include <engine/assets/AssetAuthoring.hpp>

#include <cstdint>
#include <stdexcept>
#include <system_error>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/core/ResourceID.hpp>

namespace engine {
namespace {

constexpr std::uintmax_t MaxReferenceScanSize = 16 * 1024 * 1024;

std::string Key(const std::filesystem::path& path) {
    return NormalizeName(ToGenericUtf8(path.lexically_normal()));
}

/// True when `path` is `directory` itself or anything below it.
bool IsInside(const std::filesystem::path& path, const std::filesystem::path& directory) {
    const std::string inner = Key(path);
    std::string outer = Key(directory);
    if (inner == outer) return true;
    if (!outer.ends_with('/')) outer += '/';
    return inner.starts_with(outer);
}

/// `to` is taken by something other than `from` (a case-only rename names the same file).
bool Occupied(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (!std::filesystem::exists(to, ec)) return false;
    return !std::filesystem::equivalent(from, to, ec);
}

/// Copies one file and gives it a meta of its own. Throws on failure.
void CopyFileAsset(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::copy_file(from, to);
    const auto kind = CatalogKindForFile(to);
    if (!kind) return;
    AssetMetadata metadata{NewUuid(), *kind};
    AssetMetadata original;
    if (std::string ignored; ReadAssetMetadata(from, original, ignored)) {
        metadata.centerOnLoad = original.centerOnLoad;
    }
    if (std::string error; !WriteAssetMetadata(to, metadata, error)) throw std::runtime_error(error);
}

}  // namespace



bool MoveAsset(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error) {
    std::error_code ec;
    const auto status = std::filesystem::status(from, ec);
    if (!std::filesystem::exists(status)) {
        error = "no such asset: " + ToGenericUtf8(from);
        return false;
    }
    if (Occupied(from, to)) {
        error = ToGenericUtf8(to) + " already exists";
        return false;
    }
    if (std::filesystem::is_directory(status)) {
        // Sidecars travel with the folder.
        if (IsInside(to, from) && Key(to) != Key(from)) {
            error = "cannot move a folder into itself: " + ToGenericUtf8(from);
            return false;
        }
        std::filesystem::rename(from, to, ec);
        if (ec) {
            error = "cannot move " + ToGenericUtf8(from) + ": " + ec.message();
            return false;
        }
        error.clear();
        return true;
    }

    const auto fromSidecar = AssetMetadataPath(from);
    const auto toSidecar = AssetMetadataPath(to);
    const bool hasSidecar = std::filesystem::is_regular_file(fromSidecar, ec);
    if (hasSidecar && CatalogKindForFile(from) != CatalogKindForFile(to)) {
        error = "renaming would change the asset type: " + ToGenericUtf8(from) + " -> " + ToGenericUtf8(to.filename());
        return false;
    }
    // Without a sidecar of its own the file would adopt a stray one left at the destination.
    if (hasSidecar ? Occupied(fromSidecar, toSidecar) : std::filesystem::exists(toSidecar, ec)) {
        error = ToGenericUtf8(toSidecar) + " already exists";
        return false;
    }
    std::filesystem::rename(from, to, ec);
    if (ec) {
        error = "cannot move " + ToGenericUtf8(from) + ": " + ec.message();
        return false;
    }
    if (hasSidecar) {
        std::filesystem::rename(fromSidecar, toSidecar, ec);
        if (ec) {
            error = "cannot move " + ToGenericUtf8(fromSidecar) + ": " + ec.message();
            std::error_code ignored;
            std::filesystem::rename(to, from, ignored);  // keep the pair together
            return false;
        }
    }
    error.clear();
    return true;
}

bool CopyAsset(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error) {
    std::error_code ec;
    const auto status = std::filesystem::status(from, ec);
    if (!std::filesystem::exists(status)) {
        error = "no such asset: " + ToGenericUtf8(from);
        return false;
    }
    if (std::filesystem::exists(to, ec) || std::filesystem::exists(AssetMetadataPath(to), ec)) {
        error = ToGenericUtf8(to) + " already exists";
        return false;
    }
    try {
        if (!std::filesystem::is_directory(status)) {
            CopyFileAsset(from, to);
        } else {
            if (IsInside(to, from)) throw std::runtime_error("cannot copy a folder into itself");
            std::filesystem::create_directory(to);
            for (const auto& entry : std::filesystem::recursive_directory_iterator(from)) {
                const auto destination = to / entry.path().lexically_relative(from);
                if (entry.is_directory()) {
                    std::filesystem::create_directory(destination);
                } else if (entry.is_regular_file() && entry.path().extension() != ".meta") {
                    CopyFileAsset(entry.path(), destination);
                }
            }
        }
    } catch (const std::exception& e) {
        error = "cannot copy " + ToGenericUtf8(from) + ": " + e.what();
        // Nothing half-copied stays behind for the catalog to trip over.
        std::filesystem::remove_all(to, ec);
        std::filesystem::remove(AssetMetadataPath(to), ec);
        return false;
    }
    error.clear();
    return true;
}

std::vector<std::filesystem::path> FindAssetReferences(const std::filesystem::path& root, const AssetUuid& id) {
    std::vector<std::filesystem::path> result;
    if (!id.IsValid()) return result;
    const std::string needle = id.ToString();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !NormalizeName(ToGenericUtf8(it->path().filename())).ends_with(".json")) {
            continue;
        }
        std::string text, ignored;  // unreadable or huge files can't be what we're after
        if (ReadTextFile(it->path(), text, ignored, MaxReferenceScanSize) && text.find(needle) != std::string::npos) {
            result.push_back(it->path());
        }
    }
    return result;
}

}  // namespace engine
