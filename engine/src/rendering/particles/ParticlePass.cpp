#include "ParticlePass.hpp"
#include "../LifecycleFailure.hpp"
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/Content.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/assets/ParticleEffectDefinition.hpp>
#include <engine/rendering/EngineShaders.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine {
void ParticlePass::Reset() {
    tiles_.reset();
    sim_.reset();
    textures_.reset();
    shadows_.reset();
    stats_ = {};
    requests_.clear();
    emission_.clear();
    bursts_.clear();
    textureFailures_.clear();
    step_ = lastBirthRequest_ = 0;
}

void ParticlePass::Destroy(RenderingResourceManager&, RenderingContext&, RenderingResources&) {
    Reset();
    programs_.reset();
    programHooks_.reset();
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

bool ParticlePass::Collect(const RenderWorld& world, RenderingContext& context, double dt, const ParticlePreview* preview) {
    requests_.clear();
    bool births = false;
    for (auto& [owner, emission] : emission_) emission.present = false;
    const auto finite = [](const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    for (const auto& source : world.particleEmitters) {
        if (preview && !preview->allEmitters && source.owner != preview->owner) continue;
        const auto& p = source.settings;
        if (!std::isfinite(p.rate) || p.rate < 0 || !std::isfinite(p.lifetime) || p.lifetime <= 0 ||
            !std::isfinite(p.speed) || p.speed < 0 || !std::isfinite(p.radius) || p.radius < 0 ||
            !std::isfinite(p.spread) || p.spread < 0 || !finite(p.tint) ||
            !finite(source.position) || !finite(source.direction)) continue;
        std::string validation;
        if (!ValidateParticleEffectSettings(source.effect, validation)) continue;
        auto& emission = emission_[source.owner];
        if (emission.present) continue;
        emission.present = true;
        const double accrued = emission.remainder + dt * p.rate;
        double whole;
        emission.remainder = std::modf(accrued, &whole);
        auto count = static_cast<std::uint32_t>(std::min(whole, static_cast<double>(UINT32_MAX)));
        if (const auto it = bursts_.find(source.owner); it != bursts_.end())
            count += std::min(it->second, UINT32_MAX - count);
        std::uint32_t layer{};
        auto& manager = context.GetResourceManager();
        const auto hr = textures_->Resolve(source.texture ? manager.textures_.Get(source.texture).GetSRV() : nullptr, layer);
        if (FAILED(hr)) {
            // An authored texture must never quietly become a different effect. The retained
            // layers still serve draining particles; restart frees them after extensive editing.
            if (!textureFailures_.insert(source.owner).second) continue;
            if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS))
                log::Render().error("Particle emitter {}: all {} particle texture layers are in use; restart particles to reclaim them",
                    source.owner, ParticleTextureLayers - 1);
            else
                log::Render().error("Particle emitter {} texture unavailable (HRESULT {:08X})", source.owner, static_cast<std::uint32_t>(hr));
            continue;
        }
        textureFailures_.erase(source.owner);
        requests_.push_back({source.owner, count, p.maxAlive, p.appearance == 1 ? 0 : p.appearance, p.seed,
            {source.position.x, source.position.y, source.position.z}, p.lifetime,
            {source.direction.x, source.direction.y, source.direction.z}, p.speed,
            {p.tint.x, p.tint.y, p.tint.z}, p.radius, p.spread, source.effect, layer});
        births |= count != 0;
    }
    std::erase_if(emission_, [](const auto& kv) { return !kv.second.present; });
    bursts_.clear(); // don't accumulate requests for a removed emitter
    return births;
}

bool ParticlePass::EnsurePrograms(RenderingContext& context) {
    // Cooked builds read baked bytecode. Source builds watch the project hook so edits apply live.
    HookStamp hooks;
    const auto path = GetProjectParticleHooksPath();
    if (!AssetPathResolver::IsCooked() && content::IsFile(path)) {
        std::error_code error;
        hooks = {true, std::filesystem::last_write_time(path, error), std::filesystem::file_size(path, error)};
    }
    if (programHooks_ == hooks) return programs_.has_value();
    const bool reload = programHooks_.has_value();
    programHooks_ = hooks;
    if (reload) Reset(); // the live helpers hold the previous programs
    programs_.reset();
    auto& manager = context.GetResourceManager();
    const auto compile = [&](EngineShader variant) {
        const auto handle = manager.CompileShader(variant);
        auto shader = manager.shaders_.Get(handle).shader;
        manager.DestroyShader(handle);
        return shader;
    };
    try {
        Programs programs;
        programs.update = compile(EngineShader::ParticleUpdate).computeShader;
        programs.admit = compile(EngineShader::ParticleAdmit).computeShader;
        programs.spawn = compile(EngineShader::ParticleSpawn).computeShader;
        programs.resample = compile(EngineShader::ParticleTextureResample).computeShader;
        programs.tiles = {compile(EngineShader::ParticleBuild).computeShader, compile(EngineShader::ParticleMerge).computeShader,
                          compile(EngineShader::ParticleBin).computeShader, compile(EngineShader::ParticleShade).computeShader};
        programs.shadowVS = compile(EngineShader::ParticleShadowVS).vertexShader;
        programs.shadowPS = compile(EngineShader::ParticleShadowPS).pixelShader;
        programs_ = std::move(programs);
    } catch (const RenderingException& error) {
        // Particles include project HLSL: a mistake there disables particles, not the renderer.
        const auto& failure = error.GetFailure();
        log::Render().error("Particle shaders failed to compile; particles are off until {} changes.\n{}\n{}",
            ToUtf8(path), failure.summary, failure.detail);
        return false;
    }
    if (reload) log::Render().info("Recompiled particle shaders for {}", ToUtf8(path));
    return true;
}

void ParticlePass::Start(RenderingContext& context) {
    auto* device = context.GetDevice();
    const auto& programs = *programs_;
    sim_ = std::make_unique<ParticleSimulation>();
    rendering_detail::Check(sim_->Initialize(device, {programs.update.Get(), programs.admit.Get(), programs.spawn.Get()},
        budget_.maxParticles, budget_.maxEmitters), "CreateParticleSimulation", device);
    textures_ = std::make_unique<ParticleTextures>();
    rendering_detail::Check(textures_->Initialize(device, programs.resample.Get()), "CreateParticleTextures", device);
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

bool ParticlePass::Drained() const {
    return stats_.simulationStep >= lastBirthRequest_ && !stats_.alive;
}

void ParticlePass::Advance(const RenderWorld& world, RenderingContext& context, double dt, const ParticlePreview* preview) {
    if (!std::isfinite(dt) || dt <= 0 || dt > std::numeric_limits<float>::max()) return;
    if (!sim_) {
        const bool requested = std::any_of(world.particleEmitters.begin(), world.particleEmitters.end(), [&](const auto& source) {
            return (!preview || preview->allEmitters || source.owner == preview->owner) &&
                (source.settings.rate > 0 || bursts_.contains(source.owner));
        });
        if (!requested) {
            bursts_.clear(); // requests for absent owners expire even before the first allocation
            return;
        }
    }
    if (!EnsurePrograms(context)) {
        bursts_.clear();
        return;
    }
    if (!sim_) Start(context);
    PollSimulation();
    const bool births = Collect(world, context, dt, preview);
    // A completed zero only proves there's nothing to simulate if no newer birth was requested.
    if (!births && Drained()) return;
    ++step_;
    if (births) lastBirthRequest_ = step_;
    rendering_detail::Check(sim_->Step(static_cast<float>(dt), requests_), "StepParticles", context.GetDevice());
}

void ParticlePass::RenderShadows(const RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    resources.particleShadowView = nullptr;
    if (!sim_ || Drained() || world.directionalLight.intensity <= 0) return;
    const auto views = sim_->RenderViews();
    if (!views.castsShadows) return;
    auto* device = context.GetDevice();
    auto& manager = context.GetResourceManager();
    if (!shadows_) {
        shadows_ = std::make_unique<ParticleShadows>();
        rendering_detail::Check(shadows_->Initialize(device, {programs_->shadowVS.Get(), programs_->shadowPS.Get()},
            budget_.maxParticles), "CreateParticleShadows", device);
    }
    rendering_detail::Check(shadows_->Render(views, textures_->View(), resources.cascadeInfo,
        manager.textures_.Get(resources.shadowTexture).GetSRV()), "RenderParticleShadows", device);
    resources.particleShadowView = shadows_->View();
}

void ParticlePass::Render(RenderWorld&, RenderingContext& context, RenderingResources& resources) {
    if (!sim_) return;
    PollSimulation();
    // Poll even if this view won't draw: the last submitted tile sample may have
    // finished after the last particle died or the host selected a debug view.
    if (tiles_) {
        const auto occupancy = tiles_->Latest();
        stats_.tileFrame = occupancy.frame;
        stats_.peakTileOccupancy = occupancy.peakOccupancy;
        stats_.multiBatchTiles = occupancy.batchingTiles;
    }
    if (Drained()) return;
    auto* device = context.GetDevice();
    if (resources.debugView != RenderDebugView::Final) return;
    if (!tiles_) {
        const auto& programs = programs_->tiles;
        tiles_ = std::make_unique<ParticleTiles>();
        rendering_detail::Check(tiles_->Initialize(device,
            {programs[0].Get(), programs[1].Get(), programs[2].Get(), programs[3].Get()},
            budget_.maxParticles, resources.screenWith, resources.screenHeight), "CreateParticleTiles", device);
    }
    ParticleTiles::Camera camera;
    const auto& view = resources.viewCBuffer.GetData();
    std::memcpy(camera.view, &view.viewMatrix, sizeof(camera.view));
    std::memcpy(camera.projection, &view.projectionMatrix, sizeof(camera.projection));
    auto& textures = context.GetResourceManager().textures_;
    rendering_detail::Check(tiles_->Render(sim_->RenderViews(), camera,
        textures.Get(resources.gBuffer.depthTexture).GetSRV(), textures.Get(resources.gBuffer.resultTexture).GetSRV(), textures_->View()),
        "RenderParticleTiles", device);
}

void ParticleShadowPass::Render(RenderWorld& world, RenderingContext& context, RenderingResources& resources) {
    particles_.RenderShadows(world, context, resources);
    // The light-view draws own raw D3D state, including multiplicative blending. Restore all
    // defaults, not just SRVs, before the opaque G-buffer draws.
    context.ClearState();
    resources.BindCBuffers(context);
    context.BindSamplers();
}
}
