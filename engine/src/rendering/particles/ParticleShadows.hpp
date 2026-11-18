#pragma once
#include "ParticleSimulation.hpp"
struct ID3D11VertexShader;
struct ID3D11PixelShader;
namespace engine {
struct CascadeInfo;
class ParticleShadows {
public:
    struct Programs { ID3D11VertexShader* vertex{}; ID3D11PixelShader* pixel{}; };
    ParticleShadows();
    ~ParticleShadows();
    long Initialize(ID3D11Device*, Programs, std::uint32_t capacity);
    // Four ParticleShadowMapSize-square transmittance slices. Size is sent to HLSL in b0.
    // Raw immediate-context pass; clobbers graphics
    // pipeline, viewport, VS/PS b0,t0..9,s0 and OM. Caller rebinds subsequent pass state.
    // Null/empty views clear to identity. Uses untransposed current cascade matrices.
    // Unbind conflicting views before Render; rebind engine constant buffers afterward.
    long Render(ParticleSimulation::Views, ID3D11ShaderResourceView* textures,
                const CascadeInfo&, ID3D11ShaderResourceView* opaqueDepth);
    ID3D11ShaderResourceView* View() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
