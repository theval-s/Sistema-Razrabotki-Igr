#pragma once

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <engine/assets/AssetCommon.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetHandle.hpp>
#include <engine/assets/AssetTable.hpp>
#include <engine/assets/AssetTypes.hpp>
#include <engine/engine_export.hpp>

#include <entt/resource/resource.hpp>

namespace engine {

struct FrameContext;
class Engine;
class RenderingSubsystem;
class IAssetImporter;
struct UploadTransfer;
struct LoadJob;

/// Name-addressed registry of meshes, textures, shaders, materials and material instances.
///
/// Adds identity, deduplication, state and background loading on top of
/// RenderingResourceManager, which stays the owner of all GPU memory. Assets are
/// addressed by normalized names ("models/table" == "Models\\Table"), each table
/// entry goes Unloaded -> Loading -> Ready | Failed.
///
/// Part of the kernel rather than a subsystem: it has no meaningful place in the
/// game loop's Update/Render order, and every subsystem needs it, so it is reached
/// through Engine::GetAssets(). Engine drives Initialize/Update/Shutdown.
class ENGINE_API AssetSystem {
public:
    /// Constructed by the Engine: loading needs the scheduler, the event dispatcher
    /// and the rendering subsystem, all of which live behind that reference.
    explicit AssetSystem(Engine& engine);
    /// Both the constructor and the destructor need the complete importer types,
    /// which live in an engine-private header.
    ~AssetSystem();

    /// Builds the importers. Nothing here can fail, unlike a subsystem's: the scheduler
    /// and the device are only touched once a load is actually requested.
    void Initialize();
    /// Collects finished background loads and publishes them. Engine calls this once
    /// per Tick, before the subsystems run.
    void Update(const FrameContext& frame);
    void Shutdown();
    /// Drop in-flight work and evict asset-backed GPU resources after clearing
    /// all render-world references. Handles from the old scene become stale.
    void UnloadAll();

    /// Blocking loads, for boot-time assets: the CPU phase may already run on a worker
    /// (started by the Request* inside), the call waits for it and uploads on this thread.
    /// A returned resource retains the value record, not the GPU object: after
    /// UnloadAll its embedded GPU handle must no longer be used. Empty on failure.
    entt::resource<MeshAsset> LoadMesh(std::string_view name, bool centerOnLoad = false);
    entt::resource<TextureAsset> LoadTexture(std::string_view name);
    /// JSON names are exe-relative. Extensionless names gain .material.json / .material-instance.json.
    entt::resource<MaterialAsset> LoadMaterial(std::string_view name);
    entt::resource<MaterialInstanceAsset> LoadMaterialInstance(std::string_view name);
    /// Reimport a saved asset and atomically replace its live GPU contents on the main thread.
    /// Unloaded names are read on their next request. A failure leaves ready values untouched.
    bool Reload(std::string_view name, AssetKind kind, std::string& error);
    /// Names use game/... or engine/...; each stage/entry point/define set is a separate asset.
    entt::resource<ShaderAsset> LoadShader(std::string_view name, ShaderType stage,
                                          ShaderCompileOptions options = {});
    /// Generated HLSL with a virtual shader path for relative includes and diagnostics.
    entt::resource<ShaderAsset> LoadShaderSource(std::string_view virtualPath, std::string source,
                                                ShaderType stage, ShaderCompileOptions options = {});

    /// Starts loading without blocking. Deduplicated: repeated requests for a Loading or
    /// Ready name return the same handle. Invalid() when the name hashes onto a different
    /// asset (logged) ; a Failed attempt must be re-requested explicitly.
    AssetHandle<MeshAsset> RequestMesh(std::string_view name, bool centerOnLoad = false);
    AssetHandle<TextureAsset> RequestTexture(std::string_view name);
    /// Resolve authored UUIDs against metadata; path is a non-authoritative hint.
    AssetHandle<MeshAsset> RequestMesh(const AssetReference& reference);
    AssetHandle<TextureAsset> RequestTexture(const AssetReference& reference);
    AssetHandle<MaterialAsset> RequestMaterial(const AssetReference& reference);
    AssetHandle<MaterialInstanceAsset> RequestMaterialInstance(const AssetReference& reference);
    [[nodiscard]] const AssetCatalog& GetCatalog() const noexcept { return catalog_; }
    /// Rebuilds the catalog from disk.
    /// Importer jobs read the catalog, so they are swapped in update once none read.
    /// A failed scan keeps the previous entries, false and `error` then say why
    bool RescanCatalog(std::string& error);
    /// Why the last scan was not clean; empty when it was.
    [[nodiscard]] const std::string& GetCatalogError() const noexcept { return catalogError_; }
    [[nodiscard]] std::optional<AssetReference> ReferenceFor(AssetHandle<MeshAsset> handle) const;
    [[nodiscard]] std::optional<AssetReference> ReferenceFor(AssetHandle<MaterialInstanceAsset> handle) const;
    /// Ready only after the JSON, declared shaders and referenced assets are ready.
    AssetHandle<MaterialAsset> RequestMaterial(std::string_view name);
    AssetHandle<MaterialInstanceAsset> RequestMaterialInstance(std::string_view name);
    /// Shader handles identify a single stage/entry point/define set. Use the
    /// returned handle with IsReady() / Peek() to query asynchronous compilation.
    AssetHandle<ShaderAsset> RequestShader(std::string_view name, ShaderType stage,
                                          ShaderCompileOptions options = {});
    AssetHandle<ShaderAsset> RequestShaderSource(std::string_view virtualPath, std::string source,
                                                ShaderType stage, ShaderCompileOptions options = {});

    /// Current state of a mesh or texture name; shader variants use their request handle.
    template <typename T>
    [[nodiscard]] AssetState GetState(std::string_view name) const {
        const AssetHandle<T> handle = Table<T>().Find(NormalizeAssetName(name, AssetValueKind<T>::value));
        const typename AssetTable<T>::Record* record = Table<T>().Get(handle);
        return record != nullptr ? record->state : AssetState::Unloaded;
    }

    /// The loaded asset, or an empty resource while it is not Ready.
    /// For shaders, use the handle overload below to distinguish variants.
    template <typename T>
    [[nodiscard]] entt::resource<T> Peek(std::string_view name) {
        return Peek<T>(Table<T>().Find(NormalizeAssetName(name, AssetValueKind<T>::value)));
    }

    /// Generation-checked: a handle whose slot was recycled for another asset reads as empty
    /// instead of returning the wrong asset. A resource taken while the asset was Ready
    /// outlives that recycle: it shares the value, not the record.
    template <typename T>
    [[nodiscard]] entt::resource<T> Peek(const AssetHandle<T> handle) {
        const typename AssetTable<T>::Record* record = Table<T>().Get(handle);
        if (record == nullptr || record->state != AssetState::Ready) {
            return {};
        }
        return record->value;
    }

    template <typename T>
    [[nodiscard]] bool IsReady(const AssetHandle<T> handle) const {
        const typename AssetTable<T>::Record* record = Table<T>().Get(handle);
        return record != nullptr && record->state == AssetState::Ready;
    }

private:
    [[nodiscard]] const CatalogEntry* Resolve(const AssetReference& reference, CatalogAssetKind kind) const;
    static std::string NormalizeAssetName(std::string_view name, AssetKind kind);
    AssetHandle<ShaderAsset> RequestShaderVariant(AssetDesc&& desc);
    template <typename T>
    AssetHandle<T> Request(AssetDesc&& desc);

    /// Waits for the background CPU phase if one is running, uploads on the calling thread.
    template <typename T>
    entt::resource<T> LoadSync(AssetHandle<T> handle);

    /// Queues the CPU phase of one record on the scheduler. `desc` must point into the
    /// stable record storage: workers read it while the main thread keeps streaming frames.
    void DispatchJob(AssetKind kind, const AssetDesc& desc, uint32_t slot, uint32_t generation);
    /// Detaches the scheduled job for one record; main thread only, empty if it is gone.
    /// Slot numbers repeat across the tables, so the kind is part of the match.
    std::unique_ptr<LoadJob> TakeJob(AssetKind kind, uint32_t slot);
    /// Uploads a completed job's payload and publishes or fails the record; returns the
    /// asset resource, empty on failure or when the record went stale. Fires
    /// AssetLoadedEvent / AssetFailedEvent.
    /// False while a completed JSON job is waiting for asynchronous dependencies.
    bool FinishJob(LoadJob& job, uint64_t frameIndex);
    enum class DependencyState { Ready, Waiting, Failed };
    DependencyState ResolveDependencies(LoadJob& job, bool wait);
    bool ReloadReady(LoadJob& job, std::string& error);
    template <typename T>
    entt::resource<T> FinishTyped(const LoadJob& job, AssetTable<T>& table, AssetHandle<T> handle, uint64_t frameIndex);

    /// Resolved per upload, not cached: as a kernel piece this exists before any subsystem
    /// has been registered, so there is nothing to point at in Initialize().
    [[nodiscard]] UploadTransfer MakeTransfer(RenderingSubsystem& rendering) const;
    [[nodiscard]] const IAssetImporter& ImporterFor(AssetKind kind) const;

    template <typename T>
    [[nodiscard]] AssetTable<T>& Table() {
        if constexpr (std::is_same_v<T, MeshAsset>) {
            return meshes_;
        } else if constexpr (std::is_same_v<T, TextureAsset>) {
            return textures_;
        } else if constexpr (std::is_same_v<T, ShaderAsset>) {
            return shaders_;
        } else if constexpr (std::is_same_v<T, MaterialAsset>) {
            return materials_;
        } else if constexpr (std::is_same_v<T, MaterialInstanceAsset>) {
            return materialInstances_;
        } else {
            static_assert(sizeof(T) == 0, "not an asset value type");
        }
    }

    template <typename T>
    [[nodiscard]] const AssetTable<T>& Table() const {
        return const_cast<AssetSystem*>(this)->Table<T>();
    }

    AssetTable<MeshAsset> meshes_;
    AssetTable<TextureAsset> textures_;
    AssetTable<ShaderAsset> shaders_;
    AssetTable<MaterialAsset> materials_;
    AssetTable<MaterialInstanceAsset> materialInstances_;
    AssetCatalog catalog_;
    /// A rescanned catalog waiting for the running importer jobs to finish.
    std::optional<AssetCatalog> pendingCatalog_;
    std::string catalogError_;
    std::array<std::unique_ptr<IAssetImporter>, static_cast<size_t>(AssetKind::Count)> importers_{};
    /// Queued CPU-phase jobs. Touched only from the main thread, so no lock is needed:
    /// workers see just each job's own fields.
    std::deque<std::unique_ptr<LoadJob>> inFlight_;
    Engine& engine_;
};

}  // namespace engine
