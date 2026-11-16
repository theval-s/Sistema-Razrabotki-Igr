#include <engine/scripting/LuaAnimationBindings.hpp>

#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>

#include <string>
#include <tuple>

namespace engine {
namespace {
using Result = std::tuple<bool, std::string>;

AnimationSubsystem* Resolve(LuaBindingsContext& context, const LuaEntityHandle& entity, std::string& error) {
    if (!context.engine) { error = "animation subsystem is unavailable"; return nullptr; }
    auto* scene = context.GetScene();
    if (!scene || !entity.IsValid() || !entity.BelongsTo(*scene)) {
        error = "entity is invalid or does not belong to the current scene";
        return nullptr;
    }
    auto* animation = context.engine->GetSubsystem<AnimationSubsystem>();
    if (!animation) { error = "animation subsystem is unavailable"; return nullptr; }
    if (!scene->HasComponent<Animator>(entity.Resolve())) {
        error = "entity has no Animator";
        return nullptr;
    }
    return animation;
}

bool ClipReference(LuaBindingsContext& context, const sol::object& value, AssetReference& result, std::string& error) {
    const auto& catalog = context.engine->GetSceneAssets().GetCatalog();
    const CatalogEntry* entry = nullptr;
    if (value.get_type() == sol::type::string) {
        const auto name = value.as<std::string>();
        entry = catalog.Find(std::string_view(name), CatalogAssetKind::AnimationClip);
        if (!entry) entry = catalog.FindPath(name);
    } else if (value.get_type() == sol::type::table) {
        const auto table = value.as<sol::table>();
        const sol::object id = table["id"];
        const sol::object path = table["path"];
        if (id.get_type() != sol::type::string ||
            (path.valid() && path.get_type() != sol::type::nil && path.get_type() != sol::type::string)) {
            error = "clip reference requires a string id and optional string path";
            return false;
        }
        // The id is authoritative; a stale path is never a fallback.
        entry = catalog.Find(std::string_view(id.as<std::string>()), CatalogAssetKind::AnimationClip);
    } else {
        error = "expected an animation clip UUID, path, or asset reference";
        return false;
    }
    if (!entry || entry->metadata.kind != CatalogAssetKind::AnimationClip) {
        error = "animation clip UUID/path is missing or has the wrong type";
        return false;
    }
    result = {entry->metadata.id, entry->path};
    return true;
}
}

void RegisterAnimationBindings(sol::state& lua, LuaBindingsContext& context) {
    sol::usertype<LuaEntityHandle> entity = lua["Entity"];
    entity["play_animation"] = [&context](const LuaEntityHandle& self, sol::object clip,
        sol::optional<float> fade, sol::optional<bool> loop) -> Result {
        std::string error;
        auto* animation = Resolve(context, self, error);
        if (!animation) return {false, error};
        AssetReference reference;
        if (!ClipReference(context, clip, reference, error)) return {false, error};
        const bool ok = animation->Play(self.Resolve(), reference, fade.value_or(0.f), loop.value_or(true), error);
        return {ok, error};
    };
    entity["pause_animation"] = [&context](const LuaEntityHandle& self, sol::optional<bool> paused) -> Result {
        std::string error;
        auto* animation = Resolve(context, self, error);
        if (!animation) return {false, error};
        const bool ok = animation->SetPaused(self.Resolve(), paused.value_or(true));
        return {ok, ok ? std::string{} : "entity has no Animator or animation subsystem is not initialized"};
    };
    entity["stop_animation"] = [&context](const LuaEntityHandle& self) -> Result {
        std::string error;
        auto* animation = Resolve(context, self, error);
        if (!animation) return {false, error};
        const bool ok = animation->Stop(self.Resolve());
        return {ok, ok ? std::string{} : "entity has no Animator or animation subsystem is not initialized"};
    };
    entity["seek_animation"] = [&context](const LuaEntityHandle& self, double seconds) -> Result {
        std::string error;
        auto* animation = Resolve(context, self, error);
        if (!animation) return {false, error};
        const bool ok = animation->Seek(self.Resolve(), seconds, error);
        return {ok, error};
    };
    entity["set_animation_speed"] = [&context](const LuaEntityHandle& self, float speed) -> Result {
        std::string error;
        auto* animation = Resolve(context, self, error);
        if (!animation) return {false, error};
        const bool ok = animation->SetSpeed(self.Resolve(), speed, error);
        return {ok, error};
    };
    entity["animation_state"] = [&context](const LuaEntityHandle& self, sol::this_state state) -> sol::object {
        std::string error;
        auto* animation = Resolve(context, self, error);
        if (!animation) return sol::make_object(state, sol::nil);
        const auto snapshot = animation->GetPlaybackState(self.Resolve());
        if (!snapshot) return sol::make_object(state, sol::nil);
        sol::state_view lua(state);
        auto table = lua.create_table();
        table["ready"] = snapshot->ready;
        table["playing"] = snapshot->playing;
        table["transitioning"] = snapshot->transitioning;
        table["time_seconds"] = snapshot->timeSeconds;
        table["duration"] = snapshot->duration;
        table["error"] = snapshot->error;
        return sol::make_object(state, table);
    };
}
} // namespace engine
