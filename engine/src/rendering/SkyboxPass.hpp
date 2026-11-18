#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <d3d11.h>
#include <wrl/client.h>
#include <engine/rendering/passes/RenderPass.hpp>

namespace engine {
/// Matches shaders/SkyboxConstants.hlsli, b0. Matrices are row-major, uploaded
/// without transposition; HLSL uses mul(rowVector, matrix).
struct alignas(16) SkyboxConstants {
    DirectX::SimpleMath::Matrix inverseProjection;
    /// Inverse camera view rotation * CreateRotationY(-yawRadians); no translation.
    DirectX::SimpleMath::Matrix viewToCubemap;
    DirectX::SimpleMath::Vector3 tint{1.0f, 1.0f, 1.0f};
    float brightness{1.0f};
    DirectX::SimpleMath::Vector3 fallbackColor{0.0f, 0.0f, 0.0f};
    std::uint32_t useCubemap{0};
};
static_assert(std::is_standard_layout_v<SkyboxConstants>);
static_assert(std::is_trivially_copyable_v<SkyboxConstants>);
static_assert(alignof(SkyboxConstants) == 16);
static_assert(offsetof(SkyboxConstants, inverseProjection) == 0);
static_assert(offsetof(SkyboxConstants, viewToCubemap) == 64);
static_assert(offsetof(SkyboxConstants, tint) == 128);
static_assert(offsetof(SkyboxConstants, brightness) == 140);
static_assert(offsetof(SkyboxConstants, fallbackColor) == 144);
static_assert(offsetof(SkyboxConstants, useCubemap) == 156);
static_assert(sizeof(SkyboxConstants) == 160);

/// After LightPass, before ParticlePass: fullscreen triangle at reverse-Z depth 0,
/// GREATER_EQUAL depth test, zero depth writes, opaque blending, no vertex buffer.
/// Reconstruct a view ray using inverseProjection (near-plane clip z=1 avoids
/// infinite-far division), then transform with viewToCubemap using w=0.
/// Sample TextureCube t0/sampler s0; write linear sample * tint * brightness into
/// SceneResult (or linear fallback). PresentPass performs existing tone mapping
/// exactly once; this pass does not tone map or change output encoding.
/// Restore renderer state/bindings for following passes; unbind the cubemap SRV.
class SkyboxPass final : public RenderPass {
public:
    void Initialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) override;
    void Destroy(RenderingResourceManager&, RenderingContext&, RenderingResources&) override;
    void Render(RenderWorld&, RenderingContext&, RenderingResources&) override;
private:
    ShaderHandle vertexShader_;
    ShaderHandle pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
} // namespace engine
