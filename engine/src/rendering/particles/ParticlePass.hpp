#pragma once

#include <engine/rendering/passes/RenderPass.hpp>
#include "ParticleTiles.hpp"
#include "ParticleTextures.hpp"
#include "ParticleShadows.hpp"
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace engine {
// Bridges engine assets and shaders to the private D3D11 helpers (simulation, texture resampling,
// tile composition, light-view shadows). They make native structured-buffer/UAV and per-slice
// calls that RenderingContext doesn't wrap; each documents the state it clobbers.
class ParticlePass final : public RenderPass {
public:
    ParticlePass(const ParticleBudget& budget, ParticleStats& stats) : budget_(budget), stats_(stats) {}
    /// Clears every particle and the telemetry. Compiled programs are kept.
    void Reset();
    void Burst(std::uint64_t owner, std::uint32_t count);
    void Advance(const RenderWorld&, RenderingContext&, double dt, const ParticlePreview* preview);
    void RenderShadows(const RenderWorld&, RenderingContext&, RenderingResources&);
    void Initialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) override {}
    void Destroy(RenderingResourceManager&, RenderingContext&, RenderingResources&) override;
    void Render(RenderWorld&, RenderingContext&, RenderingResources&) override;
    bool PreReinitialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) override;
private:
    struct Programs {
        ComPtr<ID3D11ComputeShader> update, admit, spawn, resample;
        std::array<ComPtr<ID3D11ComputeShader>, 4> tiles;
        ComPtr<ID3D11VertexShader> shadowVS;
        ComPtr<ID3D11PixelShader> shadowPS;
    };
    // What the project's hook file looked like when the programs were compiled.
    struct HookStamp {
        bool exists{};
        std::filesystem::file_time_type time{};
        std::uintmax_t size{};
        bool operator==(const HookStamp&) const = default;
    };
    /// Compiles on first use and again whenever the project hook changes. False while it doesn't compile.
    bool EnsurePrograms(RenderingContext& context);
    void Start(RenderingContext& context);
    void PollSimulation();
    bool Collect(const RenderWorld&, RenderingContext&, double dt, const ParticlePreview* preview);
    bool Drained() const;

    const ParticleBudget& budget_;
    ParticleStats& stats_;
    std::optional<Programs> programs_;
    std::optional<HookStamp> programHooks_; // empty until the first compile attempt
    std::unique_ptr<ParticleSimulation> sim_;
    std::unique_ptr<ParticleTiles> tiles_;
    std::unique_ptr<ParticleTextures> textures_;
    std::unique_ptr<ParticleShadows> shadows_;
    struct Emission { double remainder{}; bool present{}; };
    std::unordered_map<std::uint64_t, Emission> emission_;
    std::unordered_map<std::uint64_t, std::uint32_t> bursts_;
    std::vector<ParticleSimulation::Emitter> requests_;
    std::unordered_set<std::uint64_t> textureFailures_;
    std::uint64_t step_{}, lastBirthRequest_{};
};

/// Particle transmittance from the directional light. Runs right after ShadowCSMPass, which it
/// reads, and restores engine state for the passes that follow.
class ParticleShadowPass final : public RenderPass {
public:
    explicit ParticleShadowPass(ParticlePass& particles) : particles_(particles) {}
    void Initialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) override {}
    void Destroy(RenderingResourceManager&, RenderingContext&, RenderingResources&) override {}
    void Render(RenderWorld&, RenderingContext&, RenderingResources&) override;
private:
    ParticlePass& particles_;
};
}
