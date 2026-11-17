#include <engine/assets/RigAuthoring.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include "RigProbe.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <mutex>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace engine {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Tree = std::map<std::string, std::string>;
constexpr std::uintmax_t FileLimit = 256 * 1024 * 1024;
std::mutex authoringMutex;
void Check(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
std::string Key(const fs::path& p) {
    auto s = ToGenericUtf8(fs::absolute(p).lexically_normal());
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
bool Inside(const fs::path& p, const fs::path& root) {
    if (root.empty()) return false;
    const auto a = Key(p), b = Key(root);
    return a == b || a.starts_with(b + "/");
}
bool SafeSegment(std::string_view s) {
    if (s.empty() || s == "." || s == ".." || s.back() == '.' || s.back() == ' ') return false;
    for (unsigned char c : s) if (c < 32 || c == ':' || c == '\\' || c == '/' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return false;
    auto stem = std::string(s.substr(0, s.find('.')));
    for (auto& c : stem) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return stem != "CON" && stem != "PRN" && stem != "AUX" && stem != "NUL" &&
        !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' && stem[3] <= '9');
}
void SafeRelative(const fs::path& p) {
    Check(!p.empty() && !p.is_absolute() && !p.has_root_name(), "expected a safe relative rig path");
    for (const auto& part : p) Check(SafeSegment(ToUtf8(part)), "unsafe rig path: " + ToGenericUtf8(p));
}
void NoLinks(const fs::path& path) {
    auto p = fs::absolute(path).lexically_normal();
    for (; !p.empty(); p = p.parent_path()) {
        std::error_code ec;
        const auto status = fs::symlink_status(p, ec);
        Check(!fs::is_symlink(status), "symlink in rig path: " + ToUtf8(p));
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(p.c_str());
        Check(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "reparse point in rig path: " + ToUtf8(p));
#endif
        Check(!ec || ec == std::errc::no_such_file_or_directory, "cannot inspect rig path: " + ToUtf8(p));
        if (p == p.parent_path()) break;
    }
}
fs::path Directory(const RigAuthoringContext& c, const fs::path& path) {
    Check(!c.gameRoot.empty(), "game root is required");
    Check(!path.empty(), "rig directory is required");
    for (const auto& part : path) Check(part != "." && part != "..", "relative traversal in rig directory");
    const auto p = fs::absolute(path.is_absolute() ? path : c.gameRoot / path).lexically_normal();
    Check(Inside(p, c.gameRoot) && Key(p) != Key(c.gameRoot), "rig directory must be beneath gameRoot");
    SafeRelative(p.lexically_relative(fs::absolute(c.gameRoot).lexically_normal()));
    Check(c.engineRoot.empty() || !Inside(p, c.engineRoot), "rig directory overlaps engine root");
    NoLinks(p);
    return p;
}
std::string Logical(const RigAuthoringContext& c, const fs::path& p) {
    return "game/" + ToGenericUtf8(p.lexically_relative(fs::absolute(c.gameRoot).lexically_normal()));
}
std::string Read(const fs::path& p) {
    NoLinks(p);
    Check(fs::is_regular_file(p), "expected regular file: " + ToUtf8(p));
    std::vector<std::uint8_t> bytes; std::string error;
    Check(ReadBinaryFile(p, bytes, error, FileLimit), error);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
Tree Snapshot(const fs::path& root) {
    NoLinks(root);
    Check(fs::is_directory(root), "rig directory does not exist");
    Tree tree;
    std::set<std::string> keys;
    std::uintmax_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        NoLinks(entry.path());
        Check(entry.is_directory() || entry.is_regular_file(), "unsupported file in rig directory");
        const auto relative = entry.path().lexically_relative(root);
        SafeRelative(relative);
        auto key = ToGenericUtf8(relative);
        auto folded = key;
        for (auto& ch : folded) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        Check(keys.insert(folded).second, "case-insensitive rig path collision");
        if (entry.is_regular_file()) {
            auto bytes = Read(entry.path()); total += bytes.size();
            Check(total <= 1024ull * 1024 * 1024, "rig directory snapshot exceeds 1 GiB");
            tree.emplace(key, std::move(bytes));
        } else tree.emplace(key + "/", std::string{});
    }
    return tree;
}
std::string Revision(const Tree& tree) {
    std::string hashes;
    for (const auto& [path, bytes] : tree) {
        hashes += std::to_string(path.size()) + ":" + path + ":" + std::to_string(bytes.size()) + ":" + detail::RigDigest(bytes) + "\n";
    }
    return detail::RigDigest(hashes);
}
Json Parse(std::string_view text) {
    Check(text.size() <= 16 * 1024 * 1024, "rig JSON exceeds size limit");
    std::vector<std::set<std::string>> fields;
    return Json::parse(text.begin(), text.end(), [&fields](int depth, Json::parse_event_t event, Json& value) {
        Check(depth <= 64, "rig JSON nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) fields.emplace_back();
        if (event == Json::parse_event_t::key) Check(fields.back().insert(value.get<std::string>()).second, "duplicate rig JSON field");
        if (event == Json::parse_event_t::object_end) fields.pop_back();
        return true;
    });
}
Json RefJson(const AssetReference& r) { return {{"id", r.id.ToString()}, {"path", r.path}}; }
AssetReference Ref(const Json& j) {
    AssetReference r; std::string error;
    Check(ParseAssetReference(j.dump(), r, error), error); return r;
}
AssetCatalog Catalog(const RigAuthoringContext& c) {
    AssetCatalog catalog; std::string error;
    const bool scanned = catalog.Scan(c.gameRoot, c.engineRoot, error);
    // Scan publishes usable entries when only unrelated sidecars are missing.
    // Owned sources are checked individually below; malformed/duplicate metadata
    // remains fatal and must never be mistaken for an empty catalog.
    Check(scanned || error.starts_with("missing metadata: "), error); return catalog;
}
std::string Relative(const RigAuthoringContext& c, const RigAssetGroup& g, const AssetReference& r) {
    const auto prefix = Logical(c, g.directory) + "/";
    Check(r.id.IsValid() && r.path.starts_with(prefix), "owned rig reference must point inside its directory");
    const auto relative = r.path.substr(prefix.size());
    SafeRelative(FromUtf8(relative));
    return relative;
}
AssetReference MakeRef(const RigAuthoringContext& c, const fs::path& directory, std::string_view filename) {
    return {NewUuid(), Logical(c, directory / FromUtf8(filename))};
}
Json Manifest(const RigAuthoringContext& c, const RigAssetGroup& g) {
    Json j = {{"version", 1}, {"source", RefJson(g.source)}, {"skeleton", RefJson(g.skeleton)},
        {"mesh", RefJson(g.mesh)}, {"placementScale", g.placementScale}, {"clips", Json::array()},
        {"texture", g.texture ? RefJson(*g.texture) : Json(nullptr)},
        {"materialInstance", g.materialInstance ? RefJson(*g.materialInstance) : Json(nullptr)}};
    for (const auto& clip : g.clips) j["clips"].push_back({{"asset", RefJson(clip.asset)}, {"sourceName", clip.sourceName}, {"removed", clip.removed},
        {"relativePath", Relative(c, g, clip.asset)}});
    return j;
}
RigAssetGroup ManifestGroup(const RigAuthoringContext& c, const Json& j, const fs::path& directory) {
    Check(j.is_object() && j.size() == 8 && j.at("version").is_number_integer() && j.at("version") == 1, "invalid rig manifest version or fields");
    RigAssetGroup g; g.directory = directory;
    g.source = Ref(j.at("source")); g.skeleton = Ref(j.at("skeleton")); g.mesh = Ref(j.at("mesh"));
    if (!j.at("texture").is_null()) g.texture = Ref(j.at("texture"));
    if (!j.at("materialInstance").is_null()) g.materialInstance = Ref(j.at("materialInstance"));
    g.placementScale = j.at("placementScale").get<float>();
    Check(j.at("clips").is_array(), "rig clips must be an array");
    for (const auto& clip : j.at("clips")) {
        Check(clip.is_object() && (clip.size() == 3 || (clip.size() == 4 && clip.contains("relativePath"))), "invalid rig clip binding");
        g.clips.push_back({Ref(clip.at("asset")), clip.at("sourceName").get<std::string>(), clip.at("removed").get<bool>()});
        if (g.clips.back().removed && clip.contains("relativePath")) {
            const auto relative = FromUtf8(clip.at("relativePath").get<std::string>());
            SafeRelative(relative);
            g.clips.back().asset.path = Logical(c, directory / relative);
        }
    }
    return g;
}
template<class T> T Definition(const Tree& tree, const std::string& filename) {
    T definition; std::string error;
    const auto& text = tree.at(filename);
    if constexpr (std::is_same_v<T, SkeletonDefinition>) Check(ParseSkeletonDefinition(text, definition, error), error);
    else if constexpr (std::is_same_v<T, SkinnedMeshDefinition>) Check(ParseSkinnedMeshDefinition(text, definition, error), error);
    else if constexpr (std::is_same_v<T, AnimationClipDefinition>) Check(ParseAnimationClipDefinition(text, definition, error), error);
    else Check(ParseMaterialInstanceDefinition(text, definition, error), error);
    return definition;
}
void ValidateExternal(const AssetCatalog& catalog, const AssetReference& r, CatalogAssetKind kind) {
    const auto* entry = catalog.Find(r.id, kind);
    Check(entry != nullptr, "missing or wrong-kind asset reference: " + r.id.ToString());
    NoLinks(entry->source);
}
// Validate a staged graph with its final logical paths, without changing resolver roots.
void Validate(const RigAuthoringContext& c, const Tree& tree, RigAssetGroup& g, const AssetCatalog& catalog, bool published, bool validateSource = true) {
    Check(std::isfinite(g.placementScale) && g.placementScale > 0, "placement scale must be finite and positive");
    std::set<AssetUuid> ids;
    std::set<std::string> paths;
    const auto asset = [&](const AssetReference& r, CatalogAssetKind kind, bool removed = false) {
        const auto path = Relative(c, g, r);
        auto folded = path;
        for (auto& ch : folded) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        Check(ids.insert(r.id).second && paths.insert(folded).second, "duplicate rig identity or path");
        Check(CatalogKindForFile(FromUtf8(path)) == kind, "wrong rig filename asset kind");
        if (removed) {
            Check(!tree.contains(path) && !tree.contains(path + ".meta"), "removed rig clip path has been reused");
            for (const auto& [name, bytes] : tree) {
                auto key = name;
                if (key.ends_with('/')) key.pop_back();
                for (auto& ch : key) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
                Check(key != folded && key != folded + ".meta", "removed rig clip path has been reused");
            }
            const auto* existing = catalog.Find(r.id);
            Check(!existing || (!published && existing->path == r.path), "removed rig clip identity has been reused");
            return path;
        }
        Check(tree.contains(path) && tree.contains(path + ".meta"), "missing rig asset or sidecar: " + path);
        AssetMetadata metadata; std::string error;
        Check(ParseAssetMetadata(tree.at(path + ".meta"), metadata, error), error);
        Check(metadata.id == r.id && metadata.kind == kind, "rig sidecar identity/kind mismatch");
        if (kind == CatalogAssetKind::Mesh) Check(!metadata.centerOnLoad, "skeletal source requires centerOnLoad=false");
        if (const auto* existing = catalog.Find(r.id)) Check(existing->path == r.path, "rig UUID collides with another asset");
        if (const auto* existing = catalog.FindPath(r.path)) Check(existing->metadata.id == r.id, "rig asset path collision");
        if (published) ValidateExternal(catalog, r, kind);
        return path;
    };
    const auto source = asset(g.source, CatalogAssetKind::Mesh);
    auto sourceExtension = ToUtf8(FromUtf8(source).extension());
    for (auto& ch : sourceExtension) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    Check(sourceExtension == ".glb", "rig source must be GLB");
    const auto skeleton = asset(g.skeleton, CatalogAssetKind::Skeleton);
    const auto mesh = asset(g.mesh, CatalogAssetKind::SkinnedMesh);
    Check(Definition<SkeletonDefinition>(tree, skeleton).source.id == g.source.id, "rig skeleton source mismatch");
    Check(Definition<SkinnedMeshDefinition>(tree, mesh).skeleton.id == g.skeleton.id, "rig mesh skeleton mismatch");
    detail::RigProbe probe; std::string error;
    const auto& bytes = tree.at(source);
    if (validateSource) {
        Check(detail::ProbeRig(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), probe, error), error);
        Check(probe.inspection.importable, probe.inspection.diagnostics.empty() ? "unsupported rig source" : probe.inspection.diagnostics.front());
    }
    std::set<std::string> names;
    for (const auto& clip : g.clips) {
        const auto path = asset(clip.asset, CatalogAssetKind::AnimationClip, clip.removed);
        Check(!clip.sourceName.empty() && clip.sourceName.size() <= 1024, "invalid rig clip name");
        if (clip.removed) continue;
        Check(names.insert(clip.sourceName).second, "duplicate active rig clip mapping");
        const auto d = Definition<AnimationClipDefinition>(tree, path);
        Check(d.skeleton.id == g.skeleton.id && d.clip == clip.sourceName, "rig animation descriptor mismatch");
        if (validateSource) {
            const auto found = std::find_if(probe.inspection.clips.begin(), probe.inspection.clips.end(), [&](const auto& info) { return info.name == clip.sourceName; });
            Check(found != probe.inspection.clips.end() && found->importable, "unimportable selected clip: " + clip.sourceName);
        }
    }
    if (g.texture) asset(*g.texture, CatalogAssetKind::Texture);
    if (g.materialInstance) {
        const auto path = asset(*g.materialInstance, CatalogAssetKind::MaterialInstance);
        const auto material = Definition<MaterialInstanceDefinition>(tree, path);
        if (material.materialReference) ValidateExternal(catalog, *material.materialReference, CatalogAssetKind::Material);
        else {
            const auto* entry = catalog.FindPath(material.material);
            Check(entry && entry->metadata.kind == CatalogAssetKind::Material, "missing base material");
        }
        if (material.parameters.albedoReference && (!g.texture || material.parameters.albedoReference->id != g.texture->id))
            ValidateExternal(catalog, *material.parameters.albedoReference, CatalogAssetKind::Texture);
        if (material.parameters.albedoTexture && !material.parameters.albedoTexture->empty() && !material.parameters.albedoReference) {
            const auto* entry = catalog.FindPath(*material.parameters.albedoTexture);
            Check((g.texture && *material.parameters.albedoTexture == g.texture->path) || (entry && entry->metadata.kind == CatalogAssetKind::Texture), "missing material texture");
        }
    }
    g.revision = Revision(tree);
}
RigAssetGroup ReadGroup(const RigAuthoringContext& c, const fs::path& dir, const Tree& tree, const AssetCatalog& catalog) {
    RigAssetGroup g; g.directory = dir;
    if (tree.contains("rig.import.json")) g = ManifestGroup(c, Parse(tree.at("rig.import.json")), dir);
    else {
        std::vector<std::pair<std::string, AssetReference>> skeletons, meshes, clips;
        for (const auto& [path, text] : tree) {
            const auto kind = CatalogKindForFile(FromUtf8(path));
            if (!kind || (*kind != CatalogAssetKind::Skeleton && *kind != CatalogAssetKind::SkinnedMesh && *kind != CatalogAssetKind::AnimationClip)) continue;
            AssetMetadata meta; std::string error;
            Check(tree.contains(path + ".meta") && ParseAssetMetadata(tree.at(path + ".meta"), meta, error), "invalid or missing sidecar: " + path + " " + error);
            AssetReference ref{meta.id, Logical(c, dir / FromUtf8(path))};
            if (*kind == CatalogAssetKind::Skeleton) skeletons.emplace_back(path, ref);
            if (*kind == CatalogAssetKind::SkinnedMesh) meshes.emplace_back(path, ref);
            if (*kind == CatalogAssetKind::AnimationClip) clips.emplace_back(path, ref);
        }
        Check(skeletons.size() == 1, "prepared rig must have one unambiguous skeleton");
        g.skeleton = skeletons.front().second;
        g.source = Definition<SkeletonDefinition>(tree, skeletons.front().first).source;
        std::vector<AssetReference> matchingMeshes;
        for (const auto& [path, ref] : meshes) if (Definition<SkinnedMeshDefinition>(tree, path).skeleton.id == g.skeleton.id) matchingMeshes.push_back(ref);
        Check(matchingMeshes.size() == 1, "prepared rig must have one unambiguous mesh");
        g.mesh = matchingMeshes.front();
        for (const auto& [path, ref] : clips) {
            const auto clip = Definition<AnimationClipDefinition>(tree, path);
            if (clip.skeleton.id == g.skeleton.id) g.clips.push_back({ref, clip.clip, false});
        }
        // Runtime descriptors do not bind a material to this rig. Even a sole
        // neighbouring instance/texture pair is not evidence of rig ownership.
    }
    // Resolve active identities before using path hints for physical ownership.
    // This allows asset/folder moves without rewriting every descriptor hint.
    // Older manifests had no relative tombstone paths. Recover their old group
    // prefix only when surviving UUIDs establish an unambiguous folder move.
    std::vector<AssetReference> active{g.source, g.skeleton, g.mesh};
    if (g.texture) active.push_back(*g.texture);
    if (g.materialInstance) active.push_back(*g.materialInstance);
    for (const auto& clip : g.clips) if (!clip.removed) active.push_back(clip.asset);
    for (auto& clip : g.clips) if (clip.removed && !clip.asset.path.starts_with(Logical(c, dir) + "/")) {
        std::set<std::string> candidates;
        for (const auto& reference : active) {
            const auto* entry = catalog.Find(reference.id);
            if (!entry || !Inside(entry->source, dir)) continue;
            const auto suffix = "/" + ToGenericUtf8(entry->source.lexically_relative(dir));
            if (!reference.path.ends_with(suffix)) continue;
            const auto oldPrefix = reference.path.substr(0, reference.path.size() - suffix.size()) + "/";
            if (!clip.asset.path.starts_with(oldPrefix)) continue;
            const auto relative = FromUtf8(clip.asset.path.substr(oldPrefix.size()));
            SafeRelative(relative);
            candidates.insert(Logical(c, dir / relative));
        }
        Check(candidates.size() == 1, "cannot locate legacy tombstone after ambiguous rig move");
        clip.asset.path = *candidates.begin();
    }
    const auto resolve = [&](AssetReference& reference) {
        const auto* entry = catalog.Find(reference.id);
        Check(entry != nullptr, "missing owned rig UUID: " + reference.id.ToString());
        Check(Inside(entry->source, dir), "owned rig asset moved outside its directory");
        reference.path = entry->path;
    };
    resolve(g.source); resolve(g.skeleton); resolve(g.mesh);
    if (g.texture) resolve(*g.texture);
    if (g.materialInstance) resolve(*g.materialInstance);
    for (auto& clip : g.clips) if (!clip.removed) resolve(clip.asset);
    // Reading is structural: an edited/broken GLB must remain repairable using
    // a replacement source and explicit remap/remove decisions. Publish validates
    // the complete replacement source and every selected clip.
    Validate(c, tree, g, catalog, true, false);
    return g;
}
void PutMetadata(Tree& tree, const std::string& path, const AssetReference& ref, CatalogAssetKind kind) {
    std::string text, error;
    Check(SerializeAssetMetadata({ref.id, kind, false}, text, error), error);
    tree[path + ".meta"] = std::move(text);
}
template<class T> void PutDefinition(Tree& tree, const std::string& path, const T& value) {
    std::string text, error;
    if constexpr (std::is_same_v<T, SkeletonDefinition>) Check(SerializeSkeletonDefinition(value, text, error), error);
    else if constexpr (std::is_same_v<T, SkinnedMeshDefinition>) Check(SerializeSkinnedMeshDefinition(value, text, error), error);
    else if constexpr (std::is_same_v<T, AnimationClipDefinition>) Check(SerializeAnimationClipDefinition(value, text, error), error);
    else Check(SerializeMaterialInstanceDefinition(value, text, error), error);
    tree[path] = std::move(text);
}
void PutGraph(const RigAuthoringContext& c, Tree& tree, const RigAssetGroup& g) {
    const auto source = Relative(c, g, g.source), skeleton = Relative(c, g, g.skeleton), mesh = Relative(c, g, g.mesh);
    PutMetadata(tree, source, g.source, CatalogAssetKind::Mesh);
    PutDefinition(tree, skeleton, SkeletonDefinition{g.source}); PutMetadata(tree, skeleton, g.skeleton, CatalogAssetKind::Skeleton);
    PutDefinition(tree, mesh, SkinnedMeshDefinition{g.skeleton}); PutMetadata(tree, mesh, g.mesh, CatalogAssetKind::SkinnedMesh);
    for (const auto& clip : g.clips) {
        const auto path = Relative(c, g, clip.asset);
        if (clip.removed) { tree.erase(path); tree.erase(path + ".meta"); }
        else { PutDefinition(tree, path, AnimationClipDefinition{g.skeleton, clip.sourceName}); PutMetadata(tree, path, clip.asset, CatalogAssetKind::AnimationClip); }
    }
    tree["rig.import.json"] = Manifest(c, g).dump(2) + "\n";
}
std::string ClipFilename(const Tree& tree, const RigAssetGroup& g, std::string_view name) {
    std::string stem;
    for (unsigned char c : name) { if (stem.size() >= 60) break; stem += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? char(c) : '_'; }
    if (!SafeSegment(stem)) stem = "clip";
    for (unsigned i = 0;; ++i) {
        const auto filename = stem + (i ? "-" + std::to_string(i) : "") + ".animation.json";
        auto folded = filename; for (auto& c : folded) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        const auto equals = [&](std::string s) { if (s.ends_with('/')) s.pop_back(); for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s == folded || s == folded + ".meta"; };
        bool used = false;
        for (const auto& [path, bytes] : tree) if (equals(path)) used = true;
        for (const auto& clip : g.clips) if (equals(ToUtf8(FromUtf8(clip.asset.path).filename()))) used = true;
        if (!used) return filename;
    }
}
detail::RigProbe Source(const fs::path& path, std::string_view expected, std::string& bytes) {
    Check(!expected.empty(), "expected source revision is required");
    bytes = Read(path);
    detail::RigProbe probe; std::string error;
    Check(detail::ProbeRig(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), probe, error), error);
    Check(probe.inspection.sourceRevision == expected, "source revision changed; inspect again");
    Check(probe.inspection.importable, probe.inspection.diagnostics.empty() ? "unsupported rig source" : probe.inspection.diagnostics.front());
    return probe;
}
fs::path TransactionRoot(const RigAuthoringContext& c, const fs::path& dest) {
    Check(!c.transactionRoot.empty(), "transaction root is required");
    const auto root = fs::absolute(c.transactionRoot).lexically_normal();
    Check(!Inside(root, c.gameRoot) && !Inside(c.gameRoot, root) &&
        (c.engineRoot.empty() || (!Inside(root, c.engineRoot) && !Inside(c.engineRoot, root))), "transaction root must be outside scanned roots");
    Check(Key(root.root_path()) == Key(dest.root_path()), "transaction root must be on destination volume");
    NoLinks(root); fs::create_directories(root); NoLinks(root);
    return root;
}
// Journal survives publication failure; backup is never discarded if restoration fails.
RigPublishResult Publish(const RigAuthoringContext& c, RigAssetGroup g, const Tree& tree,
                         const std::optional<Tree>& previous, const fs::path& watchedDirectory,
                         const std::optional<Tree>& watchedTree, const fs::path& sourcePath,
                         const std::string& sourceBytes) {
    RigPublishResult result;
    fs::path transaction;
    bool backedUp = false, published = false;
    try {
        auto catalog = Catalog(c);
        Validate(c, tree, g, catalog, false);
        const auto candidate = TransactionRoot(c, g.directory) / FromUtf8(NewUuid());
        Check(fs::create_directory(candidate), "transaction directory already exists");
        transaction = candidate;
        Check(fs::create_directory(transaction / "staged"), "staging directory already exists");
        const auto journal = [&](std::string_view phase) {
            const auto text = Json{{"version", 1}, {"phase", phase}, {"destination", ToGenericUtf8(g.directory)},
                {"previousRevision", previous ? Revision(*previous) : ""}, {"newRevision", Revision(tree)}}.dump(2);
            std::string error; Check(WriteFileAtomic(transaction / "journal.json", text, error), error);
        };
        journal("staging");
        for (const auto& [path, bytes] : tree) {
            const auto target = transaction / "staged" / FromUtf8(path);
            if (path.ends_with('/')) { fs::create_directories(target); continue; }
            fs::create_directories(target.parent_path());
            std::string error; Check(WriteFileAtomic(target, bytes, error), error);
        }
        Check(Snapshot(transaction / "staged") == tree, "staging verification failed");
        Validate(c, tree, g, catalog, false);
        journal("prepared");
        NoLinks(g.directory);
        Check(fs::is_directory(g.directory.parent_path()), "destination parent does not exist");
        if (previous) Check(Snapshot(g.directory) == *previous, "group revision changed before publication");
        else Check(!fs::exists(g.directory) && !fs::exists(AssetMetadataPath(g.directory)), "destination already exists");
        if (watchedTree) Check(Snapshot(watchedDirectory) == *watchedTree, "original rig changed before publication");
        if (!sourcePath.empty()) Check(Read(sourcePath) == sourceBytes, "source changed before publication");
        // Refresh collision validation against external edits immediately before renames.
        catalog = Catalog(c); Validate(c, tree, g, catalog, false);
        if (previous) {
            fs::rename(g.directory, transaction / "backup"); backedUp = true;
            Check(Snapshot(transaction / "backup") == *previous, "group changed during publication; restoring latest tree");
        }
        journal("publishing");
        Check(Snapshot(transaction / "staged") == tree, "staged files changed before publication");
        fs::rename(transaction / "staged", g.directory); published = true;
        // The directory rename is the commit point. A journal/cleanup failure
        // afterwards must never roll back or delete a newly visible edited tree.
        try { journal("committed"); }
        catch (const std::exception& e) {
            result.state = RigPublishState::Committed; result.group = g;
            result.warnings.push_back("Committed; journal update failed: " + std::string(e.what()));
            result.recoveryDirectory = transaction; return result;
        }
        if (previous && Snapshot(transaction / "backup") != *previous) {
            result.state = RigPublishState::Committed; result.group = g;
            result.warnings.push_back("Committed; concurrent edits to old files retained in transaction backup");
            result.recoveryDirectory = transaction; return result;
        }
        result.state = RigPublishState::Committed; result.group = std::move(g);
        std::error_code ec; fs::remove_all(transaction, ec);
        if (ec) { result.warnings.push_back("Committed; transaction cleanup failed: " + ec.message()); result.recoveryDirectory = transaction; }
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
        if (published) {
            result.state = RigPublishState::Committed; result.group = g;
            result.warnings.push_back("Committed; post-publication check failed: " + result.error);
            result.error.clear(); result.recoveryDirectory = transaction; return result;
        }
        try {
            if (backedUp) fs::rename(transaction / "backup", g.directory);
        } catch (const std::exception& rollback) {
            result.state = RigPublishState::RecoveryRequired; result.recoveryDirectory = transaction;
            result.error += "; rollback failed: " + std::string(rollback.what()); return result;
        }
        if (!transaction.empty()) { std::error_code ec; fs::remove_all(transaction, ec); if (ec) result.recoveryDirectory = transaction; }
        return result;
    }
}
}

bool ReadRigAssetGroup(const RigAuthoringContext& c, const fs::path& directory, RigAssetGroup& result, std::string& error) {
    try {
        const auto dir = Directory(c, directory);
        const auto tree = Snapshot(dir);
        auto group = ReadGroup(c, dir, tree, Catalog(c));
        Check(Snapshot(dir) == tree, "rig changed while reading");
        result = std::move(group); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

RigPublishResult ImportAnimatedRig(const RigAuthoringContext& c, const RigImportRequest& request) {
    std::lock_guard lock(authoringMutex);
    try {
        RigAssetGroup g; g.directory = Directory(c, request.destinationDirectory); g.placementScale = request.placementScale;
        Check(!fs::exists(g.directory), "destination already exists");
        std::string bytes; const auto probe = Source(request.sourceGlb, request.expectedSourceRevision, bytes);
        g.source = MakeRef(c, g.directory, "model.glb"); g.skeleton = MakeRef(c, g.directory, "model.skeleton.json"); g.mesh = MakeRef(c, g.directory, "model.skinned-mesh.json");
        Tree tree{{"model.glb", bytes}};
        for (const auto& name : request.clips) g.clips.push_back({MakeRef(c, g.directory, ClipFilename(tree, g, name)), name, false});
        if (request.extractBaseColor && probe.inspection.canExtractBaseColor) {
            const auto filename = "base-color" + probe.textureExtension;
            g.texture = MakeRef(c, g.directory, filename);
            tree[filename] = {reinterpret_cast<const char*>(probe.baseColor.data()), probe.baseColor.size()};
            PutMetadata(tree, filename, *g.texture, CatalogAssetKind::Texture);
        }
        if (request.baseMaterial) {
            ValidateExternal(Catalog(c), *request.baseMaterial, CatalogAssetKind::Material);
            MaterialInstanceDefinition m; m.version = 2; m.materialReference = request.baseMaterial; m.material = request.baseMaterial->path;
            if (g.texture) {
                m.parameters.albedoReference = g.texture; m.parameters.albedoTexture = g.texture->path;
            }
            g.materialInstance = MakeRef(c, g.directory, "model.material-instance.json");
            PutDefinition(tree, "model.material-instance.json", m); PutMetadata(tree, "model.material-instance.json", *g.materialInstance, CatalogAssetKind::MaterialInstance);
        }
        PutGraph(c, tree, g);
        return Publish(c, std::move(g), tree, {}, {}, {}, request.sourceGlb, bytes);
    } catch (const std::exception& e) { RigPublishResult result; result.error = e.what(); return result; }
}

RigPublishResult ReimportAnimatedRig(const RigAuthoringContext& c, const RigReimportRequest& request) {
    std::lock_guard lock(authoringMutex);
    try {
        const auto dir = Directory(c, request.directory);
        const auto previous = Snapshot(dir); auto g = ReadGroup(c, dir, previous, Catalog(c));
        Check(!request.expectedGroupRevision.empty() && g.revision == request.expectedGroupRevision, "group revision changed; reload before reimport");
        std::string bytes; const auto probe = Source(request.sourceGlb, request.expectedSourceRevision, bytes);
        Tree tree = previous;
        std::set<AssetUuid> choices;
        for (const auto& mapping : request.clips) {
            if (!mapping.existingId.IsValid()) {
                Check(!mapping.remove, "cannot remove a new clip");
                g.clips.push_back({MakeRef(c, dir, ClipFilename(tree, g, mapping.sourceName)), mapping.sourceName, false});
                continue;
            }
            Check(choices.insert(mapping.existingId).second, "duplicate existing clip mapping");
            auto found = std::find_if(g.clips.begin(), g.clips.end(), [&](const auto& clip) { return clip.asset.id == mapping.existingId; });
            Check(found != g.clips.end(), "unknown clip mapping");
            found->removed = mapping.remove;
            if (!mapping.remove) found->sourceName = mapping.sourceName;
        }
        for (const auto& clip : ReadGroup(c, dir, previous, Catalog(c)).clips)
            Check(clip.removed || choices.contains(clip.asset.id), "every active existing clip needs an explicit mapping");
        tree[Relative(c, g, g.source)] = bytes;
        if (request.refreshTexture && g.texture) {
            Check(probe.inspection.canExtractBaseColor, "replacement source has no extractable base color");
            const auto filename = Relative(c, g, *g.texture);
            const auto extension = ToUtf8(FromUtf8(filename).extension());
            Check(extension == probe.textureExtension || (extension == ".jpeg" && probe.textureExtension == ".jpg"), "replacement texture format differs from stable owned filename");
            tree[filename] = {reinterpret_cast<const char*>(probe.baseColor.data()), probe.baseColor.size()};
        }
        PutGraph(c, tree, g);
        return Publish(c, std::move(g), tree, previous, {}, {}, request.sourceGlb, bytes);
    } catch (const std::exception& e) { RigPublishResult result; result.error = e.what(); return result; }
}

RigPublishResult DuplicateAnimatedRig(const RigAuthoringContext& c, const fs::path& directory,
                                     const fs::path& destinationDirectory, std::string_view expectedGroupRevision) {
    std::lock_guard lock(authoringMutex);
    try {
        const auto dir = Directory(c, directory), dest = Directory(c, destinationDirectory);
        Check(!Inside(dest, dir) && !Inside(dir, dest), "duplicate directories overlap");
        const auto original = Snapshot(dir); const auto old = ReadGroup(c, dir, original, Catalog(c));
        Check(!expectedGroupRevision.empty() && old.revision == expectedGroupRevision, "group revision changed; reload before duplication");
        RigAssetGroup g = old; g.directory = dest; Tree tree;
        const auto remap = [&](AssetReference& r, CatalogAssetKind kind, bool removed = false) {
            const auto path = Relative(c, old, r); r = MakeRef(c, dest, path);
            if (!removed) { tree[path] = original.at(path); PutMetadata(tree, path, r, kind); }
        };
        remap(g.source, CatalogAssetKind::Mesh); remap(g.skeleton, CatalogAssetKind::Skeleton); remap(g.mesh, CatalogAssetKind::SkinnedMesh);
        for (auto& clip : g.clips) remap(clip.asset, CatalogAssetKind::AnimationClip, clip.removed);
        if (g.texture) remap(*g.texture, CatalogAssetKind::Texture);
        if (g.materialInstance) {
            remap(*g.materialInstance, CatalogAssetKind::MaterialInstance);
            const auto path = Relative(c, g, *g.materialInstance);
            auto material = Definition<MaterialInstanceDefinition>(tree, path);
            if (old.texture && g.texture) {
                if (material.parameters.albedoReference && material.parameters.albedoReference->id == old.texture->id) material.parameters.albedoReference = g.texture;
                if (material.parameters.albedoTexture && *material.parameters.albedoTexture == old.texture->path) material.parameters.albedoTexture = g.texture->path;
            }
            PutDefinition(tree, path, material);
        }
        PutGraph(c, tree, g);
        // Nested prepared descriptors retain their relative filenames. Include
        // their parent directories in the immutable staged snapshot as well.
        std::vector<std::string> directories;
        for (const auto& [path, text] : tree) {
            auto parent = FromUtf8(path).parent_path();
            while (!parent.empty()) { directories.push_back(ToGenericUtf8(parent) + "/"); parent = parent.parent_path(); }
        }
        for (const auto& path : directories) tree.try_emplace(path);
        return Publish(c, std::move(g), tree, {}, dir, original, {}, {});
    } catch (const std::exception& e) { RigPublishResult result; result.error = e.what(); return result; }
}
} // namespace engine
