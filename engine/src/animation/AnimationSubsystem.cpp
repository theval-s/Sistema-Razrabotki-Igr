#include <engine/animation/AnimationSubsystem.hpp>
#include "AnimationData.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>

namespace engine {
namespace {
using Matrix = DirectX::SimpleMath::Matrix;
using Vector3 = DirectX::SimpleMath::Vector3;

void Grow(Aabb& bounds, bool& empty, const Vector3& point) {
    if (empty) { bounds = {point, point}; empty = false; }
    else { bounds.min = Vector3::Min(bounds.min, point); bounds.max = Vector3::Max(bounds.max, point); }
}

struct Player {
    AssetReference reference;
    AssetHandle<AnimationClipAsset> handle;
    entt::resource<AnimationClipAsset> clip;
    ozz::animation::SamplingJob::Context context;
    std::vector<ozz::math::SoaTransform> locals;
    double time = 0;
    bool loop = true;

    void Advance(double delta) {
        if (!clip || !std::isfinite(delta)) return;
        time = loop ? std::fmod(time + delta, static_cast<double>(clip->duration))
                    : std::min(time + delta, static_cast<double>(clip->duration));
    }

    bool Sample() {
        ozz::animation::SamplingJob job;
        job.animation = &clip->data->animation;
        job.context = &context;
        job.ratio = static_cast<float>(time / clip->duration);
        job.output = ozz::make_span(locals);
        return job.Run();
    }
};

struct Instance {
    Animator authored;
    AssetHandle<SkeletonAsset> skeletonHandle;
    entt::resource<SkeletonAsset> skeleton;
    std::unique_ptr<Player> current;
    std::unique_ptr<Player> target;
    std::vector<ozz::math::SoaTransform> blended;
    std::vector<ozz::math::Float4x4> models;
    std::shared_ptr<const AnimationPose> pose;
    double fadeElapsed = 0;
    float fadeDuration = 0;
    float speed = 1;
    bool playing = true;
    std::string error;
};

std::unique_ptr<Player> RequestPlayer(AssetSystem& assets, const AssetReference& clip, bool loop) {
    auto result = std::make_unique<Player>();
    result->reference = clip;
    result->handle = assets.RequestAnimationClip(clip);
    result->loop = loop;
    return result;
}

enum class LoadResult { Waiting, Ready, Failed };
LoadResult ResolvePlayer(Player& player, const SkeletonAsset& skeleton, AssetSystem& assets, std::string& error) {
    if (player.clip) return LoadResult::Ready;
    player.clip = assets.Peek(player.handle);
    if (!player.clip) {
        if (assets.GetState(player.handle) == AssetState::Loading) return LoadResult::Waiting;
        error = "animation clip failed to load; see the asset log";
        return LoadResult::Failed;
    }
    if (!player.clip->data || player.clip->skeletonSignature != skeleton.signature ||
        player.clip->data->animation.num_tracks() != skeleton.data->skeleton.num_joints() ||
        !std::isfinite(player.clip->duration) || player.clip->duration <= 0) {
        error = "animation clip does not match the skeleton/source revision";
        player.clip = {};
        return LoadResult::Failed;
    }
    player.context.Resize(skeleton.data->skeleton.num_joints());
    player.locals.resize(static_cast<std::size_t>(skeleton.data->skeleton.num_soa_joints()));
    return LoadResult::Ready;
}
} // namespace

struct AnimationSubsystem::Impl {
    Engine* engine = nullptr;
    std::size_t observer = 0;
    std::size_t changeObserver = 0;
    std::unordered_map<entt::entity, std::unique_ptr<Instance>> instances;
    entt::entity preview = entt::null;
    std::optional<SkinnedMeshRenderer> previewRenderer;

    Instance* Ensure(entt::entity entity) {
        if (!engine) return nullptr;
        auto& scene = engine->GetScene();
        const auto* component = scene.TryGetComponent<Animator>(entity);
        if (!component) return nullptr;
        auto& slot = instances[entity];
        if (slot && slot->authored == *component) return slot.get();
        if (preview == entity) preview = entt::null;
        slot = std::make_unique<Instance>();
        slot->authored = *component;
        slot->speed = component->speed;
        slot->playing = component->playOnStart;
        auto& assets = engine->GetSceneAssets();
        if (component->skeleton.id.IsValid()) slot->skeletonHandle = assets.RequestSkeleton(component->skeleton);
        if (component->playOnStart && component->clip.id.IsValid())
            slot->current = RequestPlayer(assets, component->clip, component->loop);
        if (!std::isfinite(slot->speed) || slot->speed < 0) {
            slot->speed = 0;
            slot->error = "animation speed must be finite and nonnegative";
        }
        return slot.get();
    }

    void Evaluate(Instance& state, double delta) {
        auto& assets = engine->GetSceneAssets();
        if (!state.skeleton) {
            state.skeleton = assets.Peek(state.skeletonHandle);
            if (!state.skeleton) {
                if (assets.GetState(state.skeletonHandle) != AssetState::Loading)
                    state.error = "skeleton is unassigned or failed to load; see the asset log";
                return;
            }
            if (!state.skeleton->data) { state.error = "empty skeleton data"; return; }
            state.blended.resize(static_cast<std::size_t>(state.skeleton->data->skeleton.num_soa_joints()));
            state.models.resize(state.skeleton->jointCount);
        }
        if (!state.skeleton->data) return;
        const auto resolve = [&](std::unique_ptr<Player>& player) {
            if (!player) return false;
            const auto result = ResolvePlayer(*player, *state.skeleton, assets, state.error);
            if (result == LoadResult::Failed) player.reset();
            return result == LoadResult::Ready;
        };
        bool currentReady = resolve(state.current);
        bool targetReady = resolve(state.target);
        if (targetReady && (!currentReady || state.fadeDuration == 0)) {
            state.current = std::move(state.target);
            currentReady = true;
            targetReady = false;
            state.fadeElapsed = 0;
        }
        const double advance = state.playing ? delta * state.speed : 0;
        if (currentReady) state.current->Advance(advance);
        if (targetReady) state.target->Advance(advance);
        const auto& skeleton = state.skeleton->data->skeleton;
        auto local = skeleton.joint_rest_poses();
        if (currentReady) {
            if (state.current->Sample()) local = ozz::make_span(state.current->locals);
            else { state.error = "ozz sampling failed"; currentReady = false; }
        }
        if (currentReady && targetReady) {
            if (state.playing) state.fadeElapsed += delta;
            const float weight = static_cast<float>(std::clamp(state.fadeElapsed / state.fadeDuration, 0.0, 1.0));
            if (state.target->Sample()) {
                ozz::animation::BlendingJob::Layer layers[2];
                layers[0].weight = 1.0f - weight;
                layers[0].transform = ozz::make_span(state.current->locals);
                layers[1].weight = weight;
                layers[1].transform = ozz::make_span(state.target->locals);
                ozz::animation::BlendingJob blend;
                blend.layers = ozz::make_span(layers);
                blend.rest_pose = skeleton.joint_rest_poses();
                blend.output = ozz::make_span(state.blended);
                if (blend.Run()) {
                    local = ozz::make_span(state.blended);
                    if (weight >= 1) state.current = std::move(state.target);
                } else state.error = "ozz blending failed";
            } else state.error = "ozz target sampling failed";
        }
        if (currentReady && !state.target && !state.current->loop && state.current->time >= state.current->clip->duration)
            state.playing = false;
        ozz::animation::LocalToModelJob model;
        model.skeleton = &skeleton;
        model.input = local;
        model.output = ozz::make_span(state.models);
        if (!model.Run()) { state.error = "ozz local-to-model conversion failed"; return; }
        auto pose = std::make_shared<AnimationPose>();
        pose->skeletonSignature = state.skeleton->signature;
        pose->jointModels.reserve(state.models.size());
        for (const auto& matrix : state.models) pose->jointModels.push_back(detail::ToEngineMatrix(matrix));
        state.pose = std::move(pose);
    }
};

AnimationSubsystem::AnimationSubsystem() : impl_(std::make_unique<Impl>()) {}
AnimationSubsystem::~AnimationSubsystem() = default;
std::string_view AnimationSubsystem::GetName() const { return "AnimationSubsystem"; }
bool AnimationSubsystem::Initialize() {
    impl_->engine = &GetEngine();
    impl_->observer = GetEngine().GetScene().AddLifecycleObserver([this](Scene::LifecycleEvent event, entt::entity entity, entt::id_type type) {
        if (event == Scene::LifecycleEvent::EntityDestroying || type == entt::type_hash<Animator>::value() ||
            (impl_->preview == entity && type == entt::type_hash<SkinnedMeshRenderer>::value())) {
            if (impl_->preview == entity) impl_->preview = entt::null;
            impl_->instances.erase(entity);
        }
    });
    impl_->changeObserver = GetEngine().GetScene().AddChangeObserver([this](Scene::ChangeKind, entt::entity entity) {
        if (entity == entt::null || impl_->preview != entity) return;
        const auto& scene = GetEngine().GetScene();
        const auto* animator = scene.TryGetComponent<Animator>(entity);
        const auto* renderer = scene.TryGetComponent<SkinnedMeshRenderer>(entity);
        const auto found = impl_->instances.find(entity);
        const auto& previous = impl_->previewRenderer;
        const bool meshChanged = bool(renderer) != previous.has_value() ||
            (renderer && previous && (renderer->mesh != previous->mesh || renderer->meshReference != previous->meshReference));
        if (!animator || found == impl_->instances.end() || found->second->authored != *animator || meshChanged) {
            // Scene change observers must not mutate the scene. Recreate the normal
            // authored pose at the next LateUpdate, before render extraction.
            impl_->preview = entt::null;
            impl_->previewRenderer.reset();
            impl_->instances.erase(entity);
        }
    });
    return true;
}
void AnimationSubsystem::Shutdown() {
    if (impl_->engine) {
        impl_->engine->GetScene().RemoveLifecycleObserver(impl_->observer);
        impl_->engine->GetScene().RemoveChangeObserver(impl_->changeObserver);
    }
    ResetSimulation();
    impl_->engine = nullptr;
}
void AnimationSubsystem::ResetSimulation() {
    impl_->preview = entt::null;
    impl_->previewRenderer.reset();
    impl_->instances.clear();
    if (impl_->engine) impl_->engine->GetScene().ForEach<SkinnedMeshRenderer>([&](entt::entity entity, const SkinnedMeshRenderer&) {
        impl_->engine->GetScene().TryGetComponent<SkinnedMeshRenderer>(entity)->posedBounds.reset();
    });
}
void AnimationSubsystem::LateUpdate(const FrameContext& frame) {
    if (!impl_->engine) return;
    const bool edit = GetEngine().IsEditorMode() && !GetEngine().IsPlaying();
    if (!edit && impl_->preview != entt::null) EndPreview();
    const double delta = !edit && !frame.paused && !GetEngine().IsPaused() && std::isfinite(frame.deltaSeconds)
        ? std::max(0.0, frame.deltaSeconds) : 0;
    const double previewDelta = std::isfinite(frame.realDeltaSeconds) ? std::max(0.0, frame.realDeltaSeconds) : 0;
    auto& scene = GetEngine().GetScene();
    scene.ForEach<Animator>([&](entt::entity entity, const Animator&) {
        if (auto* state = impl_->Ensure(entity))
            impl_->Evaluate(*state, edit && impl_->preview == entity ? previewDelta : delta);
    });
    std::erase_if(impl_->instances, [&](const auto& entry) { return !scene.HasComponent<Animator>(entry.first); });
    scene.ForEach<SkinnedMeshRenderer>([&](entt::entity entity, const SkinnedMeshRenderer& renderer) {
        const auto mesh = GetEngine().GetSceneAssets().Peek(renderer.mesh);
        scene.TryGetComponent<SkinnedMeshRenderer>(entity)->posedBounds =
            mesh ? std::optional{SkinnedMeshBounds(*mesh, GetPose(entity).get())} : std::nullopt;
    });
}

bool AnimationSubsystem::Play(entt::entity entity, const AssetReference& clip, float fadeSeconds, bool loop, std::string& error) {
    error.clear();
    if (!std::isfinite(fadeSeconds) || fadeSeconds < 0) { error = "fade duration must be finite and nonnegative"; return false; }
    auto* state = impl_->Ensure(entity);
    if (!state) { error = "entity has no Animator or animation subsystem is not initialized"; return false; }
    auto& assets = GetEngine().GetSceneAssets();
    if (!state->authored.skeleton.id.IsValid()) { error = "Animator has no skeleton assigned"; return false; }
    if (!state->skeleton && assets.GetState(state->skeletonHandle) == AssetState::Failed) {
        // An explicit command retries a repaired source without creating a retry
        // loop on every frame. Retired generations never become ready again.
        state->target.reset();
        state->skeletonHandle = assets.RequestSkeleton(state->authored.skeleton);
        if (!state->skeletonHandle) { error = "skeleton reference could not be resolved"; return false; }
    }
    if (state->target) { error = "an animation transition is already pending"; return false; }
    if (!GetEngine().GetSceneAssets().GetCatalog().Find(clip.id, CatalogAssetKind::AnimationClip)) {
        error = "missing animation clip UUID"; return false;
    }
    state->target = RequestPlayer(GetEngine().GetSceneAssets(), clip, loop);
    state->fadeDuration = fadeSeconds;
    state->fadeElapsed = 0;
    state->playing = true;
    state->error.clear();
    return true;
}
bool AnimationSubsystem::Preview(entt::entity entity, const AssetReference& clip, float fadeSeconds,
                                 bool loop, std::string& error) {
    error.clear();
    if (!impl_->engine || !GetEngine().IsEditorMode() || GetEngine().IsPlaying()) {
        error = "animation preview requires an editor in Edit mode";
        return false;
    }
    // Validate before replacing another entity's audition.
    if (!GetEngine().GetScene().HasComponent<Animator>(entity) ||
        !std::isfinite(fadeSeconds) || fadeSeconds < 0 ||
        !GetEngine().GetSceneAssets().GetCatalog().Find(clip.id, CatalogAssetKind::AnimationClip)) {
        error = "preview requires an Animator, a catalog clip and a finite nonnegative fade";
        return false;
    }
    if (impl_->preview != entity) {
        EndPreview();
        impl_->instances.erase(entity);
    }
    if (!Play(entity, clip, fadeSeconds, loop, error)) return false;
    impl_->preview = entity;
    if (const auto* renderer = GetEngine().GetScene().TryGetComponent<SkinnedMeshRenderer>(entity))
        impl_->previewRenderer = *renderer;
    else impl_->previewRenderer.reset();
    return true;
}
void AnimationSubsystem::EndPreview() {
    const auto entity = impl_->preview;
    impl_->preview = entt::null;
    impl_->previewRenderer.reset();
    if (entity == entt::null) return;
    impl_->instances.erase(entity);
    if (impl_->engine && GetEngine().IsEditorMode() && !GetEngine().IsPlaying()) {
        if (auto* state = impl_->Ensure(entity)) impl_->Evaluate(*state, 0);
        if (auto* renderer = GetEngine().GetScene().TryGetComponent<SkinnedMeshRenderer>(entity)) {
            const auto mesh = GetEngine().GetSceneAssets().Peek(renderer->mesh);
            renderer->posedBounds = mesh ? std::optional{SkinnedMeshBounds(*mesh, GetPose(entity).get())} : std::nullopt;
        }
    }
}
entt::entity AnimationSubsystem::GetPreviewEntity() const { return impl_->preview; }
bool AnimationSubsystem::SetPaused(entt::entity entity, bool paused) {
    auto* state = impl_->Ensure(entity);
    if (!state) return false;
    state->playing = !paused;
    return true;
}
bool AnimationSubsystem::Stop(entt::entity entity) {
    auto* state = impl_->Ensure(entity);
    if (!state) return false;
    state->current.reset();
    state->target.reset();
    state->playing = false;
    state->fadeElapsed = 0;
    state->error.clear();
    return true;
}
bool AnimationSubsystem::Seek(entt::entity entity, double seconds, std::string& error) {
    error.clear();
    auto* state = impl_->Ensure(entity);
    if (!std::isfinite(seconds) || seconds < 0) { error = "seek time must be finite and nonnegative"; return false; }
    if (!state || !state->current || !state->current->clip || state->target) {
        error = "seek requires a loaded clip and no pending transition"; return false;
    }
    auto& player = *state->current;
    player.time = player.loop ? std::fmod(seconds, static_cast<double>(player.clip->duration))
                             : std::min(seconds, static_cast<double>(player.clip->duration));
    player.context.Invalidate();
    return true;
}
bool AnimationSubsystem::SetSpeed(entt::entity entity, float speed, std::string& error) {
    error.clear();
    if (!std::isfinite(speed) || speed < 0) { error = "animation speed must be finite and nonnegative"; return false; }
    auto* state = impl_->Ensure(entity);
    if (!state) { error = "entity has no Animator"; return false; }
    state->speed = speed;
    return true;
}
std::shared_ptr<const AnimationPose> AnimationSubsystem::GetPose(entt::entity entity) const {
    const auto it = impl_->instances.find(entity);
    return it == impl_->instances.end() ? nullptr : it->second->pose;
}
std::optional<AnimationPlaybackState> AnimationSubsystem::GetPlaybackState(entt::entity entity) const {
    const auto it = impl_->instances.find(entity);
    if (it == impl_->instances.end()) return std::nullopt;
    const auto& state = *it->second;
    const auto* player = state.current.get();
    return AnimationPlaybackState{state.pose != nullptr, state.playing, state.target != nullptr,
        player ? player->time : 0, player && player->clip ? player->clip->duration : 0, state.error,
        player ? player->reference : AssetReference{}, state.target ? state.target->reference : AssetReference{},
        state.speed, player ? player->loop : state.authored.loop};
}

std::shared_ptr<const SkinningPalette> BuildSkinningPalette(const SkinBinding& binding,
    std::uint64_t skeletonSignature, const AnimationPose* pose) {
    const auto size = binding.joints.size();
    if (size == 0 || size > MaxSkinningJoints || binding.inverseBind.size() != size || binding.restPalette.size() != size)
        return {};
    auto result = std::make_shared<SkinningPalette>();
    result->matrices = binding.restPalette;
    if (pose && pose->skeletonSignature == skeletonSignature) {
        for (std::size_t i = 0; i < size; ++i) {
            if (binding.joints[i] >= pose->jointModels.size()) return {};
            result->matrices[i] = binding.inverseBind[i] * pose->jointModels[binding.joints[i]];
        }
    }
    return result;
}

Aabb SkinnedMeshBounds(const SkinnedMeshAsset& mesh, const AnimationPose* pose) {
    if (!pose || pose->skeletonSignature != mesh.skeletonSignature) return mesh.bounds;
    Aabb result;
    bool empty = true;
    for (const auto& section : mesh.sections) {
        if (!section.binding) continue;
        const auto& binding = *section.binding;
        if (binding.jointBounds.size() != binding.joints.size()) return mesh.bounds;
        for (std::size_t i = 0; i < binding.joints.size(); ++i) {
            if (binding.joints[i] >= pose->jointModels.size()) return mesh.bounds;
            const auto& box = binding.jointBounds[i];
            for (unsigned corner = 0; corner < 8; ++corner) {
                const Vector3 point{corner & 1 ? box.max.x : box.min.x,
                    corner & 2 ? box.max.y : box.min.y, corner & 4 ? box.max.z : box.min.z};
                Grow(result, empty, Vector3::Transform(point, pose->jointModels[binding.joints[i]]));
            }
        }
    }
    return empty ? mesh.bounds : result;
}

} // namespace engine
