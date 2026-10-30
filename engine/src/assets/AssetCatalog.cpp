#include <engine/assets/AssetCatalog.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include <engine/core/FileSystem.hpp>
#include <engine/core/ResourceID.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {
using Json = nlohmann::json;

constexpr std::uintmax_t MaxMetadataSize = 1024 * 1024;

std::string_view KindName(CatalogAssetKind kind) {
    switch (kind) {
    case CatalogAssetKind::Mesh: return "mesh";
    case CatalogAssetKind::Texture: return "texture";
    case CatalogAssetKind::Shader: return "shader";
    case CatalogAssetKind::Material: return "material";
    case CatalogAssetKind::MaterialInstance: return "materialInstance";
    case CatalogAssetKind::Scene: return "scene";
    }
    return {};
}

Json Read(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 16) throw std::invalid_argument("asset JSON nesting is too deep");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate JSON field: " + value.get<std::string>());
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text.begin(), text.end(), callback);
}

bool ValidPath(std::string_view path) {
    if (!(path.starts_with("game/") || path.starts_with("engine/"))) return false;
    for (const unsigned char character : path) {
        if (character == '\\' || character < 0x20 || character == 0x7f) return false;
    }
    if (path.find_first_of("<>:\"|?*") != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto segment = path.substr(start, end == std::string_view::npos ? end : end - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (end == std::string_view::npos) return true;
        start = end + 1;
    }
    return false;
}

template <class T, class Parse>
bool Decode(std::string_view text, T& value, std::string& error, Parse parse) {
    try {
        T result = parse(Read(text));
        value = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

AssetReference ReferenceFromJson(const Json& j) {
    if (!j.is_object() || j.size() != 2 || !j.contains("id") || !j.contains("path"))
        throw std::invalid_argument("reference needs exactly id and path");
    AssetReference ref{j.at("id").get<std::string>(), j.at("path").get<std::string>()};
    if (!ref.id.IsValid() || !ValidPath(ref.path))
        throw std::invalid_argument("invalid asset UUID or root-qualified path hint");
    return ref;
}

AssetMetadata MetadataFromJson(const Json& j) {
    if (!j.is_object() || !j.contains("version") || !j.contains("id") || !j.contains("kind") ||
        j.at("version") != 1 || j.size() > 4)
        throw std::invalid_argument("metadata needs version 1, id and kind");
    AssetMetadata meta;
    meta.id = AssetUuid(j.at("id").get<std::string>());
    if (!meta.id.IsValid()) throw std::invalid_argument("invalid metadata UUID");
    const auto kind = j.at("kind").get<std::string>();
    bool found = false;
    for (const auto candidate : {CatalogAssetKind::Mesh, CatalogAssetKind::Texture, CatalogAssetKind::Shader,
                                  CatalogAssetKind::Material, CatalogAssetKind::MaterialInstance, CatalogAssetKind::Scene}) {
        if (kind == KindName(candidate)) { meta.kind = candidate; found = true; break; }
    }
    if (!found || (j.contains("centerOnLoad") && meta.kind != CatalogAssetKind::Mesh))
        throw std::invalid_argument("unknown metadata kind or import setting on non-mesh");
    if (j.contains("centerOnLoad")) meta.centerOnLoad = j.at("centerOnLoad").get<bool>();
    if (j.size() != 3 + (j.contains("centerOnLoad") ? 1 : 0))
        throw std::invalid_argument("unknown metadata field");
    return meta;
}

} // namespace

std::optional<CatalogAssetKind> CatalogKindForFile(const std::filesystem::path& path) {
    const std::string name = NormalizeName(ToGenericUtf8(path.filename()));
    if (name.ends_with(".material-instance.json")) return CatalogAssetKind::MaterialInstance;
    if (name.ends_with(".material.json")) return CatalogAssetKind::Material;
    if (name.ends_with(".scene.json")) return CatalogAssetKind::Scene;
    const std::string ext = NormalizeName(ToGenericUtf8(path.extension()));
    if (ext == ".hlsl") return CatalogAssetKind::Shader;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tif" ||
        ext == ".tiff" || ext == ".dds" || ext == ".bmp") return CatalogAssetKind::Texture;
    if (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj" ||
        ext == ".dae" || ext == ".blend" || ext == ".ply") return CatalogAssetKind::Mesh;
    return std::nullopt;
}

bool ParseAssetMetadata(std::string_view json, AssetMetadata& value, std::string& error) {
    return Decode(json, value, error, MetadataFromJson);
}

bool SerializeAssetMetadata(const AssetMetadata& value, std::string& json, std::string& error) {
    try {
        Json encoded = {{"version", 1}, {"id", value.id.ToString()}, {"kind", KindName(value.kind)}};
        if (value.kind == CatalogAssetKind::Mesh) encoded["centerOnLoad"] = value.centerOnLoad;
        const auto text = encoded.dump(2);
        [[maybe_unused]] const auto checked = MetadataFromJson(Read(text));
        json = text;
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

std::filesystem::path AssetMetadataPath(const std::filesystem::path& source) {
    auto sidecar = source;
    sidecar += ".meta";
    return sidecar;
}

bool ReadAssetMetadata(const std::filesystem::path& source, AssetMetadata& value, std::string& error) {
    const auto sidecar = AssetMetadataPath(source);
    std::string text;
    if (!ReadTextFile(sidecar, text, error, MaxMetadataSize)) return false;
    if (!ParseAssetMetadata(text, value, error)) {
        error = ToGenericUtf8(sidecar) + ": " + error;
        return false;
    }
    return true;
}

bool WriteAssetMetadata(const std::filesystem::path& source, const AssetMetadata& value, std::string& error) {
    const auto kind = CatalogKindForFile(source);
    if (!kind || *kind != value.kind) {
        error = "metadata kind does not match the file type: " + ToGenericUtf8(source);
        return false;
    }
    std::string json;
    if (!SerializeAssetMetadata(value, json, error)) return false;
    return CreateNewFile(AssetMetadataPath(source), json + '\n', error);
}

bool ParseAssetReference(std::string_view json, AssetReference& value, std::string& error) {
    return Decode(json, value, error, ReferenceFromJson);
}

bool SerializeAssetReference(const AssetReference& value, std::string& json, std::string& error) {
    try {
        const auto text = Json{{"id", value.id.ToString()}, {"path", value.path}}.dump();
        [[maybe_unused]] const auto checked = ReferenceFromJson(Read(text));
        json = text;
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool AssetCatalog::Scan(const std::filesystem::path& gameRoot, const std::filesystem::path& engineRoot,
                        std::string& error) {
    std::unordered_map<AssetUuid, CatalogEntry, AssetUuidHash> entries;
    std::unordered_map<std::string, AssetUuid> paths;
    std::string missingError;
    try {
        for (const auto& [root, prefix] : {std::pair{gameRoot, std::string("game")},
                                          std::pair{engineRoot, std::string("engine")}}) {
            if (!std::filesystem::exists(root)) continue;
            for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
                if (!file.is_regular_file()) continue;
                if (CatalogKindForFile(file.path())) {
                    if (!std::filesystem::is_regular_file(AssetMetadataPath(file.path())) && missingError.empty())
                        missingError = "missing metadata: " + ToGenericUtf8(file.path());
                }
                if (file.path().extension() != ".meta") continue;
                auto source = file.path();
                source.replace_extension();
                if (!std::filesystem::is_regular_file(source))
                    throw std::runtime_error("orphaned metadata: " + ToGenericUtf8(file.path()));
                const auto kind = CatalogKindForFile(source);
                if (!kind) throw std::runtime_error("metadata on unsupported file: " + ToGenericUtf8(source));
                AssetMetadata meta;
                if (!ReadAssetMetadata(source, meta, error)) return false;
                if (meta.kind != *kind) throw std::runtime_error("metadata kind mismatch: " + ToGenericUtf8(file.path()));
                const std::string logical = prefix + "/" + ToGenericUtf8(source.lexically_relative(root));
                const std::string key = NormalizeName(logical);
                if (!paths.emplace(key, meta.id).second)
                    throw std::runtime_error("duplicate asset path: " + logical);
                const auto [it, inserted] = entries.emplace(meta.id, CatalogEntry{meta, logical, source});
                if (!inserted) throw std::runtime_error("duplicate asset UUID: " + meta.id.ToString() + " at " + ToGenericUtf8(source));
            }
        }
    } catch (const std::exception& e) { error = e.what(); return false; }
    entries_ = std::move(entries);
    paths_ = std::move(paths);
    error = std::move(missingError);
    return error.empty();
}

const CatalogEntry* AssetCatalog::Find(std::string_view uuid) const {
    const auto parsed = AssetUuid::Parse(uuid);
    return parsed ? Find(*parsed) : nullptr;
}

const CatalogEntry* AssetCatalog::Find(const AssetUuid& uuid) const {
    const auto it = entries_.find(uuid);
    return it == entries_.end() ? nullptr : &it->second;
}

const CatalogEntry* AssetCatalog::Find(std::string_view uuid, CatalogAssetKind kind) const {
    const auto* entry = Find(uuid);
    return entry && entry->metadata.kind == kind ? entry : nullptr;
}

const CatalogEntry* AssetCatalog::Find(const AssetUuid& uuid, CatalogAssetKind kind) const {
    const auto* entry = Find(uuid);
    return entry && entry->metadata.kind == kind ? entry : nullptr;
}

const CatalogEntry* AssetCatalog::FindPath(std::string_view path) const {
    const auto it = paths_.find(NormalizeName(path));
    return it == paths_.end() ? nullptr : Find(it->second);
}

std::vector<const CatalogEntry*> AssetCatalog::List(CatalogAssetKind kind) const {
    std::vector<const CatalogEntry*> result;
    for (const auto& [id, entry] : entries_) {
        if (entry.metadata.kind == kind) result.push_back(&entry);
    }
    std::ranges::sort(result, {}, &CatalogEntry::path);
    return result;
}

bool GenerateMissingAssetMetadata(const std::filesystem::path& root, std::size_t& generated, std::string& error) {
    std::size_t count = 0;
    try {
        if (!std::filesystem::is_directory(root)) throw std::runtime_error("not an asset directory: " + ToGenericUtf8(root));
        for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
            if (!file.is_regular_file()) continue;
            const auto kind = CatalogKindForFile(file.path());
            if (!kind) continue;
            const auto sidecar = AssetMetadataPath(file.path());
            if (std::filesystem::exists(sidecar)) continue;
            if (!WriteAssetMetadata(file.path(), {NewUuid(), *kind}, error)) {
                // Another process created it while scanning: keep theirs.
                if (std::filesystem::exists(sidecar)) continue;
                return false;
            }
            ++count;
        }
    } catch (const std::exception& e) { error = e.what(); return false; }
    generated = count;
    error.clear();
    return true;
}
} // namespace engine
