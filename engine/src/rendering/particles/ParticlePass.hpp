#pragma once

#include <engine/rendering/passes/RenderPass.hpp>
#include "ParticleTiles.hpp"
#include <unordered_map>

namespace engine {
class ParticlePass final : public RenderPass {
public:
    ParticlePass(const ParticleBudget& budget, ParticleStats& stats) : budget_(budget), stats_(stats) {}
    void Reset();
    void Burst(std::uint64_t owner, std::uint32_t count);
    void Initialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) override {}
    void Destroy(RenderingResourceManager&, RenderingContext&, RenderingResources&) override { Reset(); }
    void Render(RenderWorld&, RenderingContext&, RenderingResources&) override;
    bool PreReinitialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) override;
private:
    void Start(RenderingContext& context);
    void PollSimulation();
    bool Collect(const RenderWorld&, double dt);

    const ParticleBudget& budget_;
    ParticleStats& stats_;
    std::unique_ptr<ParticleSimulation> sim_;
    std::unique_ptr<ParticleTiles> tiles_;
    std::array<ComPtr<ID3D11ComputeShader>, 4> tilePrograms_;
    struct Emission { double remainder{}; bool present{}; };
    std::unordered_map<std::uint64_t, Emission> emission_;
    std::unordered_map<std::uint64_t, std::uint32_t> bursts_;
    std::vector<ParticleSimulation::Emitter> requests_;
    std::uint64_t step_{}, lastBirthRequest_{};
};
}
