#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <engine/ai/BehaviorTreeSource.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetCooking.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/Content.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Pak.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/core/Utf.hpp>
#include <engine/navigation/NavigationMesh.hpp>
#include <engine/project/Project.hpp>
#include <engine/project/RuntimeSettings.hpp>
#include <engine/scene/SceneDocument.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

    namespace fs = std::filesystem;

    constexpr const wchar_t* BuildStampName = L".sri-build";
    constexpr std::string_view BuildStampText =
        "Made by sri-build. The next build deletes and replaces this whole folder.\n";

    using Payloads = std::unordered_map<engine::AssetUuid, engine::CpuPayload, engine::AssetUuidHash>;

    struct CookedFile {
        fs::path target; // relative to the output folder
        std::vector<std::uint8_t> bytes;
    };

    struct Cooked {
        std::vector<CookedFile> files;
        std::set<fs::path> sources; // replaced by a cooked file, so not copied
        std::string manifest;
        std::vector<std::uint8_t> shaders;
    };

    struct Options {
        fs::path project;
        fs::path out;
        fs::path runtime;
        bool pack = true;

        engine::RuntimeSettings settings;
    };

    void Info(const std::string& message) {
        std::cout << "info: " << message << '\n';
    }

    void Warning(const std::string& message) {
        std::cout << "warning: " << message << '\n';
    }

    void Error(const std::string& message) {
        std::cout << "error: " << message << '\n';
    }

    bool ParsePositiveInteger(
        std::wstring_view text,
        std::uint32_t& result
    ) {
        const auto value = engine::utf::ToUtf8(text);

        if (!value || value->empty()) {
            return false;
        }

        std::uint32_t parsed = 0;

        const auto [end, ec] = std::from_chars(
            value->data(),
            value->data() + value->size(),
            parsed
        );

        if (ec != std::errc{} ||
            end != value->data() + value->size() ||
            parsed == 0) {
            return false;
        }

        result = parsed;
        return true;
    }

    void PrintUsage() {
        std::cout
            << "Usage:\n"
            << "  sri-build --project <file> --out <dir> "
            << "[--runtime <dir>] [--title T] [--width N] [--height N] "
            << "[--fullscreen] [--capture-mouse] [--no-pak]\n";
    }

    bool ParseOptions(
        int argc,
        wchar_t* argv[],
        Options& options,
        const fs::path& toolRoot,
        std::string& error
    ) {
        options.runtime = toolRoot / L"runtime";

        for (int i = 1; i < argc; ++i) {
            const std::wstring_view argument(argv[i]);

            const auto ReadPathArgument = [&](fs::path& destination) -> bool {
                if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                    error = "Missing value after " + engine::ToUtf8(fs::path(argument));
                    return false;
                }

                destination = fs::path(argv[++i]);
                return true;
                };

            const auto ReadStringArgument = [&](std::string& destination) -> bool {
                if (i + 1 >= argc) {
                    error = "Missing value after " + engine::ToUtf8(fs::path(argument));
                    return false;
                }

                auto text = engine::utf::ToUtf8(std::wstring_view(argv[++i]));

                if (!text) {
                    error = "Could not convert command-line text to UTF-8";
                    return false;
                }

                destination = std::move(*text);
                return true;
                };

            if (argument == L"--project") {
                if (!ReadPathArgument(options.project)) {
                    return false;
                }
            }
            else if (argument == L"--out") {
                if (!ReadPathArgument(options.out)) {
                    return false;
                }
            }
            else if (argument == L"--runtime") {
                if (!ReadPathArgument(options.runtime)) {
                    return false;
                }
            }
            else if (argument == L"--title") {
                if (!ReadStringArgument(options.settings.title)) {
                    return false;
                }
            }
            else if (argument == L"--width") {
                if (i + 1 >= argc ||
                    !ParsePositiveInteger(argv[i + 1], options.settings.width)) {
                    error = "Invalid value for --width";
                    return false;
                }
                ++i;
            }
            else if (argument == L"--height") {
                if (i + 1 >= argc || !ParsePositiveInteger(argv[i + 1], options.settings.height)) {
                    error = "Invalid value for --height";
                    return false;
                }
                ++i;
            }
            else if (argument == L"--fullscreen") {
                options.settings.fullscreen = true;
            }
            else if (argument == L"--capture-mouse") {
                options.settings.captureMouse = true;
            }
            else if (argument == L"--no-pak") {
                options.pack = false;
            }
            else {
                error = "Unknown argument: " + engine::ToUtf8(fs::path(argument));
                return false;
            }
        }

        if (options.project.empty()) {
            error = "Required argument --project was not specified";
            return false;
        }

        if (options.out.empty()) {
            error = "Required argument --out was not specified";
            return false;
        }

        options.project = fs::absolute(options.project).lexically_normal();
        options.out = fs::absolute(options.out).lexically_normal();
        options.runtime = fs::absolute(options.runtime).lexically_normal();

        return true;
    }

    bool IsSameOrInside(const fs::path& candidate, const fs::path& base) {
        const fs::path normalizedCandidate = fs::weakly_canonical(candidate);
        const fs::path normalizedBase = fs::weakly_canonical(base);
        const fs::path relative = normalizedCandidate.lexically_relative(normalizedBase);

        if (relative.empty()) return false;
        const auto first = relative.begin();
        return *first != L"..";
    }

    bool PathsOverlap(const fs::path& first, const fs::path& second) {
        return IsSameOrInside(first, second) || IsSameOrInside(second, first);
    }

    // The engine/ and game/ roots a build ships; engine/ is the one beside sri-build.
    std::array<std::pair<fs::path, const wchar_t*>, 2> ContentRoots(const engine::Project& project) {
        return {{{engine::AssetPathResolver::GetRoot() / L"engine", L"engine"}, {project.ContentDir(), L"game"}}};
    }

    bool ValidateCubemapDds(const fs::path& file, std::string& error) {
        if (engine::NormalizeName(engine::ToUtf8(file.extension())) != ".dds") {
            error = "skybox cubemap source must be DDS";
            return false;
        }
        // DDS is a little-endian file format. Read just the header; no GPU or
        // DirectXTK private headers are needed for dimension/array validation.
        std::ifstream stream(file, std::ios::binary);
        std::array<unsigned char, 148> bytes{};
        stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const auto size = stream.gcount();
        const auto word = [&](std::size_t offset) -> std::uint32_t {
            return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
                (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
        };
        const auto reject = [&] {
            error = "DDS must contain one square six-face Texture2D cubemap (no arrays or volumes)";
            return false;
        };
        if (size < 128 || word(0) != 0x20534444 || word(4) != 124 || word(76) != 32 ||
            word(12) == 0 || word(12) != word(16) || word(24) > 1 ||
            (word(8) & 0x800000) || (word(112) & 0x200000)) return reject();
        const bool dx10 = (word(80) & 4) && word(84) == 0x30315844;
        if (dx10) {
            // D3D10_RESOURCE_DIMENSION_TEXTURE2D, TEXTURECUBE misc flag,
            // and one cube, not six cubes (DX10 arraySize counts cubes).
            if (size < 148 || word(132) != 3 || !(word(136) & 4) || word(140) != 1) return reject();
        } else if ((word(112) & 0xfe00) != 0xfe00) {
            return reject(); // legacy DDS must declare the cube and all six faces
        }
        if (fs::file_size(file) <= (dx10 ? 148u : 128u)) {
            error = "DDS cubemap has no image data";
            return false;
        }
        error.clear();
        return true;
    }

    // Skeletal payloads are kept from validation so the cook does not read their sources again.
    bool Validate(
        const engine::Project& project,
        engine::AssetCatalog& catalog,
        Payloads& payloads,
        std::vector<std::string>& errors,
        std::vector<std::string>& warnings
    ) {
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());

        std::string error;

        const fs::path engineRoot =
            engine::AssetPathResolver::GetRoot() / L"engine";

        if (!catalog.Scan(project.ContentDir(), engineRoot, error)) {
            errors.push_back("asset catalog: " + error);
            return false;
        }

        const auto* start = catalog.FindPath(project.startupScene);

        if (!start ||
            start->metadata.kind != engine::CatalogAssetKind::Scene) {
            errors.push_back("startup scene '" + project.startupScene + "' is not in the catalog");
        }

        using Signatures = std::unordered_map<engine::AssetUuid, std::uint64_t, engine::AssetUuidHash>;
        Signatures skeletons, clips, meshes;
        const auto validateSkeletal = [&](engine::CatalogAssetKind catalogKind,
                                          engine::AssetKind assetKind, Signatures& signatures) {
            engine::SkeletalAssetImporter importer(assetKind);
            for (const auto* entry : catalog.List(catalogKind)) {
                engine::AssetDesc desc;
                desc.name = entry->path;
                desc.path = entry->source;
                desc.catalog = &catalog;
                desc.centerOnLoad = entry->metadata.centerOnLoad;
                engine::CpuPayload payload;
                error.clear();
                if (!importer.ReadCpu(desc, nullptr, payload, error)) {
                    errors.push_back(entry->path + ": " + error);
                    continue;
                }
                const auto signature = assetKind == engine::AssetKind::Skeleton ? payload.skeleton.signature :
                    assetKind == engine::AssetKind::AnimationClip ? payload.animationClip.skeletonSignature :
                    payload.skinnedMesh.skeletonSignature;
                signatures.emplace(entry->metadata.id, signature);
                payloads.emplace(entry->metadata.id, std::move(payload));
            }
        };
        validateSkeletal(engine::CatalogAssetKind::Skeleton, engine::AssetKind::Skeleton, skeletons);
        validateSkeletal(engine::CatalogAssetKind::AnimationClip, engine::AssetKind::AnimationClip, clips);
        validateSkeletal(engine::CatalogAssetKind::SkinnedMesh, engine::AssetKind::SkinnedMesh, meshes);

        for (const auto* entry : catalog.List(engine::CatalogAssetKind::Skybox)) {
            engine::CpuPayload payload;
            engine::AssetDesc desc;
            desc.name = entry->path;
            desc.path = entry->source;
            if (!engine::SkyboxImporter{}.ReadCpu(desc, nullptr, payload, error)) {
                errors.push_back(entry->path + ": " + error);
                continue;
            }
            const auto& reference = payload.skybox.cubemap;
            const auto* texture = catalog.Find(reference.id, engine::CatalogAssetKind::Texture);
            if (!texture) {
                errors.push_back(entry->path + ": cubemap references missing or wrong-kind asset " +
                    reference.id.ToString() + " (" + reference.path + ")");
            } else if (!ValidateCubemapDds(texture->source, error)) {
                errors.push_back(entry->path + ": cubemap " + texture->path + ": " + error);
            }
        }

        for (const auto* entry : catalog.List(engine::CatalogAssetKind::ParticleEffect)) {
            engine::CpuPayload payload;
            engine::AssetDesc desc;
            desc.name = entry->path;
            desc.path = entry->source;
            error.clear();
            if (!engine::ParticleEffectImporter{}.ReadCpu(desc, nullptr, payload, error)) {
                errors.push_back(entry->path + ": " + error);
                continue;
            }
            const auto& texture = payload.particleEffect.texture;
            if (texture.id.IsValid() && !catalog.Find(texture.id, engine::CatalogAssetKind::Texture))
                errors.push_back(entry->path + ": texture references missing or wrong-kind asset " +
                    texture.id.ToString() + " (" + texture.path + ")");
        }

        for (const auto* mesh : catalog.List(engine::CatalogAssetKind::Mesh)) {
            for (std::size_t slot = 0; slot < mesh->metadata.materials.size(); ++slot) {
                const auto& material = mesh->metadata.materials[slot];
                if (material && !catalog.Find(material->id, engine::CatalogAssetKind::MaterialInstance)) {
                    errors.push_back(mesh->path + ": material slot " + std::to_string(slot) +
                        " references missing or wrong-kind asset " + material->id.ToString() +
                        (material->path.empty() ? "" : " (" + material->path + ")"));
                }
            }
        }

        for (const auto* tree : catalog.List(engine::CatalogAssetKind::BehaviorTree)) {
            std::string text;
            engine::ai::TreeSource source;
            error.clear();
            if (!engine::ReadTextFile(tree->source, text, error) || !engine::ai::ReadTreeSource(text, source, error) ||
                !engine::ai::ValidateTreeSource(source, catalog, error)) {
                errors.push_back(tree->path + ": " + error);
            }
        }

        for (const auto* scene :
            catalog.List(engine::CatalogAssetKind::Scene)) {

            std::string text;
            engine::SceneDocument document;

            error.clear();

            if (!engine::ReadTextFile(scene->source, text, error) ||
                !engine::ParseSceneDocument(text, document, error)) {

                errors.push_back(scene->path + ": " + error);
                continue;
            }
            if (scene == start && !document.activeCamera) {
                warnings.push_back(scene->path + ": no active camera, the game would show nothing");
            }
            const auto& skybox = document.environment.skybox;
            if (skybox.id.IsValid() && !catalog.Find(skybox.id, engine::CatalogAssetKind::Skybox)) {
                errors.push_back(scene->path + ": environment.skybox references missing or wrong-kind asset " +
                    skybox.id.ToString() + " (" + skybox.path + ")");
            }
            for (const auto& entity : document.entities) {
                const std::string context = scene->path + ": entity '" + entity.name + "' (" +
                    entity.id.ToString() + "): ";
                const auto checkReference = [&](const engine::AssetReference& reference,
                                                engine::CatalogAssetKind kind, const char* field) {
                    if (!reference.id.IsValid()) return;
                    if (!catalog.Find(reference.id, kind)) {
                        errors.push_back(context + field + " references missing or wrong-kind asset " +
                            reference.id.ToString() + (reference.path.empty() ? "" : " (" + reference.path + ")"));
                    }
                };
                if (entity.skinnedRenderer) {
                    checkReference(entity.skinnedRenderer->mesh, engine::CatalogAssetKind::SkinnedMesh,
                                   "SkinnedMeshRenderer.mesh");
                    checkReference(entity.skinnedRenderer->materialInstance, engine::CatalogAssetKind::MaterialInstance,
                                   "SkinnedMeshRenderer.materialInstance");
                }
                if (entity.particleEmitter)
                    checkReference(entity.particleEmitter->effect, engine::CatalogAssetKind::ParticleEffect,
                                   "ParticleEmitter.effect");
                if (entity.behaviorTree) {
                    checkReference(entity.behaviorTree->tree, engine::CatalogAssetKind::BehaviorTree, "BehaviorTree.tree");
                }
                if (entity.navigationRegion) {
                    const auto& bake = entity.navigationRegion->navMesh;
                    engine::NavigationMesh mesh;
                    if (!bake.id.IsValid()) {
                        warnings.push_back(context + "NavigationRegion is not baked, so the game has no NavMesh for it");
                    } else if (!engine::LoadNavigationBake(catalog, bake, entity.id, mesh, error)) {
                        errors.push_back(context + "NavigationRegion.navMesh " + bake.id.ToString() +
                            (bake.path.empty() ? "" : " (" + bake.path + ")") + ": " + error + ", bake it again");
                    }
                }
                if (entity.animator) {
                    checkReference(entity.animator->skeleton, engine::CatalogAssetKind::Skeleton, "Animator.skeleton");
                    checkReference(entity.animator->clip, engine::CatalogAssetKind::AnimationClip, "Animator.clip");
                    const auto skeleton = skeletons.find(entity.animator->skeleton.id);
                    const auto clip = clips.find(entity.animator->clip.id);
                    if (skeleton != skeletons.end() && clip != clips.end() && skeleton->second != clip->second) {
                        errors.push_back(context + "Animator.clip skeleton signature does not match Animator.skeleton (" +
                            catalog.Find(entity.animator->clip.id)->path + " and " +
                            catalog.Find(entity.animator->skeleton.id)->path + ")");
                    }
                    if (entity.skinnedRenderer) {
                        const auto mesh = meshes.find(entity.skinnedRenderer->mesh.id);
                        if (skeleton != skeletons.end() && mesh != meshes.end() && skeleton->second != mesh->second) {
                            errors.push_back(context + "SkinnedMeshRenderer.mesh skeleton signature does not match Animator.skeleton (" +
                                catalog.Find(entity.skinnedRenderer->mesh.id)->path + " and " +
                                catalog.Find(entity.animator->skeleton.id)->path + ")");
                        }
                    }
                }
            }
        }

        for (const auto& autoload : project.autoloads) {
            if (!autoload.enabled) {
                continue;
            }

            if (!catalog.Find(
                autoload.script.id,
                engine::CatalogAssetKind::Script)) {

                errors.push_back("autoload '" + autoload.name + "': script " + autoload.script.id.ToString() + " not found");
            }
        }

        engine::MaterialImporter materialImporter;

        for (const auto* material :
            catalog.List(engine::CatalogAssetKind::Material)) {

            engine::AssetDesc desc;
            desc.name = material->path;
            desc.path = material->source;
            desc.catalog = &catalog;

            engine::CpuPayload payload;

            error.clear();

            if (!materialImporter.ReadCpu(
                desc,
                nullptr,
                payload,
                error
            )) {

                errors.push_back(material->path + ": " + error);
            }
        }

        return errors.empty();
    }

    std::optional<engine::AssetKind> CookedKind(const fs::path& file) {
        switch (engine::CatalogKindForFile(file).value_or(engine::CatalogAssetKind::Scene)) {
        case engine::CatalogAssetKind::Mesh: return engine::AssetKind::Mesh;
        case engine::CatalogAssetKind::Skeleton: return engine::AssetKind::Skeleton;
        case engine::CatalogAssetKind::AnimationClip: return engine::AssetKind::AnimationClip;
        case engine::CatalogAssetKind::SkinnedMesh: return engine::AssetKind::SkinnedMesh;
        default: return std::nullopt;
        }
    }

    // In memory, so a failure leaves the previous output untouched.
    bool Cook(
        const engine::Project& project,
        const engine::AssetCatalog& catalog,
        Payloads& payloads,
        Cooked& cooked,
        std::vector<std::string>& errors
    ) {
        for (const auto& [root, prefix] : ContentRoots(project)) {
            if (!fs::is_directory(root)) continue;
            for (const auto& item : fs::recursive_directory_iterator(root)) {
                const auto kind = item.is_regular_file() ? CookedKind(item.path()) : std::nullopt;
                if (!kind) continue;
                const fs::path relative = fs::path(prefix) / item.path().lexically_relative(root);
                const std::string name = engine::ToGenericUtf8(relative);
                engine::CpuPayload payload;
                std::string error;
                const auto* entry = catalog.FindPath(name);
                if (const auto found = entry ? payloads.find(entry->metadata.id) : payloads.end();
                    found != payloads.end()) {
                    payload = std::move(found->second);
                    payloads.erase(found);
                } else {
                    engine::AssetDesc desc;
                    desc.name = name;
                    desc.path = item.path();
                    desc.catalog = &catalog;
                    const bool read = *kind == engine::AssetKind::Mesh
                        ? engine::AssimpMeshImporter{}.ReadCpu(desc, nullptr, payload, error)
                        : engine::SkeletalAssetImporter{*kind}.ReadCpu(desc, nullptr, payload, error);
                    if (!read) {
                        errors.push_back(name + ": " + error);
                        continue;
                    }
                }
                CookedFile file{engine::CookedPath(relative), {}};
                if (!engine::SerializeCooked(*kind, payload, file.bytes, error)) {
                    errors.push_back(name + ": cannot cook: " + error);
                    continue;
                }
                cooked.files.push_back(std::move(file));
                cooked.sources.insert(item.path());
            }
        }
        if (std::string error; !catalog.SerializeManifest(cooked.manifest, error)) {
            errors.push_back("asset manifest: " + error);
        }
        engine::ShaderLibrary shaders;
        if (engine::CookShaderLibrary(catalog, shaders, errors)) {
            if (std::string error; !shaders.Serialize(cooked.shaders, error)) {
                errors.push_back("shader library: " + error);
            }
        }
        return errors.empty();
    }

    // Audio streams straight from the pack, and these images are compressed already.
    bool Compressible(const fs::path& file) {
        if (engine::CatalogKindForFile(file) == engine::CatalogAssetKind::AudioClip) return false;
        const auto extension = engine::content::FoldCase(engine::ToUtf8(file.extension()));
        return extension != ".png" && extension != ".jpg" && extension != ".jpeg";
    }

    std::vector<std::uint8_t> Bytes(std::string_view text) {
        return {text.begin(), text.end()};
    }

    bool Stage(
        const Options& options,
        const engine::Project& project,
        Cooked cooked,
        std::string& error
    ) {

        if (!fs::is_directory(options.runtime)) {
            error = "runtime directory does not exist: " + engine::ToUtf8(options.runtime);
            return false;
        }

        const fs::path runtimeExecutable = options.runtime / L"sri_runtime.exe";

        if (!fs::is_regular_file(runtimeExecutable)) {
            error = "runtime executable not found: " + engine::ToUtf8(runtimeExecutable);
            return false;
        }

        if (!fs::is_directory(project.ContentDir())) {
            error = "project content directory does not exist: " + engine::ToUtf8(project.ContentDir());
            return false;
        }

        if (PathsOverlap(options.out, options.runtime)) {
            error = "output directory must not overlap the runtime directory";
            return false;
        }

        if (PathsOverlap(options.out, project.Root())) {
            error = "output directory must not overlap the project directory";
            return false;
        }

        const fs::path projectName = engine::FromUtf8(project.name);

        if (projectName.empty() ||
            projectName == L"." ||
            projectName == L".." ||
            projectName.has_parent_path() ||
            projectName.filename() != projectName) {

            error = "invalid project name for the output executable";
            return false;
        }

        // Only folders carrying this stamp are ever deleted: game.json alone is too common a name.
        const fs::path stamp = options.out / BuildStampName;

        if (fs::exists(options.out)) {
            if (!fs::is_directory(options.out)) {
                error = "output path exists and is not a directory: " + engine::ToUtf8(options.out);
                return false;
            }

            if (!fs::is_empty(options.out) && !fs::is_regular_file(stamp)) {
                error = "refusing to overwrite '" + engine::ToUtf8(options.out) + "': not empty and not made by sri-build";
                return false;
            }
        }

        std::string json;
        if (!engine::SerializeRuntimeSettings(options.settings, json, error)) return false;
        if (engine::RuntimeSettings check; !engine::ParseRuntimeSettings(json, check, error)) {
            error = "invalid runtime settings: " + error;
            return false;
        }

        fs::remove_all(options.out);
        fs::create_directories(options.out);

        // Stamp first, so the next build can still replace a half-finished one.
        if (!engine::WriteFileAtomic(stamp, BuildStampText, error) ||
            !engine::WriteFileAtomic(options.out / engine::RuntimeSettings::FileName, json, error)) {
            return false;
        }

        // The template's engine/ holds sources; the cooked one below replaces it.
        for (const auto& item : fs::directory_iterator(options.runtime)) {
            if (item.path().filename() == L"engine") continue;
            fs::copy(item.path(), options.out / item.path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }

        fs::rename(
            options.out / L"sri_runtime.exe",
            options.out / (projectName.native() + L".exe")
        );

        // Game content goes into the pack, or beside the executable with --no-pak.
        engine::PakWriter pak;
        const auto addFile = [&](const fs::path& relative, const fs::path& source) {
            if (options.pack) {
                pak.AddFile(engine::ToGenericUtf8(relative), source, Compressible(source));
                return;
            }
            const fs::path target = options.out / relative;
            fs::create_directories(target.parent_path());
            fs::copy_file(source, target);
        };
        const auto addBytes = [&](const fs::path& relative, std::vector<std::uint8_t> bytes) {
            if (options.pack) {
                pak.AddBytes(engine::ToGenericUtf8(relative), std::move(bytes), true);
                return true;
            }
            const fs::path target = options.out / relative;
            fs::create_directories(target.parent_path());
            return engine::WriteFileAtomic(
                target, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), error);
        };

        // .meta files are replaced by the manifest, cooked sources by their cooked files and HLSL
        // by the shader library.
        for (const auto& [root, prefix] : ContentRoots(project)) {
            if (!fs::is_directory(root)) continue;
            for (const auto& item : fs::recursive_directory_iterator(root)) {
                if (!item.is_regular_file() || item.path().extension() == L".meta" ||
                    engine::CatalogKindForFile(item.path()) == engine::CatalogAssetKind::Shader ||
                    engine::content::FoldCase(engine::ToUtf8(item.path().extension())) == ".hlsli" ||
                    cooked.sources.contains(item.path())) {
                    continue;
                }
                addFile(prefix / item.path().lexically_relative(root), item.path());
            }
        }
        for (auto& file : cooked.files) {
            if (!addBytes(file.target, std::move(file.bytes))) return false;
        }
        if (!addBytes(L"manifest.json", Bytes(cooked.manifest)) ||
            !addBytes(engine::ShaderLibraryName, std::move(cooked.shaders))) {
            return false;
        }
        addFile(L"game.sriproject", project.file);

        if (options.pack) {
            const fs::path file = options.out / engine::PakFileName;
            if (!pak.Write(file, error)) return false;
            Info("Packed " + std::to_string(pak.Size()) + " files into " + engine::ToUtf8(file.filename()) + " (" +
                 std::to_string((fs::file_size(file) + 512 * 1024) / (1024 * 1024)) + " MB)");
        }
        return true;
    }

} // namespace

int wmain(int argc, wchar_t* argv[]) {

    std::cout << std::unitbuf;
    SetConsoleOutputCP(CP_UTF8);

    try {
        const fs::path toolRoot = engine::AssetPathResolver::GetRoot();

        Options options;
        std::string error;

        if (!ParseOptions(
            argc,
            argv,
            options,
            toolRoot,
            error
        )) {

            PrintUsage();
            Error(error);
            return 1;
        }

        Info("Project: " + engine::ToUtf8(options.project));
        Info("Output: " + engine::ToUtf8(options.out));
        Info("Runtime: " + engine::ToUtf8(options.runtime));

        auto project = engine::Project::Load(options.project,error);

        if (!project) {
            Error(error);
            return 1;
        }

        Info("Loaded project '" + project->name + "'");

        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        engine::AssetCatalog catalog;
        Payloads payloads;

        if (!Validate(*project, catalog, payloads, errors, warnings)) {
            for (const auto& warning : warnings) {
                Warning(warning);
            }

            for (const auto& validationError : errors) {
                Error(validationError);
            }

            return 1;
        }

        for (const auto& warning : warnings) {
            Warning(warning);
        }

        Info("Project validation passed");

        Cooked cooked;
        if (!Cook(*project, catalog, payloads, cooked, errors)) {
            for (const auto& cookError : errors) {
                Error(cookError);
            }
            return 1;
        }

        Info("Cooked " + std::to_string(cooked.files.size()) + " assets");

        if (!Stage(options, *project, std::move(cooked), error)) {
            Error(error);
            return 1;
        }

        Info("Build completed successfully");
        Info("Game output: " + engine::ToUtf8(options.out));

        return 0;
    }
    catch (const fs::filesystem_error& e) {
        Error(std::string("filesystem error: ") + e.what());
        return 1;
    }
    catch (const std::exception& e) {
        Error(std::string("build failed: ") + e.what());
        return 1;
    }
    catch (...) {
        Error("build failed: unknown exception");
        return 1;
    }
}
