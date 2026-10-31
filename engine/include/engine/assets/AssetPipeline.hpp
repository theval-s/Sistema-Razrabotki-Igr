#pragma once

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <TaskScheduler.h>
#include <spdlog/spdlog.h>

#include <engine/rendering/RenderingResourceManager.hpp>
#include <engine/core/ResourceID.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/rendering/resources/ShaderContext.hpp>
#include <engine/rendering/UglyUtils.hpp>
#include <engine/assets/AssetCommon.hpp>
#include <engine/assets/AssetTypes.hpp>
#include <engine/assets/AssetHandle.hpp>

namespace engine {

/// Output of the CPU phase (worker thread): file bytes, parsed geometry, compiled
/// bytecode. Contains no shared registry state.
struct MeshCpu {
    std::vector<ugly_utils::VertData> vertices;
    std::vector<uint32_t> indices;
    Aabb bounds;
};

/// The decoded texture, already a device resource: WIC decoding a 4K image and building its
/// mip chain is the expensive part of a texture load, and it has to happen on the worker.
/// ID3D11Device is free-threaded (never created with SINGLETHREADED), so creating resources
/// from a worker while the main thread submits is supported; only the device context is not.
struct TextureCpu {
    ComPtr<ID3D11ShaderResourceView> view;
};

struct MaterialCpu {
    MaterialDefinition definition;
    // ShadowVS, GBufferVS, GBufferPS. An undeclared hook has no bytecode.
    std::array<CompiledShader, 3> shaders;
};

struct CpuPayload {
    enum class Kind : uint8_t { None, Mesh, Texture, Shader, Material, MaterialInstance };

    Kind kind = Kind::None;
    MeshCpu mesh;
    TextureCpu texture;
    CompiledShader shader;
    MaterialCpu material;
    MaterialInstanceDefinition materialInstance;
};

/// Output of the GPU phase (main thread): handles of the resources that were created.
/// Invalid members mean "this step produced nothing".
struct GpuPayload {
    MeshHandle mesh;
    TextureHandle texture;
    ShaderHandle shader;
    MaterialHandle material;
    MaterialInstanceHandle materialInstance;
};

/// Everything the upload phase may need, resolved on the main thread before the call.
struct UploadTransfer {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    RenderingResourceManager* resources = nullptr;
    MaterialParameters materialParameters; // resolved dependencies and inherited values; main thread only
    MaterialHandle material;              // parent of a material instance
};

/// Two-phase importer: ReadCpu is thread-safe and may run on a worker; UploadGpu must run
/// on the main thread (HandleMap insertion). Neither terminates the process: failures return
/// false with a message. `device` is the free-threaded ID3D11Device, given to ReadCpu so the
/// expensive part of a texture load can happen on the worker; importers that only parse bytes
/// ignore it. It must never be used to touch the device context.
class IAssetImporter {
public:
    virtual ~IAssetImporter() = default;

    [[nodiscard]] virtual bool ReadCpu(const AssetDesc& desc, ID3D11Device* device,
                                       CpuPayload& payload, std::string& error) const = 0;
    [[nodiscard]] virtual bool UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                         GpuPayload& result, std::string& error) const = 0;
};

class AssimpMeshImporter final : public IAssetImporter {
public:
    [[nodiscard]] bool ReadCpu(const AssetDesc& desc, ID3D11Device* device, CpuPayload& payload,
                               std::string& error) const override;
    [[nodiscard]] bool UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                 GpuPayload& result, std::string& error) const override;
};

class TextureImporter final : public IAssetImporter {
public:
    [[nodiscard]] bool ReadCpu(const AssetDesc& desc, ID3D11Device* device, CpuPayload& payload,
                               std::string& error) const override;
    [[nodiscard]] bool UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                 GpuPayload& result, std::string& error) const override;
};

class ShaderImporter final : public IAssetImporter {
public:
    [[nodiscard]] bool ReadCpu(const AssetDesc& desc, ID3D11Device* device, CpuPayload& payload,
                               std::string& error) const override;
    [[nodiscard]] bool UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                 GpuPayload& result, std::string& error) const override;
};

class MaterialImporter final : public IAssetImporter {
public:
    bool ReadCpu(const AssetDesc& desc, ID3D11Device* device, CpuPayload& payload,
                 std::string& error) const override;
    bool UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                   GpuPayload& result, std::string& error) const override;
};

class MaterialInstanceImporter final : public IAssetImporter {
public:
    /// Rebuild inherited parameters without discarding authored overrides or runtime buffers.
    static MaterialParameters Inherit(const MaterialParameters& defaults, const MaterialParameterOverrides& overrides,
                                      TextureHandle explicitTexture, BufferHandle customBuffer);
    bool ReadCpu(const AssetDesc& desc, ID3D11Device* device, CpuPayload& payload,
                 std::string& error) const override;
    bool UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                   GpuPayload& result, std::string& error) const override;
};

/// CPU phase of one asset load, queued on the engine task scheduler. ExecuteRange runs on a
/// worker thread and touches only the job's own payload plus the immutable-while-Loading record
/// description; every device call happens later on the main thread. The job object must be
/// destroyed only once GetIsComplete() was observed (~ICompletable asserts completion).
struct LoadJob final : public enki::ITaskSet {
    AssetKind kind = AssetKind::Mesh;
    ResourceID id = 0;
    std::string name;
    uint32_t slot = 0;
    uint32_t generation = 0;
    const AssetDesc* desc = nullptr;
    const IAssetImporter* importer = nullptr;
    ID3D11Device* device = nullptr;
    CpuPayload payload;
    std::string error;
    bool cpuOk = false;
    bool dependenciesRequested = false;
    AssetHandle<MaterialAsset> materialDependency;
    AssetHandle<TextureAsset> textureDependency;
    MaterialParameters resolvedParameters;
    MaterialHandle resolvedMaterial;
    std::chrono::steady_clock::time_point queuedAt{};

    void ExecuteRange(enki::TaskSetPartition /*range*/, uint32_t threadnum) override {
        const auto start = std::chrono::steady_clock::now();
        cpuOk = importer->ReadCpu(*desc, device, payload, error);
        const auto cpuSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto queueDelaySeconds = std::chrono::duration<double>(start - queuedAt).count();
        // threadnum 0 is the thread that called TaskScheduler::Initialize(), i.e. the main one:
        // a non-zero number here is the proof that the read never blocked the game loop.
        spdlog::info("Asset '{}': CPU phase ran on worker {} ({} s in the pipe, {} s of work)",
                     name, threadnum, queueDelaySeconds, cpuSeconds);
    }
};

}  // namespace engine
