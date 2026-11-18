#include "ParticlePass.hpp"
#include "../LifecycleFailure.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine {
void ParticlePass::Reset() {
    tiles_.reset();
    sim_.reset();
    tilePrograms_ = {};
    stats_ = {};
    requests_.clear();
    emission_.clear();
    bursts_.clear();
    step_ = lastBirthRequest_ = 0;
}

bool ParticlePass::PreReinitialize(RenderingResourceManager&, RenderingContext&, RenderingResources&) {
    tiles_.reset(); // only the view-dependent resources change; live particles survive resize
    stats_.tileFrame = stats_.peakTileOccupancy = stats_.multiBatchTiles = 0;
    return true;
}

void ParticlePass::Burst(std::uint64_t owner, std::uint32_t count) {
    auto& pending = bursts_[owner];
    pending += std::min(count, UINT32_MAX - pending);
}

bool ParticlePass::Collect(const RenderWorld& world, double dt) {
    requests_.clear();
    bool births = false;
    for (auto& [owner, emission] : emission_) emission.present = false;
    const auto finite = [](const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    for (const auto& source : world.particleEmitters) {
        const auto& p = source.settings;
        if (!std::isfinite(p.rate) || p.rate < 0 || !std::isfinite(p.lifetime) || p.lifetime <= 0 ||
            !std::isfinite(p.speed) || p.speed < 0 || !std::isfinite(p.radius) || p.radius < 0 ||
            !std::isfinite(p.spread) || p.spread < 0 || !finite(p.tint) ||
            !finite(source.position) || !finite(source.direction)) continue;
        auto& emission = emission_[source.owner];
        if (emission.present) continue;
        emission.present = true;
        const double accrued = emission.remainder + dt * p.rate;
        double whole;
        emission.remainder = std::modf(accrued, &whole);
        auto count = static_cast<std::uint32_t>(std::min(whole, static_cast<double>(UINT32_MAX)));
        if (const auto it = bursts_.find(source.owner); it != bursts_.end())
            count += std::min(it->second, UINT32_MAX - count);
        requests_.push_back({source.owner, count, p.maxAlive, p.appearance, p.seed,
            {source.position.x, source.position.y, source.position.z}, p.lifetime,
            {source.direction.x, source.direction.y, source.direction.z}, p.speed,
            {p.tint.x, p.tint.y, p.tint.z}, p.radius, p.spread});
        births |= count != 0;
    }
    std::erase_if(emission_, [](const auto& kv) { return !kv.second.present; });
    bursts_.clear(); // don't accumulate requests for a removed emitter
    return births;
}

void ParticlePass::Start(RenderingContext& context) {
    auto& manager = context.GetResourceManager();
    auto program = [&](EngineShader variant) {
        const auto handle = manager.CompileShader(variant);
        auto shader = manager.shaders_.Get(handle).shader.computeShader;
        manager.DestroyShader(handle);
        return shader;
    };
    auto update = program(EngineShader::ParticleUpdate), admit = program(EngineShader::ParticleAdmit),
         spawn = program(EngineShader::ParticleSpawn);
    tilePrograms_ = {program(EngineShader::ParticleBuild), program(EngineShader::ParticleMerge),
                     program(EngineShader::ParticleBin), program(EngineShader::ParticleShade)};
    sim_ = std::make_unique<ParticleSimulation>();
    auto* device = context.GetDevice();
    rendering_detail::Check(sim_->Initialize(device, {update.Get(), admit.Get(), spawn.Get()},
        budget_.maxParticles, budget_.maxEmitters), "CreateParticleSimulation", device);
}

void ParticlePass::PollSimulation() {
    if (const auto* s = sim_->Latest(); s && s->frame != stats_.simulationStep) {
        stats_.simulationStep = s->frame;
        stats_.alive = s->alive;
        stats_.rejected = s->rejected;
        stats_.emitters.clear();
        for (const auto& row : s->emitters) stats_.emitters.push_back({row.owner, row.alive, row.rejected});
    }
}

void ParticlePass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    const double dt = resources.simulationDeltaSeconds;
    const bool advance = dt > 0 && dt <= std::numeric_limits<float>::max();
    const bool births = advance && Collect(world, dt);
    if (!sim_) {
        if (!births) return;
        Start(context);
    }
    PollSimulation();
    // Poll even if this view won't draw: the last submitted tile sample may have
    // finished after the last particle died or the host selected a debug view.
    if (tiles_) {
        const auto occupancy = tiles_->Latest();
        stats_.tileFrame = occupancy.frame;
        stats_.peakTileOccupancy = occupancy.peakOccupancy;
        stats_.multiBatchTiles = occupancy.batchingTiles;
    }
    // A completed zero only proves there's nothing to render if no newer birth was requested.
    const bool drained = stats_.simulationStep >= lastBirthRequest_ && !stats_.alive;
    if (!births && drained) return;
    auto* device = context.GetDevice();
    if (advance) {
        ++step_;
        if (births) lastBirthRequest_ = step_;
        rendering_detail::Check(sim_->Step(static_cast<float>(dt), requests_), "StepParticles", device);
    }
    if (resources.debugView != RenderDebugView::Final) return;
    if (!tiles_) {
        tiles_ = std::make_unique<ParticleTiles>();
        rendering_detail::Check(tiles_->Initialize(device,
            {tilePrograms_[0].Get(), tilePrograms_[1].Get(), tilePrograms_[2].Get(), tilePrograms_[3].Get()},
            budget_.maxParticles, resources.screenWith, resources.screenHeight), "CreateParticleTiles", device);
    }
    ParticleTiles::Camera camera;
    const auto& view = resources.viewCBuffer.GetData();
    std::memcpy(camera.view, &view.viewMatrix, sizeof(camera.view));
    std::memcpy(camera.projection, &view.projectionMatrix, sizeof(camera.projection));
    auto& textures = context.GetResourceManager().textures_;
    rendering_detail::Check(tiles_->Render(sim_->RenderViews(), camera,
        textures.Get(resources.gBuffer.depthTexture).GetSRV(), textures.Get(resources.gBuffer.resultTexture).GetSRV()),
        "RenderParticleTiles", device);
}
}
