#include <engine/assets/AssetSystem.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <string>
#include <utility>

#include <engine/core/Log.hpp>

#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/assets/AssetEvents.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>

namespace engine {
namespace {

template <typename T>
T MakeValue(const AssetDesc& desc, const LoadJob& job, const GpuPayload& gpu) {
    const CpuPayload& cpu = job.payload;
    if constexpr (std::is_same_v<T, MeshAsset>) {
        return MeshAsset{gpu.meshSections, cpu.mesh.materialSlots, job.meshMaterials, cpu.mesh.bounds,
                         cpu.mesh.geometry};
    } else if constexpr (std::is_same_v<T, TextureAsset>) {
        return TextureAsset{gpu.texture};
    } else if constexpr (std::is_same_v<T, ShaderAsset>) {
        return ShaderAsset{gpu.shader, desc.shaderStage};
    } else if constexpr (std::is_same_v<T, MaterialAsset>) {
        return MaterialAsset{gpu.material, cpu.material.definition};
    } else if constexpr (std::is_same_v<T, SkeletonAsset>) {
        return cpu.skeleton;
    } else if constexpr (std::is_same_v<T, ParticleEffectAsset>) {
        return ParticleEffectAsset{cpu.particleEffect, job.textureDependency};
    } else if constexpr (std::is_same_v<T, AnimationClipAsset>) {
        return cpu.animationClip;
    } else if constexpr (std::is_same_v<T, SkinnedMeshAsset>) {
        return SkinnedMeshAsset{gpu.skinnedSections, cpu.skinnedMesh.bounds,
            cpu.skinnedMesh.skeletonSignature, cpu.skinnedMesh.skeletonReference};
    } else {
        return MaterialInstanceAsset{gpu.materialInstance, cpu.materialInstance};
    }
}

void AppendKeyPart(std::string& key, const std::string_view part) {
    key += std::to_string(part.size());
    key += ':';
    key.append(part);
}

std::uint64_t HashSource(const std::string_view source) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : source) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}

std::string ShaderVariantName(const AssetDesc& desc) {
    std::string key = "shader:";
    AppendKeyPart(key, ToGenericUtf8(desc.path));
    AppendKeyPart(key, std::to_string(static_cast<int>(desc.shaderStage)));
    AppendKeyPart(key, desc.shaderOptions.entryPoint);
    AppendKeyPart(key, std::to_string(desc.shaderOptions.defines.size()));
    for (const auto& define : desc.shaderOptions.defines) {
        AppendKeyPart(key, define.name);
        AppendKeyPart(key, define.value);
    }
    AppendKeyPart(key, desc.shaderFromMemory ? "memory" : "file");
    if (desc.shaderFromMemory) {
        AppendKeyPart(key, std::to_string(desc.shaderSource.size()));
        AppendKeyPart(key, std::to_string(HashSource(desc.shaderSource)));
    }
    return key;
}

template <typename T>
entt::resource<T> FailLoad(AssetTable<T>& table, const AssetHandle<T> handle, const std::string& error) {
    const typename AssetTable<T>::Record* record = table.Get(handle);
    log::Assets().error("Failed to load '{}': {}",
                  record != nullptr ? record->desc.name : std::string{}, error);
    table.Fail(handle, error);
    return {};
}

constexpr std::string_view ToString(const AssetKind kind) {
    switch (kind) {
    case AssetKind::Mesh:
        return "mesh";
    case AssetKind::Texture:
        return "texture";
    case AssetKind::Shader:
        return "shader";
    case AssetKind::Material:
        return "material";
    case AssetKind::MaterialInstance:
        return "material instance";
    case AssetKind::Skeleton: return "skeleton";
    case AssetKind::AnimationClip: return "animation clip";
    case AssetKind::SkinnedMesh: return "skinned mesh";
    case AssetKind::ParticleEffect: return "particle effect";
    case AssetKind::Count:
        break;
    }
    return "unknown";
}

/// Cooked builds ship a manifest in place of the .meta files a scan reads.
bool BuildCatalog(AssetCatalog& catalog, std::string& error) {
    const auto engineRoot = AssetPathResolver::GetRoot() / "engine";
    if (!AssetPathResolver::IsCooked()) return catalog.Scan(AssetPathResolver::GetGameRoot(), engineRoot, error);
    std::string text;
    if (!ReadTextFile(AssetPathResolver::GetManifest(), text, error)) return false;
    return catalog.LoadManifest(text, AssetPathResolver::GetGameRoot(), engineRoot, error);
}

}  // namespace

AssetSystem::AssetSystem(Engine& engine) : engine_(engine) {}

AssetSystem::~AssetSystem() = default;

void AssetSystem::Initialize() {
    if (!BuildCatalog(catalog_, catalogError_)) {
        log::Assets().error("Cannot build asset catalog: {}", catalogError_);
    }
    importers_[static_cast<size_t>(AssetKind::Mesh)] = std::make_unique<AssimpMeshImporter>();
    importers_[static_cast<size_t>(AssetKind::Texture)] = std::make_unique<TextureImporter>();
    importers_[static_cast<size_t>(AssetKind::Shader)] = std::make_unique<ShaderImporter>();
    importers_[static_cast<size_t>(AssetKind::Material)] = std::make_unique<MaterialImporter>();
    importers_[static_cast<size_t>(AssetKind::MaterialInstance)] = std::make_unique<MaterialInstanceImporter>();
    importers_[static_cast<size_t>(AssetKind::ParticleEffect)] = std::make_unique<ParticleEffectImporter>();
    for (const auto kind : {AssetKind::Skeleton, AssetKind::AnimationClip, AssetKind::SkinnedMesh})
        importers_[static_cast<size_t>(kind)] = std::make_unique<SkeletalAssetImporter>(kind);
    if (AssetPathResolver::IsCooked()) {
        for (const auto kind : {AssetKind::Mesh, AssetKind::Skeleton, AssetKind::AnimationClip, AssetKind::SkinnedMesh}) {
            auto& importer = importers_[static_cast<size_t>(kind)];
            importer = std::make_unique<CookedAssetImporter>(kind, std::move(importer));
        }
    }
}

void AssetSystem::Update(const FrameContext& frame) {
    // The worker only fills its payload; device calls and table writes stay on this thread.
    // inFlight_ itself is touched from here and LoadSync only, so it needs no lock.
    const auto count = inFlight_.size();
    for (size_t i = 0; i < count && !inFlight_.empty(); ++i) {
        // No iterator survives a dependency request or an event callback that changes the queue.
        auto job = std::move(inFlight_.front());
        inFlight_.pop_front();
        if (!job->GetIsComplete() || !FinishJob(*job, frame.frameIndex)) {
            inFlight_.push_back(std::move(job));
        }
    }
    // Jobs that are complete but wait for dependencies are only touched here, on this thread.
    if (pendingCatalog_ && std::ranges::all_of(inFlight_, [](const auto& job) { return job->GetIsComplete(); })) {
        ApplyCatalog(std::move(*pendingCatalog_));
    }
}

bool AssetSystem::RescanCatalog(std::string& error) {
    AssetCatalog next = pendingCatalog_ ? *pendingCatalog_ : catalog_;
    const bool clean = BuildCatalog(next, error);
    catalogError_ = error;
    if (std::ranges::all_of(inFlight_, [](const auto& job) { return job->GetIsComplete(); })) {
        ApplyCatalog(std::move(next));
    } else {
        pendingCatalog_ = std::move(next);
    }
    return clean;
}

void AssetSystem::Shutdown() {
    // Engine::Shutdown() calls this before TaskScheduler::WaitforAllAndShutdown(), so the
    // scheduler is still usable. Waiting for each job is what ~ICompletable requires; their
    // results are dropped, nobody consumes assets during kernel shutdown.
    for (std::unique_ptr<LoadJob>& job : inFlight_) {
        engine_.GetTaskScheduler().WaitforTask(job.get());
    }
    inFlight_.clear();
    // A deferred rescan belongs to this session. Do not let Update replace the
    // freshly scanned catalog after a subsequent Initialize.
    pendingCatalog_.reset();
}

entt::resource<MeshAsset> AssetSystem::LoadMesh(const std::string_view name, const bool centerOnLoad) {
    return LoadSync(RequestMesh(name, centerOnLoad));
}

entt::resource<TextureAsset> AssetSystem::LoadTexture(const std::string_view name) {
    return LoadSync(RequestTexture(name));
}

entt::resource<MaterialAsset> AssetSystem::LoadMaterial(const std::string_view name) {
    return LoadSync(RequestMaterial(name));
}

entt::resource<MaterialInstanceAsset> AssetSystem::LoadMaterialInstance(const std::string_view name) {
    return LoadSync(RequestMaterialInstance(name));
}

entt::resource<SkeletonAsset> AssetSystem::LoadSkeleton(std::string_view name) {
    return LoadSync(RequestSkeleton(name));
}
entt::resource<ParticleEffectAsset> AssetSystem::LoadParticleEffect(std::string_view name) {
    return LoadSync(RequestParticleEffect(name));
}
AssetHandle<ParticleEffectAsset> AssetSystem::RequestParticleEffect(std::string_view name) {
    if (name.empty()) return {};
    AssetDesc desc;
    desc.name = NormalizeAssetName(name, AssetKind::ParticleEffect);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::ParticleEffect);
    return Request<ParticleEffectAsset>(std::move(desc));
}
AssetHandle<ParticleEffectAsset> AssetSystem::RequestParticleEffect(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::ParticleEffect);
    return entry ? RequestParticleEffect(entry->path) : AssetHandle<ParticleEffectAsset>{};
}
entt::resource<AnimationClipAsset> AssetSystem::LoadAnimationClip(std::string_view name) {
    return LoadSync(RequestAnimationClip(name));
}
entt::resource<SkinnedMeshAsset> AssetSystem::LoadSkinnedMesh(std::string_view name) {
    return LoadSync(RequestSkinnedMesh(name));
}

AssetHandle<SkeletonAsset> AssetSystem::RequestSkeleton(std::string_view name) {
    if (name.empty()) return {};
    AssetDesc desc;
    desc.name = NormalizeName(name);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::Skeleton);
    return Request<SkeletonAsset>(std::move(desc));
}
AssetHandle<AnimationClipAsset> AssetSystem::RequestAnimationClip(std::string_view name) {
    if (name.empty()) return {};
    AssetDesc desc;
    desc.name = NormalizeName(name);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::AnimationClip);
    return Request<AnimationClipAsset>(std::move(desc));
}
AssetHandle<SkinnedMeshAsset> AssetSystem::RequestSkinnedMesh(std::string_view name) {
    if (name.empty()) return {};
    AssetDesc desc;
    desc.name = NormalizeName(name);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::SkinnedMesh);
    return Request<SkinnedMeshAsset>(std::move(desc));
}

AssetHandle<SkeletonAsset> AssetSystem::RequestSkeleton(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::Skeleton);
    return entry ? RequestSkeleton(entry->path) : AssetHandle<SkeletonAsset>{};
}
AssetHandle<AnimationClipAsset> AssetSystem::RequestAnimationClip(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::AnimationClip);
    return entry ? RequestAnimationClip(entry->path) : AssetHandle<AnimationClipAsset>{};
}
AssetHandle<SkinnedMeshAsset> AssetSystem::RequestSkinnedMesh(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::SkinnedMesh);
    return entry ? RequestSkinnedMesh(entry->path) : AssetHandle<SkinnedMeshAsset>{};
}

bool AssetSystem::Reload(std::string_view name, AssetKind kind, std::string& error) {
    error.clear();
    if (kind != AssetKind::Material && kind != AssetKind::MaterialInstance && kind != AssetKind::ParticleEffect) {
        error = "reload is not supported for this asset type";
        return false;
    }
    const std::string normalized = NormalizeAssetName(name, kind);
    // A never-requested asset has no live state to replace.
    if (kind == AssetKind::Material && !materials_.Find(normalized)) return true;
    if (kind == AssetKind::MaterialInstance && !materialInstances_.Find(normalized)) return true;
    if (kind == AssetKind::ParticleEffect && !particleEffects_.Find(normalized)) {
        // Failed records retire their handles. Extraction resolves effects by catalog name,
        // so a save can recover a previously broken scene effect without editing its emitter.
        RequestParticleEffect(normalized);
        return true;
    }
    if (kind == AssetKind::Material) {
        const auto handle = materials_.Find(normalized);
        if (materials_.Get(handle)->state == AssetState::Loading && !LoadSync(handle)) {
            error = "could not finish loading the previous material";
            return false;
        }
    } else if (kind == AssetKind::ParticleEffect) {
        const auto handle = particleEffects_.Find(normalized);
        if (particleEffects_.Get(handle)->state == AssetState::Loading && !LoadSync(handle)) {
            error = "could not finish loading the previous particle effect";
            return false;
        }
    } else {
        const auto handle = materialInstances_.Find(normalized);
        if (materialInstances_.Get(handle)->state == AssetState::Loading && !LoadSync(handle)) {
            error = "could not finish loading the previous material instance";
            return false;
        }
    }
    LoadJob job;
    job.kind = kind;
    job.name = normalized;
    job.id = ToResourceID(normalized);
    AssetDesc desc;
    const auto capture = [&](const auto& table) {
        const auto handle = table.Find(normalized);
        const auto* record = table.Get(handle);
        if (!record || record->state != AssetState::Ready) return false;
        job.slot = handle.slot;
        job.generation = handle.generation;
        desc = record->desc;
        return true;
    };
    if (!(kind == AssetKind::ParticleEffect ? capture(particleEffects_) :
          kind == AssetKind::Material ? capture(materials_) : capture(materialInstances_))) {
        error = "asset is no longer Ready for reload";
        return false;
    }
    // Dependency completion can dispatch reentrant listeners. Keep the descriptor
    // owned locally rather than borrowing a record that a listener can evict.
    job.desc = &desc;
    if (auto* rendering = engine_.GetSubsystem<RenderingSubsystem>()) job.device = rendering->GetContext().GetDevice();
    job.cpuOk = ImporterFor(kind).ReadCpu(desc, job.device, job.payload, job.error);
    if (job.cpuOk && ResolveDependencies(job, true) == DependencyState::Failed) job.cpuOk = false;
    if (!job.cpuOk) {
        error = job.error;
        log::Assets().error("Live reload failed for '{}': {}", normalized, error);
        return false;
    }
    if (!ReloadReady(job, error)) {
        log::Assets().error("Live reload failed for '{}': {}", normalized, error);
        return false;
    }
    const AssetLoadedEvent event{job.id, job.name, job.kind, this, job.slot, job.generation};
    engine_.GetEvents().trigger(event);
    return true;
}

bool AssetSystem::ReloadReady(LoadJob& job, std::string& error) {
    if (job.kind == AssetKind::ParticleEffect) {
        auto* record = particleEffects_.Get({job.slot, job.generation});
        if (!record || record->state != AssetState::Ready) {
            error = "asset was retired while resolving reload dependencies";
            return false;
        }
        *record->value = ParticleEffectAsset{std::move(job.payload.particleEffect), job.textureDependency};
        return true;
    }
    const auto current = [&](const auto& table, const auto handle) {
        const auto* record = table.Get(handle);
        return record && record->state == AssetState::Ready;
    };
    if (!(job.kind == AssetKind::Material
        ? current(materials_, AssetHandle<MaterialAsset>{job.slot, job.generation})
        : current(materialInstances_, AssetHandle<MaterialInstanceAsset>{job.slot, job.generation}))) {
        error = "asset was retired while resolving reload dependencies";
        return false;
    }
    auto* rendering = engine_.GetSubsystem<RenderingSubsystem>();
    if (!rendering) { error = "no RenderingSubsystem to upload the result to"; return false; }
    auto& resources = rendering->GetResourceManager();
    auto transfer = MakeTransfer(*rendering);
    transfer.material = job.resolvedMaterial;
    transfer.materialParameters = job.resolvedParameters;
    GpuPayload staged;
    if (!ImporterFor(job.kind).UploadGpu(transfer, job.payload, staged, error)) return false;

    if (job.kind == AssetKind::MaterialInstance) {
        auto* record = materialInstances_.Get({job.slot, job.generation});
        const auto* old = resources.TryGetMaterialInstance(record->value->instance);
        if (!old) {
            resources.DestroyMaterialInstance(staged.materialInstance);
            error = "live material instance is no longer available";
            return false;
        }
        const bool committed = resources.UpdateMaterialInstance(record->value->instance, job.resolvedMaterial,
            MaterialInstanceImporter::MakeOverrides(job.payload.materialInstance.parameters,
                job.resolvedParameters.albedoTexture));
        resources.DestroyMaterialInstance(staged.materialInstance);
        if (!committed) { error = "cannot update the loaded material instance"; return false; }
        record->value->definition = std::move(job.payload.materialInstance);
        return true;
    }

    auto* record = materials_.Get({job.slot, job.generation});
    const MaterialHandle live = record->value->material;
    if (!resources.ReplaceMaterial(live, staged.material)) {
        resources.DestroyMaterial(staged.material);
        error = "cannot replace the loaded material";
        return false;
    }
    record->value->definition = std::move(job.payload.material.definition);
    return true;
}

std::string AssetSystem::NormalizeAssetName(std::string_view name, AssetKind kind) {
    auto normalized = NormalizeName(name);
    if (!normalized.empty() && (kind == AssetKind::Material || kind == AssetKind::MaterialInstance ||
                               kind == AssetKind::ParticleEffect) &&
        FromUtf8(normalized).extension().empty()) {
        normalized += kind == AssetKind::ParticleEffect ? ".particle-effect.json" :
            kind == AssetKind::Material ? ".material.json" : ".material-instance.json";
    }
    return normalized;
}

AssetHandle<MaterialAsset> AssetSystem::RequestMaterial(std::string_view name) {
    if (name.empty()) return {};
    AssetDesc desc;
    desc.name = NormalizeAssetName(name, AssetKind::Material);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::Material);
    return Request<MaterialAsset>(std::move(desc));
}

AssetHandle<MaterialInstanceAsset> AssetSystem::RequestMaterialInstance(std::string_view name) {
    if (name.empty()) return {};
    AssetDesc desc;
    desc.name = NormalizeAssetName(name, AssetKind::MaterialInstance);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::MaterialInstance);
    return Request<MaterialInstanceAsset>(std::move(desc));
}

entt::resource<ShaderAsset> AssetSystem::LoadShader(const std::string_view name, const ShaderType stage,
                                                    ShaderCompileOptions options) {
    return LoadSync(RequestShader(name, stage, std::move(options)));
}

entt::resource<ShaderAsset> AssetSystem::LoadShaderSource(const std::string_view virtualPath, std::string source,
                                                          const ShaderType stage, ShaderCompileOptions options) {
    return LoadSync(RequestShaderSource(virtualPath, std::move(source), stage, std::move(options)));
}

AssetHandle<ShaderAsset> AssetSystem::RequestShader(const std::string_view name, const ShaderType stage,
                                                   ShaderCompileOptions options) {
    AssetDesc desc;
    desc.path = AssetPathResolver::DefaultPath(NormalizeName(name), AssetKind::Shader);
    if (desc.path.extension().empty()) {
        desc.path += ".hlsl";
    }
    desc.shaderStage = stage;
    desc.shaderOptions = std::move(options);
    return RequestShaderVariant(std::move(desc));
}

AssetHandle<ShaderAsset> AssetSystem::RequestShaderSource(const std::string_view virtualPath, std::string source,
                                                         const ShaderType stage, ShaderCompileOptions options) {
    AssetDesc desc;
    desc.path = AssetPathResolver::DefaultPath(NormalizeName(virtualPath), AssetKind::Shader);
    if (desc.path.extension().empty()) {
        desc.path += ".hlsl";
    }
    desc.shaderStage = stage;
    desc.shaderOptions = std::move(options);
    desc.shaderFromMemory = true;
    desc.shaderSource = std::move(source);
    return RequestShaderVariant(std::move(desc));
}

AssetHandle<ShaderAsset> AssetSystem::RequestShaderVariant(AssetDesc&& desc) {
    if (desc.shaderOptions.entryPoint.empty()) {
        desc.shaderOptions.entryPoint = GetEntryPoint(desc.shaderStage);
    }
    auto& defines = desc.shaderOptions.defines;
    std::ranges::sort(defines, {}, &ShaderDefine::name);
    for (std::size_t i = 0; i < defines.size(); ++i) {
        if (defines[i].name.empty() || (i > 0 && defines[i].name == defines[i - 1].name)) {
            log::Assets().error("Invalid or duplicate shader define for '{}'", ToUtf8(desc.path));
            return AssetHandle<ShaderAsset>::Invalid();
        }
    }
    desc.name = ShaderVariantName(desc);
    // The source hash is only a compact part of the key; do not silently alias
    // different generated sources if it ever collides.
    if (const auto existing = shaders_.Find(desc.name)) {
        if (shaders_.Get(existing)->desc.shaderSource != desc.shaderSource ||
            shaders_.Get(existing)->desc.shaderFromMemory != desc.shaderFromMemory) {
            log::Assets().error("Shader source identity collision for '{}'", ToUtf8(desc.path));
            return AssetHandle<ShaderAsset>::Invalid();
        }
    }
    return Request<ShaderAsset>(std::move(desc));
}

AssetHandle<MeshAsset> AssetSystem::RequestMesh(const std::string_view name, const bool centerOnLoad) {
    AssetDesc desc;
    desc.name = NormalizeName(name);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::Mesh);
    desc.centerOnLoad = centerOnLoad;
    return Request<MeshAsset>(std::move(desc));
}

std::vector<AssetHandle<MaterialInstanceAsset>> AssetSystem::RequestSlotMaterials(const AssetDesc& desc,
                                                                                   const std::size_t slots) {
    const auto* entry = catalog_.FindPath(AssetPathResolver::ToName(AssetPathResolver::ResolveMesh(desc)));
    const bool authored = entry != nullptr && entry->metadata.kind == CatalogAssetKind::Mesh;
    std::vector<AssetHandle<MaterialInstanceAsset>> materials;
    materials.reserve(slots);
    for (std::size_t slot = 0; slot < slots; ++slot) {
        AssetHandle<MaterialInstanceAsset> material;
        if (authored && slot < entry->metadata.materials.size() && entry->metadata.materials[slot]) {
            material = RequestMaterialInstance(*entry->metadata.materials[slot]);
        }
        materials.push_back(material ? material : RequestMaterialInstance(BuiltinAssets::DefaultMaterialInstance));
    }
    return materials;
}

void AssetSystem::ApplyCatalog(AssetCatalog&& catalog) {
    catalog_ = std::move(catalog);
    pendingCatalog_.reset();
    // Unchanged slots get the handles they have, so only edited ones reload.
    meshes_.ForEachReadyRecord([this](const auto& record) {
        record.value->materials = RequestSlotMaterials(record.desc, record.value->materialSlots.size());
    });
}

void AssetSystem::UnloadAll() {
    for (const auto& job : inFlight_) engine_.GetTaskScheduler().WaitforTask(job.get());
    inFlight_.clear(); // do not publish a scene that has already gone away
    if (auto* rendering = engine_.GetSubsystem<RenderingSubsystem>()) {
        auto& gpu = rendering->GetResourceManager();
        materialInstances_.ForEachReady([&](const MaterialInstanceAsset& value) {
            gpu.DestroyMaterialInstance(value.instance);
        });
        materials_.ForEachReady([&](const MaterialAsset& value) { gpu.DestroyMaterial(value.material); });
        shaders_.ForEachReady([&](const ShaderAsset& value) { gpu.DestroyShader(value.shader); });
        textures_.ForEachReady([&](const TextureAsset& value) { gpu.DestroyTexture(value.texture); });
        meshes_.ForEachReady([&](const MeshAsset& value) {
            for (const auto& section : value.sections) gpu.DestroyMesh(section.mesh);
        });
        skinnedMeshes_.ForEachReady([&](const SkinnedMeshAsset& value) {
            for (const auto& section : value.sections) gpu.DestroyMesh(section.mesh);
        });
    }
    materialInstances_.EvictAll();
    materials_.EvictAll();
    shaders_.EvictAll();
    textures_.EvictAll();
    meshes_.EvictAll();
    skinnedMeshes_.EvictAll();
    animationClips_.EvictAll();
    skeletons_.EvictAll();
    particleEffects_.EvictAll();
}

std::optional<AssetReference> AssetSystem::ReferenceFor(AssetHandle<SkinnedMeshAsset> handle) const {
    const auto* record = skinnedMeshes_.Get(handle);
    if (!record) return std::nullopt;
    const auto* entry = catalog_.FindPath(record->desc.name);
    if (!entry || entry->metadata.kind != CatalogAssetKind::SkinnedMesh) return std::nullopt;
    return AssetReference{entry->metadata.id, entry->path};
}

std::optional<AssetReference> AssetSystem::ReferenceFor(AssetHandle<MeshAsset> handle) const {
    const auto* record = meshes_.Get(handle);
    if (!record) return std::nullopt;
    const auto source = AssetPathResolver::ResolveMesh(record->desc);
    const auto* entry = catalog_.FindPath(AssetPathResolver::ToName(source));
    if (!entry || entry->metadata.kind != CatalogAssetKind::Mesh) return std::nullopt;
    return AssetReference{entry->metadata.id, entry->path};
}

std::optional<AssetReference> AssetSystem::ReferenceFor(AssetHandle<MaterialInstanceAsset> handle) const {
    const auto* record = materialInstances_.Get(handle);
    if (!record) return std::nullopt;
    const auto* entry = catalog_.FindPath(record->desc.name);
    if (!entry || entry->metadata.kind != CatalogAssetKind::MaterialInstance) return std::nullopt;
    return AssetReference{entry->metadata.id, entry->path};
}

const CatalogEntry* AssetSystem::Resolve(const AssetReference& reference, const CatalogAssetKind kind) const {
    const CatalogEntry* entry = catalog_.Find(reference.id, kind);
    if (entry == nullptr) {
        log::Assets().error("Missing or wrong-kind UUID '{}' (hint '{}')", reference.id.ToString(), reference.path);
        return nullptr;
    }
    if (NormalizeName(reference.path) != NormalizeName(entry->path)) {
        log::Assets().warn("Stale path hint '{}' for UUID '{}', now at '{}'",
                      reference.path, reference.id.ToString(), entry->path);
    }
    return entry;
}

AssetHandle<MeshAsset> AssetSystem::RequestMesh(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::Mesh);
    return entry ? RequestMesh(entry->path, entry->metadata.centerOnLoad) : AssetHandle<MeshAsset>{};
}

AssetHandle<TextureAsset> AssetSystem::RequestTexture(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::Texture);
    return entry ? RequestTexture(entry->path) : AssetHandle<TextureAsset>{};
}

AssetHandle<MaterialAsset> AssetSystem::RequestMaterial(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::Material);
    return entry ? RequestMaterial(entry->path) : AssetHandle<MaterialAsset>{};
}

AssetHandle<MaterialInstanceAsset> AssetSystem::RequestMaterialInstance(const AssetReference& reference) {
    const auto* entry = Resolve(reference, CatalogAssetKind::MaterialInstance);
    return entry ? RequestMaterialInstance(entry->path) : AssetHandle<MaterialInstanceAsset>{};
}

AssetHandle<TextureAsset> AssetSystem::RequestTexture(const std::string_view name) {
    AssetDesc desc;
    desc.name = NormalizeName(name);
    desc.path = AssetPathResolver::DefaultPath(desc.name, AssetKind::Texture);
    return Request<TextureAsset>(std::move(desc));
}

template <typename T>
AssetHandle<T> AssetSystem::Request(AssetDesc&& desc) {
    desc.catalog = &catalog_;
    AssetTable<T>& table = Table<T>();
    const std::string name = desc.name;

    AssetHandle<T> handle = table.Acquire(std::move(desc));
    if (!handle) {
        const typename AssetTable<T>::Record* collision = table.FindCollision(name);
        log::Assets().error("Name '{}' hashes to the same id as already registered '{}'",
                      name, collision != nullptr ? collision->desc.name : std::string{});
        return AssetHandle<T>::Invalid();
    }

    if (typename AssetTable<T>::Record* record = table.Get(handle);
        record != nullptr && record->state == AssetState::Unloaded) {
        table.MarkLoading(handle);
        DispatchJob(AssetValueKind<T>::value, record->desc, handle.slot, handle.generation);
    }
    return handle;
}

template <typename T>
entt::resource<T> AssetSystem::LoadSync(const AssetHandle<T> handle) {
    if (!handle) {
        return {};
    }
    AssetTable<T>& table = Table<T>();
    typename AssetTable<T>::Record* record = table.Get(handle);
    if (record == nullptr) {
        return {};
    }
    if (record->state == AssetState::Ready) {
        return record->value;
    }

    const AssetKind kind = AssetValueKind<T>::value;
    if (record->state != AssetState::Loading) {
        table.MarkLoading(handle);
        DispatchJob(kind, record->desc, handle.slot, handle.generation);
    }

    std::unique_ptr<LoadJob> job = TakeJob(kind, handle.slot);
    if (job == nullptr) {
        const AssetFailedEvent event{record->id, record->desc.name, kind,
            "load job disappeared from the scheduler", this, handle.slot, handle.generation};
        auto failed = FailLoad(table, handle, event.error);
        engine_.GetEvents().trigger(event);
        return failed;
    }
    engine_.GetTaskScheduler().WaitforTask(job.get());
    if (job->cpuOk && ResolveDependencies(*job, true) == DependencyState::Failed) job->cpuOk = false;
    return FinishTyped(*job, table, handle, engine_.GetFrame().frameIndex);
}

void AssetSystem::DispatchJob(const AssetKind kind, const AssetDesc& desc, const uint32_t slot,
                              const uint32_t generation) {
    std::unique_ptr<LoadJob> job = std::make_unique<LoadJob>();
    job->kind = kind;
    job->id = ToResourceID(desc.name);
    job->name = desc.name;
    job->slot = slot;
    job->generation = generation;
    job->desc = &desc;
    job->importer = importers_[static_cast<size_t>(kind)].get();
    // Resolved here, on the main thread: the worker only ever uses the free-threaded device.
    // Missing rendering is not fatal - a texture importer reports it, the rest never need it.
    if (RenderingSubsystem* rendering = engine_.GetSubsystem<RenderingSubsystem>()) {
        job->device = rendering->GetContext().GetDevice();
    }
    job->queuedAt = std::chrono::steady_clock::now();
    log::Assets().info("Queued '{}' ({}) on frame {}", desc.name, ToString(kind),
                 engine_.GetFrame().frameIndex);
    LoadJob* scheduled = job.get();
    inFlight_.push_back(std::move(job));
    engine_.GetTaskScheduler().AddTaskSetToPipe(scheduled);
}

std::unique_ptr<LoadJob> AssetSystem::TakeJob(const AssetKind kind, const uint32_t slot) {
    for (auto it = inFlight_.begin(); it != inFlight_.end(); ++it) {
        if ((*it)->kind == kind && (*it)->slot == slot) {
            std::unique_ptr<LoadJob> job = std::move(*it);
            inFlight_.erase(it);
            return job;
        }
    }
    return nullptr;
}

bool AssetSystem::FinishJob(LoadJob& job, const uint64_t frameIndex) {
    if (job.cpuOk) {
        const auto dependencies = ResolveDependencies(job, false);
        if (dependencies == DependencyState::Waiting) return false;
        if (dependencies == DependencyState::Failed) job.cpuOk = false;
    }
    switch (job.kind) {
    case AssetKind::Mesh:
        FinishTyped(job, meshes_, AssetHandle<MeshAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::Texture:
        FinishTyped(job, textures_, AssetHandle<TextureAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::Shader:
        FinishTyped(job, shaders_, AssetHandle<ShaderAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::Material:
        FinishTyped(job, materials_, AssetHandle<MaterialAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::MaterialInstance:
        FinishTyped(job, materialInstances_, AssetHandle<MaterialInstanceAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::Skeleton:
        FinishTyped(job, skeletons_, AssetHandle<SkeletonAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::AnimationClip:
        FinishTyped(job, animationClips_, AssetHandle<AnimationClipAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::SkinnedMesh:
        FinishTyped(job, skinnedMeshes_, AssetHandle<SkinnedMeshAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::ParticleEffect:
        FinishTyped(job, particleEffects_, AssetHandle<ParticleEffectAsset>{job.slot, job.generation}, frameIndex);
        break;
    case AssetKind::Count:
        break;
    }
    return true;
}

AssetSystem::DependencyState AssetSystem::ResolveDependencies(LoadJob& job, bool wait) {
    if (job.kind == AssetKind::ParticleEffect) {
        const auto& reference = job.payload.particleEffect.texture;
        if (!reference.id.IsValid()) return DependencyState::Ready;
        if (!job.dependenciesRequested) {
            job.dependenciesRequested = true;
            job.textureDependency = RequestTexture(reference);
        }
        const auto texture = wait ? LoadSync(job.textureDependency) : Peek(job.textureDependency);
        if (texture) return DependencyState::Ready;
        if (GetState(job.textureDependency) == AssetState::Loading) return DependencyState::Waiting;
        job.error = "particle texture dependency '" + reference.id.ToString() + "' failed while loading '" + job.name + "'";
        return DependencyState::Failed;
    }
    if (job.kind == AssetKind::Mesh) {
        if (!job.dependenciesRequested) {
            job.dependenciesRequested = true;
            job.meshMaterials = RequestSlotMaterials(*job.desc, job.payload.mesh.materialSlots.size());
        }
        return DependencyState::Ready;
    }
    const bool instance = job.kind == AssetKind::MaterialInstance;
    if (!instance && job.kind != AssetKind::Material) return DependencyState::Ready;
    const auto& overrides = instance ? job.payload.materialInstance.parameters : job.payload.material.definition.defaults;
    if (!job.dependenciesRequested) {
        job.dependenciesRequested = true;
        if (instance) job.materialDependency = job.payload.materialInstance.version == 2
            ? RequestMaterial(*job.payload.materialInstance.materialReference)
            : RequestMaterial(job.payload.materialInstance.material);
        if (overrides.albedoTexture && !overrides.albedoTexture->empty())
            job.textureDependency = overrides.albedoReference
                ? RequestTexture(*overrides.albedoReference) : RequestTexture(*overrides.albedoTexture);
    }
    const auto resolve = [&]<typename T>(AssetHandle<T> handle, entt::resource<T>& value,
                                        std::string_view name) -> DependencyState {
        value = wait ? LoadSync(handle) : Peek(handle);
        if (value) return DependencyState::Ready;
        const auto* record = Table<T>().Get(handle);
        if (!record || record->state != AssetState::Loading) {
            job.error = "dependency '" + std::string(name) + "' failed while loading '" + job.name + "'";
            return DependencyState::Failed;
        }
        return DependencyState::Waiting;
    };
    MaterialParameters parameters;
    if (instance) {
        entt::resource<MaterialAsset> parent;
        const auto state = resolve(job.materialDependency, parent, job.payload.materialInstance.material);
        if (state != DependencyState::Ready) return state;
        auto* rendering = engine_.GetSubsystem<RenderingSubsystem>();
        const auto* material = rendering ? rendering->GetResourceManager().TryGetMaterial(parent->material) : nullptr;
        if (!material) {
            job.error = "parent material no longer exists";
            return DependencyState::Failed;
        }
        job.resolvedMaterial = parent->material;
    }
    if (overrides.specularColor) parameters.parameters.specularColor = *overrides.specularColor;
    if (overrides.shininess) parameters.parameters.shininess = *overrides.shininess;
    if (overrides.albedoTexture) {
        parameters.albedoTexture = {}; // explicit null clears an inherited texture
        if (!overrides.albedoTexture->empty()) {
            entt::resource<TextureAsset> texture;
            const auto state = resolve(job.textureDependency, texture, *overrides.albedoTexture);
            if (state != DependencyState::Ready) return state;
            parameters.albedoTexture = texture->texture;
        }
    }
    job.resolvedParameters = parameters;
    return DependencyState::Ready;
}

template <typename T>
entt::resource<T> AssetSystem::FinishTyped(const LoadJob& job, AssetTable<T>& table,
                                           const AssetHandle<T> handle, const uint64_t frameIndex) {
    typename AssetTable<T>::Record* record = table.Get(handle);
    if (record == nullptr) {
        return {};  // the record was recycled while the job ran; the result is stale
    }
    if (!job.cpuOk) {
        const AssetFailedEvent event{job.id, job.name, job.kind, job.error, this, handle.slot, handle.generation};
        entt::resource<T> failed = FailLoad(table, handle, event.error);
        engine_.GetEvents().trigger(event);
        return failed;
    }

    GpuPayload gpu;
    if constexpr (!std::is_same_v<T, SkeletonAsset> && !std::is_same_v<T, AnimationClipAsset> &&
                  !std::is_same_v<T, ParticleEffectAsset>) {
        auto* rendering = engine_.GetSubsystem<RenderingSubsystem>();
        std::string error;
        bool uploaded = false;
        if (rendering) {
            auto transfer = MakeTransfer(*rendering);
            transfer.material = job.resolvedMaterial;
            transfer.materialParameters = job.resolvedParameters;
            uploaded = ImporterFor(job.kind).UploadGpu(transfer, job.payload, gpu, error);
        } else {
            error = "no RenderingSubsystem to upload the result to";
        }
        if (!uploaded) {
            const AssetFailedEvent event{job.id, job.name, job.kind, error, this, handle.slot, handle.generation};
            auto failed = FailLoad(table, handle, event.error);
            engine_.GetEvents().trigger(event);
            return failed;
        }
    }

    const auto totalSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - job.queuedAt).count();
    log::Assets().info("'{}' ({}) published on frame {} ({} s after queueing)",
                 job.name, ToString(job.kind), frameIndex, totalSeconds);

    const AssetLoadedEvent event{job.id, job.name, job.kind, this, handle.slot, handle.generation};
    entt::resource<T> value = table.Publish(handle, MakeValue<T>(record->desc, job, gpu));
    engine_.GetEvents().trigger(event);
    return value;
}

UploadTransfer AssetSystem::MakeTransfer(RenderingSubsystem& rendering) const {
    RenderingContext& context = rendering.GetContext();
    UploadTransfer transfer;
    transfer.device = context.GetDevice();
    transfer.context = context.GetDeviceContext();
    transfer.resources = &context.GetResourceManager();
    return transfer;
}

const IAssetImporter& AssetSystem::ImporterFor(const AssetKind kind) const {
    assert(importers_[static_cast<size_t>(kind)] != nullptr && "AssetSystem::Initialize() did not run");
    return *importers_[static_cast<size_t>(kind)];
}

}  // namespace engine
