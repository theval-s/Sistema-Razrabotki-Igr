#include <engine/assets/ModelImport.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <nlohmann/json.hpp>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

#include "ModelSlots.hpp"

namespace engine {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// The extension we decode for an embedded image's format hint; empty when we can't.
std::string ImageExtension(const std::string& hint) {
    const std::string lower = Lower(hint);
    if (lower == "png") return ".png";
    if (lower == "jpg" || lower == "jpeg") return ".jpg";
    if (lower == "bmp") return ".bmp";
    if (lower == "tif" || lower == "tiff") return ".tif";
    if (lower == "dds") return ".dds";
    return {};
}

/// Cuts `text` to at most `size` bytes without splitting a UTF-8 sequence.
void TruncateUtf8(std::string& text, std::size_t size) {
    if (text.size() <= size) return;
    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) --size;
    text.resize(size);
}

/// `name` made safe as one file name: no separators, reserved characters or device names.
std::string SafeFileName(std::string name, const std::string& fallback) {
    for (char& c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
    }
    TruncateUtf8(name, 96);
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) name = fallback;
    static constexpr std::array<std::string_view, 22> Devices{
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (std::ranges::find(Devices, Lower(name)) != Devices.end()) name += '_';
    return name;
}

/// `stem` + `extension`, numbered "stem (2)" while the name is taken (case-insensitively).
std::string UniqueName(const std::string& stem, const std::string& extension, std::set<std::string>& taken) {
    std::string name = stem + extension;
    for (int number = 2; !taken.insert(Lower(name)).second; ++number) {
        name = std::format("{} ({}){}", stem, number, extension);
    }
    return name;
}

std::string DecodeUri(std::string_view uri) {
    std::string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
            decoded += static_cast<char>(std::stoi(std::string(uri.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            decoded += uri[i];
        }
    }
    return decoded;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

/// A file the model names, as written, URI-decoded, or by its name beside the model or in textures/
/// there: exporters often keep the artist's absolute paths.
fs::path FindNamedFile(const fs::path& directory, const std::string& named) {
    for (const std::string& text : {named, DecodeUri(named)}) {
        const fs::path path = FromUtf8(text);
        for (const fs::path& candidate : {path.is_absolute() ? path : directory / path, directory / path.filename(),
                                          directory / "textures" / path.filename()}) {
            if (IsFile(candidate)) return candidate.lexically_normal();
        }
    }
    return {};
}

/// The stem an embedded image's name gives, without an image extension it may carry.
std::string EmbeddedStem(const std::string& name) {
    const fs::path file = FromUtf8(name).filename();
    return ToUtf8(AssetPathResolver::IsImageFile(file) ? file.stem() : file);
}

ModelTexture DescribeTexture(const aiScene& scene, const fs::path& model, const std::string& named,
                             const std::string& slot, std::set<std::string>& taken) {
    ModelTexture texture;
    std::string stem;
    std::string extension;
    if (const aiTexture* embedded = scene.GetEmbeddedTexture(named.c_str())) {
        stem = EmbeddedStem(embedded->mFilename.C_Str());
        if (embedded->mHeight != 0) {
            texture.problem = "its embedded image is raw texels, which aren't supported";
        } else if (extension = ImageExtension(embedded->achFormatHint); extension.empty()) {
            texture.problem = std::format("embedded '{}' images aren't supported", embedded->achFormatHint);
        } else {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(embedded->pcData);
            texture.bytes.assign(bytes, bytes + embedded->mWidth);
        }
    } else {
        texture.file = FindNamedFile(model.parent_path(), named);
        const fs::path file = texture.file.empty() ? FromUtf8(named) : texture.file;
        stem = ToUtf8(file.stem());
        extension = Lower(ToUtf8(file.extension()));
        if (texture.file.empty()) {
            texture.problem = std::format("'{}' isn't there", named);
        } else if (!AssetPathResolver::IsImageFile(texture.file)) {
            texture.problem = std::format("'{}' isn't an image format we read", ToUtf8(texture.file.filename()));
        }
    }
    texture.name = UniqueName(SafeFileName(stem, slot + " base color"), extension, taken);
    return texture;
}

/// Files a .gltf or .obj loads by name, that exist beside it.
std::vector<fs::path> Companions(const fs::path& model) {
    std::vector<std::string> named;
    const std::string extension = Lower(ToUtf8(model.extension()));
    std::string text;
    if (std::string ignored; (extension != ".gltf" && extension != ".obj") || !ReadTextFile(model, text, ignored)) {
        return {};
    }
    if (extension == ".gltf") {
        const auto json = nlohmann::json::parse(text, nullptr, false);
        if (!json.is_object()) return {};
        for (const char* key : {"buffers", "images"}) {
            if (!json.contains(key) || !json.at(key).is_array()) continue;
            for (const auto& item : json.at(key)) {
                if (!item.is_object() || !item.contains("uri") || !item.at("uri").is_string()) continue;
                const auto uri = item.at("uri").get<std::string>();
                if (!uri.starts_with("data:")) named.push_back(DecodeUri(uri));
            }
        }
    } else {
        std::size_t start = 0;
        while (start < text.size()) {
            const auto end = std::min(text.find('\n', start), text.size());
            std::string_view line(text.data() + start, end - start);
            start = end + 1;
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
            if (line.size() > 7 && line.starts_with("mtllib") && std::isspace(static_cast<unsigned char>(line[6]))) {
                line.remove_prefix(7);
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
                named.emplace_back(line);
            }
        }
    }
    std::vector<fs::path> files;
    for (const std::string& name : named) {
        const fs::path file = (model.parent_path() / FromUtf8(name)).lexically_normal();
        if (IsFile(file) && std::ranges::find(files, file) == files.end()) files.push_back(file);
    }
    return files;
}

/// A reference's path hint: the file's game/... name with its case kept.
std::string Hint(const fs::path& file) {
    const std::string name = AssetPathResolver::ToName(file);
    if (name.empty()) throw std::runtime_error(ToUtf8(file) + " is not inside the game content folder");
    const auto segments = static_cast<std::size_t>(std::ranges::count(name, '/'));
    std::vector<std::string> parts;
    for (const auto& part : file.lexically_normal()) parts.push_back(ToGenericUtf8(part));
    std::string hint = name.substr(0, name.find('/'));
    for (std::size_t i = parts.size() - segments; i < parts.size(); ++i) hint += '/' + parts[i];
    return hint;
}

/// What an extraction created, removed again unless it is committed.
class Transaction {
public:
    ~Transaction() {
        if (committed_) return;
        std::error_code ec;
        for (auto it = files_.rbegin(); it != files_.rend(); ++it) fs::remove(*it, ec);
        for (auto it = folders_.rbegin(); it != folders_.rend(); ++it) fs::remove(*it, ec);  // only if empty
    }
    void Commit() { committed_ = true; }

    void Folder(const fs::path& folder) {
        std::vector<fs::path> missing;
        for (fs::path at = folder; !at.empty() && !fs::exists(at); at = at.parent_path()) missing.push_back(at);
        fs::create_directories(folder);
        folders_.insert(folders_.end(), missing.rbegin(), missing.rend());
    }

    /// The file's reference, giving it a meta when it has none.
    AssetReference Adopt(const fs::path& file, CatalogAssetKind kind) {
        AssetMetadata meta;
        std::string error;
        if (fs::exists(AssetMetadataPath(file))) {
            if (!ReadAssetMetadata(file, meta, error)) throw std::runtime_error(error);
            if (meta.kind != kind) throw std::runtime_error(ToUtf8(file) + " is not the kind of asset expected there");
        } else {
            meta = {NewUuid(), kind};
            if (!WriteAssetMetadata(file, meta, error)) throw std::runtime_error(error);
            files_.push_back(AssetMetadataPath(file));
        }
        return {meta.id, Hint(file)};
    }

    /// Writes `bytes` to `file` with a meta, or adopts what is already there.
    AssetReference Create(const fs::path& file, std::string_view bytes, CatalogAssetKind kind) {
        if (!fs::exists(file)) {
            Folder(file.parent_path());
            if (std::string error; !CreateNewFile(file, bytes, error)) throw std::runtime_error(error);
            files_.push_back(file);
        }
        return Adopt(file, kind);
    }

    AssetReference Copy(const fs::path& from, const fs::path& to, CatalogAssetKind kind) {
        if (!fs::exists(to)) {
            Folder(to.parent_path());
            fs::copy_file(from, to);
            files_.push_back(to);
        }
        return Adopt(to, kind);
    }

private:
    std::vector<fs::path> files_;
    std::vector<fs::path> folders_;
    bool committed_ = false;
};

/// A texture file in the project used in place, else one copied or written into `textures`.
std::optional<AssetReference> PlaceTexture(Transaction& transaction, const ModelTexture& texture,
                                           const fs::path& textures, const bool extract) {
    if (!texture.file.empty() && !AssetPathResolver::ToName(texture.file).empty()) {
        return transaction.Adopt(texture.file, CatalogAssetKind::Texture);
    }
    if (!extract) return std::nullopt;
    const fs::path target = textures / FromUtf8(texture.name);
    if (!texture.file.empty()) return transaction.Copy(texture.file, target, CatalogAssetKind::Texture);
    return transaction.Create(target, {reinterpret_cast<const char*>(texture.bytes.data()), texture.bytes.size()},
                              CatalogAssetKind::Texture);
}

/// ExtractModelMaterials with `info` from inspecting `model`, or the file it was copied from.
bool Extract(const fs::path& model, const ModelInfo& info, const ModelMaterialSettings& settings,
             std::vector<std::string>& warnings, std::string& error) {
    try {
        AssetMetadata meta;
        if (!ReadAssetMetadata(model, meta, error)) return false;
        if (meta.kind != CatalogAssetKind::Mesh) throw std::runtime_error(ToUtf8(model) + " is not a mesh");

        Transaction transaction;
        const fs::path folder = ModelMaterialsFolder(model);
        std::vector<std::optional<AssetReference>> textures(info.textures.size());
        for (std::size_t i = 0; i < info.textures.size(); ++i) {
            const ModelTexture& texture = info.textures[i];
            if (!texture.problem.empty()) {
                warnings.push_back(std::format("{}: texture {} left out, {}", ToUtf8(model.filename()), texture.name,
                                               texture.problem));
                continue;
            }
            textures[i] = PlaceTexture(transaction, texture, folder / "textures", settings.extractTextures);
        }
        std::optional<AssetReference> fallback;
        if (!settings.fallbackTexture.empty()) {
            if (!IsFile(settings.fallbackTexture) || !AssetPathResolver::IsImageFile(settings.fallbackTexture)) {
                throw std::runtime_error(ToUtf8(settings.fallbackTexture) + " is not an image file we read");
            }
            ModelTexture texture{ToUtf8(settings.fallbackTexture.filename()), {}, settings.fallbackTexture, {}};
            fallback = PlaceTexture(transaction, texture, folder / "textures", true);
        }

        if (settings.baseMaterial) {
            std::set<std::string> names;
            meta.materials.clear();
            for (std::size_t slot = 0; slot < info.slots.size(); ++slot) {
                const ModelSlotInfo& source = info.slots[slot];
                const std::string name = UniqueName(SafeFileName(source.name, std::format("Slot {}", slot)),
                                                    ".material-instance.json", names);
                std::optional<AssetReference> albedo = source.texture ? textures[*source.texture] : std::nullopt;
                if (!albedo) albedo = fallback;
                MaterialInstanceDefinition definition;
                definition.version = 2;
                definition.material = settings.baseMaterial->path;
                definition.materialReference = settings.baseMaterial;
                if (albedo) {
                    definition.parameters.albedoTexture = albedo->path;
                    definition.parameters.albedoReference = albedo;
                }
                std::string text;
                if (!SerializeMaterialInstanceDefinition(definition, text, error)) return false;
                meta.materials.emplace_back(transaction.Create(folder / "materials" / FromUtf8(name), text + '\n',
                                                               CatalogAssetKind::MaterialInstance));
            }
            std::string text;
            if (!SerializeAssetMetadata(meta, text, error) ||
                !WriteFileAtomic(AssetMetadataPath(model), text + '\n', error)) {
                return false;
            }
        }
        transaction.Commit();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

}  // namespace

bool InspectModel(const fs::path& model, ModelInfo& info, std::string& error) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(ToUtf8(model), detail::StaticMeshStructureFlags);
    if (scene == nullptr || scene->mRootNode == nullptr) {
        error = std::format("Assimp failed to read '{}': {}", ToUtf8(model), importer.GetErrorString());
        return false;
    }
    ModelInfo result;
    std::unordered_map<std::string, std::size_t> textures;  // by the model's name for them
    std::set<std::string> names;
    for (const detail::ModelSlot& slot : detail::CollectModelSlots(*scene).slots) {
        ModelSlotInfo described{slot.name, std::nullopt, slot.texture.uvChannel};
        if (!slot.texture.path.empty()) {
            const auto [it, added] = textures.try_emplace(slot.texture.path, result.textures.size());
            if (added) result.textures.push_back(DescribeTexture(*scene, model, slot.texture.path, slot.name, names));
            described.texture = it->second;
        }
        result.slots.push_back(std::move(described));
    }
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        if (!detail::IsTriangleMesh(scene->mMeshes[i])) continue;
        result.vertices += scene->mMeshes[i]->mNumVertices;
        result.triangles += scene->mMeshes[i]->mNumFaces;
    }
    result.companions = Companions(model);
    info = std::move(result);
    error.clear();
    return true;
}

fs::path ModelMaterialsFolder(const fs::path& model) {
    if (Lower(ToUtf8(model.stem())) == "model") return model.parent_path();
    return model.parent_path() / model.stem();
}

bool ExtractModelMaterials(const fs::path& model, const ModelMaterialSettings& settings,
                           std::vector<std::string>& warnings, std::string& error) {
    ModelInfo info;
    return InspectModel(model, info, error) && Extract(model, info, settings, warnings, error);
}

bool ImportModel(const fs::path& model, const fs::path& folder, const bool centerOnLoad,
                 const ModelMaterialSettings& settings, std::vector<std::string>& warnings, std::string& error) {
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        error = ToUtf8(folder) + " already exists";
        return false;
    }
    try {
        ModelInfo info;
        if (!InspectModel(model, info, error)) return false;
        fs::create_directories(folder);
        const fs::path target = folder / FromUtf8("model" + Lower(ToUtf8(model.extension())));
        fs::copy_file(model, target);
        for (const fs::path& companion : info.companions) {
            const fs::path relative = companion.lexically_relative(model.parent_path());
            if (relative.empty() || *relative.begin() == "..") {
                // An image still becomes a texture below, from where it is.
                if (AssetPathResolver::IsImageFile(companion)) continue;
                throw std::runtime_error(std::format("the model loads {} from outside its folder", ToUtf8(companion)));
            }
            const fs::path copy = folder / relative;
            fs::create_directories(copy.parent_path());
            fs::copy_file(companion, copy);
            if (const auto kind = CatalogKindForFile(copy); kind && !WriteAssetMetadata(copy, {NewUuid(), *kind}, error)) {
                throw std::runtime_error(error);
            }
            for (ModelTexture& texture : info.textures) {
                if (texture.file == companion) texture.file = copy;
            }
        }
        if (!WriteAssetMetadata(target, {NewUuid(), CatalogAssetKind::Mesh, centerOnLoad}, error) ||
            !Extract(target, info, settings, warnings, error)) {
            throw std::runtime_error(error);
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        fs::remove_all(folder, ec);  // nothing half-imported stays behind
        return false;
    }
}

}  // namespace engine
