#pragma once
#include <cstdint>
#include <memory>
struct ID3D11Device;
struct ID3D11ComputeShader;
struct ID3D11ShaderResourceView;
namespace engine {
class ParticleTextures {
public:
    ParticleTextures();
    ~ParticleTextures();
    long Initialize(ID3D11Device*, ID3D11ComputeShader* resample);
    // Single-mip linear RGBA16F array, bilinear filtering. Null source selects soft disc.
    // Retains SRVs until reinitialize. Exhaustion returns HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)
    // and UINT32_MAX, never an unrelated layer. Caller must unbind array before Resolve.
    // Clobbers CS shader/b0/t0/u0/s0 and leaves these null. Same-device float-sampleable Texture2D SRVs.
    long Resolve(ID3D11ShaderResourceView* source, std::uint32_t& layer);
    ID3D11ShaderResourceView* View() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
