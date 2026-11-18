#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/rendering/RenderingResourceManager.hpp>
#include <gtest/gtest.h>

namespace {
using namespace engine;

std::filesystem::path DiagnosticPath() {
    return AssetPathResolver::GetRoot() / "rendering-diagnostics-fixture" / L"shader-\u0442\u0435\u0441\u0442.hlsl";
}

TEST(RenderingResourceFailures, CompilerFailureOwnsSourceVariantAndFullOutput) {
    const auto path = DiagnosticPath();
    try {
        (void)ShaderContext::CompileSource("float4 CustomEntry() : SV_TARGET { return UnknownDiagnosticSymbol; }",
            path, ShaderType::Pixel, {.entryPoint = "CustomEntry", .defines = {{"FIRST", "7"}, {"SECOND", "9"}}});
        FAIL() << "Invalid HLSL unexpectedly compiled";
    } catch (const RenderingException& e) {
        const auto failure = e.GetFailure(); // Copy remains valid independently of exception/compiler blobs.
        EXPECT_EQ(failure.operation, "D3DCompile");
        EXPECT_EQ(failure.path, path);
        EXPECT_NE(failure.detail.find("UnknownDiagnosticSymbol"), std::string::npos);
        ASSERT_TRUE(failure.nativeError);
        EXPECT_LT(*failure.nativeError, 0);
        ASSERT_TRUE(failure.shader);
        EXPECT_EQ(failure.shader->stage, "pixel");
        EXPECT_EQ(failure.shader->entryPoint, "CustomEntry");
        EXPECT_EQ(failure.shader->target, "ps_5_0");
        ASSERT_EQ(failure.shader->defines.size(), 2u);
        EXPECT_EQ(failure.shader->defines[0], (std::pair<std::string, std::string>{"FIRST", "7"}));
        EXPECT_EQ(failure.shader->defines[1], (std::pair<std::string, std::string>{"SECOND", "9"}));
        EXPECT_EQ(failure.detail.find('\0'), std::string::npos);
    }
}

TEST(RenderingResourceFailures, MissingSourceAndIncludeRetainReadDetails) {
    const auto path = DiagnosticPath(); // A virtual fixture: no checked-in files are modified.
    try {
        (void)ShaderContext::CompileCpu(path, ShaderType::Vertex);
        FAIL() << "Missing source unexpectedly compiled";
    } catch (const RenderingException& e) {
        EXPECT_EQ(e.GetFailure().operation, "ReadShaderSource");
        EXPECT_EQ(e.GetFailure().path, path);
        EXPECT_NE(e.GetFailure().detail.find("cannot read"), std::string::npos);
        ASSERT_TRUE(e.GetFailure().shader);
        EXPECT_EQ(e.GetFailure().shader->entryPoint, "VSMain");
    }
    try {
        (void)ShaderContext::CompileSource("#include \"missing-diagnostic-include.hlsl\"\n", path, ShaderType::Vertex);
        FAIL() << "Missing include unexpectedly compiled";
    } catch (const RenderingException& e) {
        EXPECT_EQ(e.GetFailure().operation, "D3DCompile");
        EXPECT_EQ(e.GetFailure().path, path);
        EXPECT_NE(e.GetFailure().detail.find("missing-diagnostic-include.hlsl"), std::string::npos);
        EXPECT_NE(e.GetFailure().detail.find("cannot read"), std::string::npos);
        EXPECT_NE(e.GetFailure().detail.find(ToUtf8(path.parent_path())), std::string::npos);
    }
}

TEST(RenderingResourceFailures, MissingDeviceThrowsAndImporterRetainsStructuredFields) {
    RenderingResourceManager manager;
    EXPECT_THROW(manager.Initialize(nullptr), RenderingException);
    EXPECT_THROW((void)manager.CreateBuffer({.size = 16}, nullptr), RenderingException);
    EXPECT_THROW((void)manager.CreateTexture({}), RenderingException);
    CpuPayload cpu;
    cpu.kind = CpuPayload::Kind::Shader;
    cpu.shader = ShaderContext::CompileSource("float4 PSMain() : SV_TARGET { return 1; }",
        DiagnosticPath(), ShaderType::Pixel, {.defines = {{"DIAGNOSTIC_DEFINE", "17"}}});
    GpuPayload gpu;
    std::string error;
    EXPECT_FALSE(ShaderImporter{}.UploadGpu({.resources = &manager}, cpu, gpu, error));
    EXPECT_FALSE(gpu.shader);
    for (const auto* field : {"FinalizeShader", "Path:", "Native error:", "pixel", "PSMain", "ps_5_0", "DIAGNOSTIC_DEFINE=17"})
        EXPECT_NE(error.find(field), std::string::npos) << error;
    manager.Shutdown();
    manager.Shutdown();
}

TEST(RenderingResourceFailures, ShaderReadBoundaryPreservesCompilerDetailsForWorkerAndSyncCallers) {
    AssetDesc desc;
    desc.name = "invalid diagnostic shader";
    desc.path = DiagnosticPath();
    desc.shaderStage = ShaderType::Pixel;
    desc.shaderOptions = {.entryPoint = "BrokenEntry", .defines = {{"IMPORT_VARIANT", "23"}}};
    desc.shaderFromMemory = true;
    desc.shaderSource = "float4 BrokenEntry() : SV_TARGET { return MissingImporterSymbol; }";
    CpuPayload payload;
    std::string error;
    EXPECT_FALSE(ShaderImporter{}.ReadCpu(desc, nullptr, payload, error));
    for (const auto* field : {"D3DCompile", "Path:", "Native error:", "pixel", "BrokenEntry", "ps_5_0",
                             "IMPORT_VARIANT=23", "MissingImporterSymbol"})
        EXPECT_NE(error.find(field), std::string::npos) << error;
    EXPECT_EQ(payload.kind, CpuPayload::Kind::None);
}

class RenderingResourceWarpFailures : public ::testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    RenderingResourceManager manager;
    void SetUp() override {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, nullptr))) GTEST_SKIP() << "D3D11 WARP unavailable";
        manager.Initialize(device.Get());
    }
};

TEST_F(RenderingResourceWarpFailures, ComputeImporterReflectsAndFinalizes) {
    AssetDesc desc;
    desc.path = DiagnosticPath();
    desc.shaderStage = ShaderType::Compute;
    desc.shaderFromMemory = true;
    desc.shaderSource = R"(
RWStructuredBuffer<uint> outputData : register(u2);
[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { outputData[id.x] = id.x; }
)";
    CpuPayload payload;
    std::string error;
    ASSERT_TRUE(ShaderImporter{}.ReadCpu(desc, nullptr, payload, error)) << error;
    EXPECT_EQ(payload.shader.type, ShaderType::Compute);
    EXPECT_EQ(payload.shader.diagnosticContext.stage, "compute");
    EXPECT_EQ(payload.shader.diagnosticContext.entryPoint, "CSMain");
    EXPECT_EQ(payload.shader.diagnosticContext.target, "cs_5_0");
    ASSERT_EQ(payload.shader.bindings.size(), 1u);
    EXPECT_EQ(payload.shader.bindings[0].name, "outputData");
    EXPECT_EQ(payload.shader.bindings[0].slot, 2);
    EXPECT_EQ(payload.shader.bindings[0].type, D3D_SIT_UAV_RWSTRUCTURED);
    InputLayoutCache layouts;
    const auto shader = ShaderContext::FinalizeOnDevice(payload.shader, device.Get(), layouts);
    EXPECT_TRUE(shader.shader.computeShader);
    EXPECT_FALSE(shader.shader.vertexShader);
    GpuPayload gpu;
    ASSERT_TRUE(ShaderImporter{}.UploadGpu({.resources = &manager}, payload, gpu, error)) << error;
    EXPECT_TRUE(gpu.shader);
}

TEST_F(RenderingResourceWarpFailures, InvalidRequiredViewDoesNotPublishTextureAndRetryWorks) {
    TextureDesc desc;
    desc.srvFormat = DXGI_FORMAT_R32_FLOAT; // Incompatible with the RGBA8 resource, after CreateTexture2D succeeds.
    try {
        (void)manager.CreateTexture(desc);
        FAIL() << "Incompatible view unexpectedly succeeded";
    } catch (const RenderingException& e) {
        EXPECT_EQ(e.GetFailure().operation, "CreateShaderResourceView");
        ASSERT_TRUE(e.GetFailure().nativeError);
        EXPECT_LT(*e.GetFailure().nativeError, 0);
    }
    const auto texture = manager.CreateTexture({});
    EXPECT_EQ(texture.id, 0u); // Failed creation never entered the handle table.
    EXPECT_TRUE(manager.DestroyTexture(texture));
    EXPECT_FALSE(manager.DestroyTexture(texture));
    // Both writable and read-only DSVs are mandatory, including default view format.
    const auto depth = manager.CreateTexture({.format = DXGI_FORMAT_D32_FLOAT, .usage = TextureUsage::DepthStencil});
    EXPECT_TRUE(manager.DestroyTexture(depth));
}

TEST_F(RenderingResourceWarpFailures, MeshImporterRollsBackVertexBufferWhenIndexCreationFails) {
    CpuPayload cpu;
    cpu.kind = CpuPayload::Kind::Mesh;
    cpu.mesh.vertices.resize(3);
    cpu.mesh.sections.push_back({.vertexCount = 3});
    // Empty indices fail only after the vertex buffer has been registered.
    GpuPayload gpu;
    std::string error;
    EXPECT_FALSE(AssimpMeshImporter{}.UploadGpu({.resources = &manager}, cpu, gpu, error));
    EXPECT_TRUE(gpu.meshSections.empty());
    EXPECT_NE(error.find("CreateBuffer"), std::string::npos);
    const auto buffer = manager.CreateVertexBuffer<ugly_utils::VertData>(cpu.mesh.vertices);
    EXPECT_EQ(buffer.id, 0u);
    EXPECT_NE(buffer.generation, 0);
    EXPECT_TRUE(manager.DestroyBuffer(buffer));
}

TEST_F(RenderingResourceWarpFailures, MeshImporterRollsBackEarlierSectionAndCurrentBuffersBeforeRetry) {
    CpuPayload cpu;
    cpu.kind = CpuPayload::Kind::Mesh;
    cpu.mesh.vertices.resize(6);
    cpu.mesh.indices = {0, 1, 2};
    cpu.mesh.sections = {
        {.vertexCount = 3, .indexCount = 3, .materialSlot = 0},
        {.firstVertex = 3, .vertexCount = 3, .firstIndex = 3, .materialSlot = 1}
    };
    // The first section registers a mesh and two buffers. The second registers
    // its vertex buffer, then its empty index buffer fails native creation.
    GpuPayload gpu;
    std::string error;
    EXPECT_FALSE(AssimpMeshImporter{}.UploadGpu({.resources = &manager}, cpu, gpu, error));
    EXPECT_TRUE(gpu.meshSections.empty());
    EXPECT_NE(error.find("CreateBuffer"), std::string::npos) << error;
    EXPECT_NE(error.find("Native error:"), std::string::npos) << error;
    // Fresh-manager slots identify every acquired object. These removals must
    // already have happened, including the buffers owned by the first mesh.
    EXPECT_FALSE(manager.DestroyMesh(MeshHandle{0, 0}));
    for (uint32_t id = 0; id < 3; ++id)
        EXPECT_FALSE(manager.DestroyBuffer(BufferHandle{id, 0})) << "buffer " << id;

    cpu.mesh.indices.insert(cpu.mesh.indices.end(), {3, 4, 5});
    cpu.mesh.sections[1].indexCount = 3;
    ASSERT_TRUE(AssimpMeshImporter{}.UploadGpu({.resources = &manager}, cpu, gpu, error)) << error;
    ASSERT_EQ(gpu.meshSections.size(), 2u);
    EXPECT_EQ(gpu.meshSections[0].materialSlot, 0u);
    EXPECT_EQ(gpu.meshSections[1].materialSlot, 1u);
    EXPECT_EQ(gpu.meshSections[0].mesh.id, 0u);
    EXPECT_NE(gpu.meshSections[0].mesh.generation, 0);
    EXPECT_FALSE(manager.DestroyMesh(MeshHandle{0, 0})); // A retired handle cannot remove the retry.
    for (const auto& section : gpu.meshSections) {
        EXPECT_TRUE(manager.DestroyMesh(section.mesh));
        EXPECT_FALSE(manager.DestroyMesh(section.mesh));
    }
}

TEST_F(RenderingResourceWarpFailures, MaterialImporterRollsBackEarlierVariantOnLaterDeviceFailure) {
    CpuPayload cpu;
    cpu.kind = CpuPayload::Kind::Material;
    cpu.material.definition.hooks = MaterialHook::ShadowVS | MaterialHook::GBufferVS;
    cpu.material.shaders[0] = ShaderContext::CompileSource(
        "float4 VSMain(float4 p : POSITION) : SV_POSITION { return p; }", DiagnosticPath(), ShaderType::Vertex);
    // Missing bytecode in the next required stage causes deterministic FinalizeShader failure.
    cpu.material.shaders[1].sourcePath = DiagnosticPath();
    cpu.material.shaders[1].diagnosticContext = cpu.material.shaders[0].diagnosticContext;
    GpuPayload gpu;
    std::string error;
    EXPECT_FALSE(MaterialImporter{}.UploadGpu({.resources = &manager}, cpu, gpu, error));
    EXPECT_FALSE(gpu.material);
    EXPECT_NE(error.find("FinalizeShader"), std::string::npos);
    const auto shader = manager.RegisterCompiledShader(cpu.material.shaders[0]);
    EXPECT_EQ(shader.id, 0u);
    EXPECT_NE(shader.generation, 0);
    EXPECT_TRUE(manager.DestroyShader(shader));
}

TEST_F(RenderingResourceWarpFailures, DeviceShaderFailureRetainsOriginalVariant) {
    auto compiled = ShaderContext::CompileSource("float4 PSMain() : SV_TARGET { return 1; }",
        DiagnosticPath(), ShaderType::Pixel, {.defines = {{"DEVICE_VARIANT", "3"}}});
    compiled.type = ShaderType::Vertex; // Valid bytecode for the wrong native creation API.
    try {
        (void)manager.RegisterCompiledShader(compiled);
        FAIL() << "Wrong-stage bytecode unexpectedly finalized";
    } catch (const RenderingException& e) {
        const auto& failure = e.GetFailure();
        EXPECT_EQ(failure.operation, "CreateVertexShader");
        EXPECT_EQ(failure.path, DiagnosticPath());
        ASSERT_TRUE(failure.nativeError);
        EXPECT_LT(*failure.nativeError, 0);
        ASSERT_TRUE(failure.shader);
        EXPECT_EQ(failure.shader->stage, "pixel");
        EXPECT_EQ(failure.shader->defines.front().first, "DEVICE_VARIANT");
    }
}
} // namespace
