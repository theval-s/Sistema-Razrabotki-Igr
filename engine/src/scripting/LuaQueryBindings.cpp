#include "LuaBindingGroups.hpp"

#include <engine/engine.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/scene/SceneQueries.hpp>
#include <engine/scripting/LuaBindingsContext.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <sol/sol.hpp>

namespace engine {
namespace {
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using Result = std::tuple<sol::object, std::string>;
using Options = sol::optional<sol::object>;

bool Present(const sol::object& value) {
    return value.valid() && value.get_type() != sol::type::nil;
}

Result Failure(sol::state_view lua, std::string error) {
    return {sol::make_object(lua, sol::nil), std::move(error)};
}

bool BooleanOption(const sol::table& table, const char* name, bool& output, std::string& error) {
    const auto value = table.raw_get<sol::object>(name);
    if (!Present(value)) return true;
    if (!value.is<bool>()) {
        error = std::string("options.") + name + " must be a boolean";
        return false;
    }
    output = value.as<bool>();
    return true;
}

bool EntityOption(const Scene& scene, const sol::object& value, entt::entity& entity, std::string& error) {
    if (!value.is<LuaEntityHandle>()) {
        error = "expected an Entity handle";
        return false;
    }
    const auto handle = value.as<LuaEntityHandle>();
    if (!handle.IsValid() || !handle.BelongsTo(scene)) {
        error = "entity is invalid or does not belong to the current scene";
        return false;
    }
    entity = handle.Resolve();
    return true;
}

bool ReadOptions(const Scene& scene, const Options& options, bool physics,
                 PhysicsRaycastOptions& output, std::string& error) {
    if (!options || !Present(*options)) return true;
    if (options->get_type() != sol::type::table) {
        error = "query options must be a table";
        return false;
    }
    const auto table = options->as<sol::table>();
    for (const auto& entry : table) {
        if (entry.first.get_type() != sol::type::string) {
            error = "query option names must be strings";
            return false;
        }
        const auto key = entry.first.as<std::string>();
        if (key != "ignore" && key != "ignoreDescendants" &&
            !(physics && (key == "includeTriggers" || key == "hitBackFaces"))) {
            error = "unknown query option: " + key;
            return false;
        }
    }
    if (!BooleanOption(table, "ignoreDescendants", output.filter.ignoreDescendants, error) ||
        (physics && (!BooleanOption(table, "includeTriggers", output.includeTriggers, error) ||
                     !BooleanOption(table, "hitBackFaces", output.hitBackFaces, error)))) return false;
    const auto ignore = table.raw_get<sol::object>("ignore");
    if (!Present(ignore)) return true;
    if (ignore.get_type() != sol::type::table) {
        error = "options.ignore must be a dense array of Entity handles";
        return false;
    }
    const auto list = ignore.as<sol::table>();
    const auto size = list.size();
    std::size_t count = 0;
    for (const auto& entry : list) {
        if (entry.first.get_type() != sol::type::number) {
            error = "options.ignore must be a dense array";
            return false;
        }
        const auto index = entry.first.as<double>();
        if (!std::isfinite(index) || std::floor(index) != index || index < 1 || index > double(size)) {
            error = "options.ignore must be a dense array";
            return false;
        }
        entt::entity entity;
        if (!EntityOption(scene, entry.second, entity, error)) return false;
        output.filter.ignore.push_back(entity);
        ++count;
    }
    if (count != size) {
        error = "options.ignore must be a dense array";
        return false;
    }
    return true;
}

bool ReadRay(const sol::object& origin, const sol::object& direction, const sol::object& distance,
             Ray& ray, std::string& error) {
    if (!origin.is<Vector3>() || !direction.is<Vector3>()) {
        error = "ray origin and direction must be Vector3 values";
        return false;
    }
    if (distance.get_type() != sol::type::number) {
        error = "maxDistance must be a number";
        return false;
    }
    const auto value = distance.as<double>();
    if (!std::isfinite(value) || value <= 0 || value > std::numeric_limits<float>::max()) {
        error = "maxDistance must be positive, finite and within the supported range";
        return false;
    }
    ray = {origin.as<Vector3>(), direction.as<Vector3>(), static_cast<float>(value)};
    return true;
}

sol::table HitTable(sol::state_view lua, Scene& scene, const RayHit& hit) {
    auto table = lua.create_table();
    table["entity"] = LuaEntityHandle::FromScene(scene, hit.entity);
    table["distance"] = hit.distance;
    table["point"] = hit.point;
    if (hit.normal) table["normal"] = *hit.normal;
    table["isTrigger"] = hit.isTrigger;
    return table;
}

Result HitResult(sol::state_view lua, Scene& scene, const std::optional<RayHit>& hit) {
    if (!hit) return {sol::make_object(lua, sol::nil), {}};
    return {sol::make_object(lua, HitTable(lua, scene, *hit)), {}};
}

Result Cast(LuaBindingsContext& context, sol::state_view lua, const sol::object& origin,
            const sol::object& direction, const sol::object& distance, const Options& options,
            bool physical, bool all) {
    auto* scene = context.GetScene();
    if (!context.engine || !scene) return Failure(lua, "no active scene");
    Ray ray;
    PhysicsRaycastOptions config;
    std::string error;
    if (!ReadRay(origin, direction, distance, ray, error) ||
        !ReadOptions(*scene, options, physical, config, error)) return Failure(lua, error);
    auto* physics = physical ? context.engine->GetSubsystem<PhysicsSubsystem>() : nullptr;
    if (physical && !physics) return Failure(lua, "physics subsystem is unavailable");
    if (all) {
        std::vector<RayHit> hits;
        const bool ok = physical ? physics->RaycastAll(ray, config, hits, error)
            : RaycastBoundsAll(*scene, context.engine->GetSceneAssets(), ray, config.filter, hits, error);
        if (!ok) return Failure(lua, error);
        auto result = lua.create_table();
        for (const auto& hit : hits) result.add(HitTable(lua, *scene, hit));
        return {sol::make_object(lua, result), {}};
    }
    std::optional<RayHit> hit;
    const bool ok = physical ? physics->Raycast(ray, config, hit, error)
        : RaycastBounds(*scene, context.engine->GetSceneAssets(), ray, config.filter, hit, error);
    return ok ? HitResult(lua, *scene, hit) : Failure(lua, error);
}

Result ScreenRay(LuaBindingsContext& context, sol::state_view lua,
                 const sol::object& point, const Options& options) {
    auto* scene = context.GetScene();
    if (!context.engine || !scene) return Failure(lua, "no active scene");
    if (!point.is<Vector2>()) return Failure(lua, "screen point must be a Vector2");
    const auto& surface = context.engine->GetSurface();
    Vector2 size{static_cast<float>(surface.width), static_cast<float>(surface.height)};
    entt::entity camera = entt::null;
    std::string error;
    if (options && Present(*options)) {
        if (options->get_type() != sol::type::table) return Failure(lua, "screen ray options must be a table");
        const auto table = options->as<sol::table>();
        for (const auto& entry : table) {
            if (entry.first.get_type() != sol::type::string)
                return Failure(lua, "screen ray option names must be strings");
            const auto key = entry.first.as<std::string>();
            if (key != "camera" && key != "viewportSize")
                return Failure(lua, "unknown screen ray option: " + key);
        }
        const auto value = table.raw_get<sol::object>("camera");
        if (Present(value) && !EntityOption(*scene, value, camera, error)) return Failure(lua, error);
        const auto viewport = table.raw_get<sol::object>("viewportSize");
        if (Present(viewport)) {
            if (!viewport.is<Vector2>()) return Failure(lua, "options.viewportSize must be a Vector2");
            size = viewport.as<Vector2>();
        }
    }
    Ray ray;
    if (!ScreenPointToRay(*scene, point.as<Vector2>(), size, camera, ray, error)) return Failure(lua, error);
    auto result = lua.create_table();
    result["origin"] = ray.origin;
    result["direction"] = ray.direction;
    result["maxDistance"] = ray.maxDistance;
    return {sol::make_object(lua, result), {}};
}
} // namespace

void RegisterQueryBindings(sol::state& lua, LuaBindingsContext& context) {
    sol::table physics = lua["Physics"];
    sol::table scene = lua["Scene"];
    auto bind = [&](sol::table& table, const char* name, bool physical, bool all) {
        table.set_function(name, [&context, physical, all](sol::object origin, sol::object direction,
            sol::object distance, Options options, sol::this_state state) {
            return Cast(context, sol::state_view(state), origin, direction, distance, options, physical, all);
        });
    };
    bind(physics, "Raycast", true, false);
    bind(physics, "RaycastAll", true, true);
    bind(scene, "RaycastBounds", false, false);
    bind(scene, "RaycastBoundsAll", false, true);
    physics.set_function("Linecast", [&context](sol::object start, sol::object end,
        Options options, sol::this_state state) -> Result {
        sol::state_view lua(state);
        auto* scene = context.GetScene();
        if (!context.engine || !scene) return Failure(lua, "no active scene");
        if (!start.is<Vector3>() || !end.is<Vector3>())
            return Failure(lua, "linecast endpoints must be Vector3 values");
        PhysicsRaycastOptions config;
        std::string error;
        if (!ReadOptions(*scene, options, true, config, error)) return Failure(lua, error);
        auto* physics = context.engine->GetSubsystem<PhysicsSubsystem>();
        if (!physics) return Failure(lua, "physics subsystem is unavailable");
        std::optional<RayHit> hit;
        if (!physics->Linecast(start.as<Vector3>(), end.as<Vector3>(), config, hit, error))
            return Failure(lua, error);
        return HitResult(lua, *scene, hit);
    });
    physics.set_function("SyncQueries", [&context] {
        auto* physics = context.engine ? context.engine->GetSubsystem<PhysicsSubsystem>() : nullptr;
        std::string error;
        const bool ok = physics && physics->SyncQueries(error);
        if (!physics) error = "physics subsystem is unavailable";
        return std::make_tuple(ok, error);
    });
    scene.set_function("ScreenPointToRay", [&context](sol::object point, Options options, sol::this_state state) {
        return ScreenRay(context, sol::state_view(state), point, options);
    });
}

} // namespace engine
