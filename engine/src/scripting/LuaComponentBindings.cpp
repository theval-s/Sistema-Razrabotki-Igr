#include <engine/scripting/LuaComponentBindings.hpp>
#include <engine/engine.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/rendering/RenderingResourceManager.hpp>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine {
namespace {
using Vector3 = DirectX::SimpleMath::Vector3;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Scene& Checked(const LuaEntityHandle& entity, LuaBindingsContext& context) {
    Require(entity.IsValid(), "Invalid Entity handle");
    Scene* scene = context.GetScene();
    Require(scene && entity.BelongsTo(*scene), "Entity belongs to another scene");
    return *scene;
}
template<class F> decltype(auto) Dispatch(const std::string& type, F&& f) {
    if (type == "Camera") return f(std::type_identity<Camera>{});
    if (type == "DirectionalLight") return f(std::type_identity<DirectionalLight>{});
    if (type == "PointLight") return f(std::type_identity<PointLight>{});
    if (type == "SpotLight") return f(std::type_identity<SpotLight>{});
    if (type == "MeshRenderer") return f(std::type_identity<MeshRenderer>{});
    if (type == "RigidBody") return f(std::type_identity<RigidBody>{});
    if (type == "Collider") return f(std::type_identity<Collider>{});
    if (type == "Script") return f(std::type_identity<ScriptComponent>{});
    if (type == "GamePersistence") return f(std::type_identity<GamePersistence>{});
    if (type == "Bounds") return f(std::type_identity<Bounds>{});
    throw std::runtime_error("Unsupported component type: " + type);
}
AssetReference Resolve(LuaBindingsContext& context, const std::string& name, CatalogAssetKind kind) {
    Require(context.engine != nullptr, "Asset catalog unavailable");
    const auto& catalog = context.engine->GetAssets().GetCatalog();
    const auto* entry = catalog.Find(std::string_view(name), kind);
    if (!entry) entry = catalog.FindPath(name);
    Require(entry && entry->metadata.kind == kind, "Asset UUID/path is missing or has the wrong type");
    return {entry->metadata.id, entry->path};
}
void Validate(float v) { Require(std::isfinite(v), "Component number must be finite and representable"); }
void Validate(bool) {}
void Validate(const std::string&) {}
void Validate(Vector3 v) { Validate(v.x); Validate(v.y); Validate(v.z); }
struct Fields {
    sol::table table;
    bool read;
    std::set<std::string> names;
    template<class T> void operator()(const char* name, T& value) {
        names.insert(name);
        if (!read) { Validate(value); table[name] = T(value); return; }
        sol::object object = table[name];
        if (!object.valid() || object.get_type() == sol::type::nil) { Validate(value); return; }
        if constexpr (std::is_same_v<T, float>) {
            Require(object.get_type() == sol::type::number, "Expected number");
            const double number = object.as<double>();
            Require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max(), "Number out of range");
            value = static_cast<float>(number);
        } else {
            if constexpr (std::is_same_v<T, bool>) Require(object.get_type() == sol::type::boolean, "Expected boolean");
            if constexpr (std::is_same_v<T, std::string>) Require(object.get_type() == sol::type::string, "Expected string");
            Require(object.is<T>(), "Incorrect component field type");
            value = object.as<T>();
        }
        Validate(value);
    }
    void Finish() {
        if (!read) return;
        for (const auto& entry : table)
            Require(entry.first.get_type() == sol::type::string && names.contains(entry.first.as<std::string>()), "Unknown component field");
    }
};
template<class T> void Codec(Fields& f, T& c, LuaBindingsContext& context) {
    if constexpr (std::is_same_v<T, Camera>) {
        f("fovY", c.fovY); f("nearPlane", c.nearPlane); f("farPlane", c.farPlane);
        Require(c.fovY > 0 && c.fovY < 3.14159265f && c.nearPlane > 0 && c.farPlane > c.nearPlane, "Invalid camera frustum");
    } else if constexpr (std::is_same_v<T, DirectionalLight> || std::is_same_v<T, PointLight> || std::is_same_v<T, SpotLight>) {
        f("color", c.color); f("intensity", c.intensity);
        Require(c.color.x >= 0 && c.color.y >= 0 && c.color.z >= 0 && c.intensity >= 0, "Negative light color/intensity");
        if constexpr (!std::is_same_v<T, DirectionalLight>) { f("range", c.range); Require(c.range > 0, "Light range must be positive"); }
        if constexpr (std::is_same_v<T, SpotLight>) {
            f("innerHalfAngle", c.innerHalfAngle); f("outerHalfAngle", c.outerHalfAngle);
            Require(c.innerHalfAngle >= 0 && c.outerHalfAngle > 0 && c.innerHalfAngle <= c.outerHalfAngle && c.outerHalfAngle < 1.57079633f, "Invalid spot angles");
        }
    } else if constexpr (std::is_same_v<T, RigidBody>) {
        Require(c.motionType == MotionType::Static || c.motionType == MotionType::Dynamic || c.motionType == MotionType::Kinematic, "Invalid motionType");
        std::string motion = c.motionType == MotionType::Dynamic ? "Dynamic" : c.motionType == MotionType::Kinematic ? "Kinematic" : "Static";
        f("motionType", motion); f("mass", c.mass); f("friction", c.friction); f("restitution", c.restitution);
        Require(motion == "Static" || motion == "Dynamic" || motion == "Kinematic", "Invalid motionType");
        c.motionType = motion == "Dynamic" ? MotionType::Dynamic : motion == "Kinematic" ? MotionType::Kinematic : MotionType::Static;
        Require(c.mass > 0 && c.friction >= 0 && c.restitution >= 0 && c.restitution <= 1, "Invalid rigid body values");
    } else if constexpr (std::is_same_v<T, Collider>) {
        const char* shapes[] = {"Cube", "Box", "Sphere", "Capsule", "ConvexMesh", "TriangleMesh"};
        Require(static_cast<int>(c.shape) >= 0 && static_cast<int>(c.shape) < 6, "Invalid collider shape");
        std::string shape = shapes[static_cast<int>(c.shape)];
        f("shape", shape); f("center", c.center); f("halfExtents", c.halfExtents); f("radius", c.radius); f("halfHeight", c.halfHeight); f("showDebug", c.showDebug); f("isTrigger", c.isTrigger);
        int index = 0; while (index < 6 && shape != shapes[index]) ++index;
        Require(index < 6, "Invalid collider shape"); c.shape = static_cast<ColliderShape>(index);
        Require(c.halfExtents.x > 0 && c.halfExtents.y > 0 && c.halfExtents.z > 0 && c.radius > 0 && c.halfHeight >= 0, "Invalid collider dimensions");
    } else if constexpr (std::is_same_v<T, Bounds>) {
        f("min", c.local.min); f("max", c.local.max);
        Require(c.local.min.x <= c.local.max.x && c.local.min.y <= c.local.max.y && c.local.min.z <= c.local.max.z, "Invalid bounds");
    } else if constexpr (std::is_same_v<T, GamePersistence>) {
        f("transform", c.transform); f("controllerState", c.controllerState); f("scriptState", c.scriptState);
    } else if constexpr (std::is_same_v<T, MeshRenderer>) {
        std::string mesh = c.meshReference.id.ToString(), material = c.materialReference.id.ToString();
        f("mesh", mesh); f("materialInstance", material);
        if (f.read) {
            c.meshReference = Resolve(context, mesh, CatalogAssetKind::Mesh);
            c.materialReference = Resolve(context, material, CatalogAssetKind::MaterialInstance);
        }
    } else if constexpr (std::is_same_v<T, ScriptComponent>) {
        std::string script = c.script.id.ToString();
        f("script", script); f("enabled", c.enabled);
        f.names.insert("properties");
        if (f.read) {
            c.script = Resolve(context, script, CatalogAssetKind::Script);
            sol::object props = f.table["properties"];
            if (props.valid() && props.get_type() != sol::type::nil) {
                Require(props.get_type() == sol::type::table, "Properties must be a table");
                ScriptPropertyMap values;
                for (const auto& entry : props.as<sol::table>()) {
                    Require(entry.first.get_type() == sol::type::string, "Property keys must be strings");
                    const auto key = entry.first.as<std::string>();
                    const auto& v = entry.second;
                    if (v.get_type() == sol::type::string) values.emplace(key, v.as<std::string>());
                    else if (v.get_type() == sol::type::boolean) values.emplace(key, v.as<bool>());
                    else if (v.get_type() == sol::type::number) {
                        const double number = v.as<double>(); Require(std::isfinite(number), "Property number must be finite"); values.emplace(key, number);
                    } else throw std::runtime_error("Properties support string, number and boolean values only");
                }
                std::string error;
                Require(ValidateScriptProperties(values, error), "Invalid scalar script properties");
                c.properties = std::move(values);
            }
        } else {
            sol::state_view lua(f.table.lua_state());
            sol::table props = lua.create_table();
            for (const auto& [key, v] : c.properties) {
                if (const auto* s = std::get_if<std::string>(&v.value)) props[key] = *s;
                else if (const auto* b = std::get_if<bool>(&v.value)) props[key] = *b;
                else props[key] = v.Number();
            }
            f.table["properties"] = props;
        }
    }
    f.Finish();
}
template<class T> void Commit(const LuaEntityHandle& entity, LuaBindingsContext& context, T value, bool add) {
    auto& scene = Checked(entity, context);
    const auto id = entity.Resolve();
    if (add ? scene.HasComponent<T>(id) : !scene.HasComponent<T>(id)) return;
    if constexpr (std::is_same_v<T, MeshRenderer>) {
        value.mesh = context.engine->GetSceneAssets().RequestMesh(value.meshReference);
        value.materialInstance = context.engine->GetSceneAssets().RequestMaterialInstance(value.materialReference);
        const auto* previous = scene.TryGetComponent<MeshRenderer>(id);
        if (previous && previous->materialReference.id != value.materialReference.id)
            context.engine->ClearEntityMaterialOverride(id);
    }
    if (add) scene.AddComponent<T>(id, std::move(value));
    else scene.Modify<T>(id, [&](T& target) { target = std::move(value); });
}
template<class T> void Write(const LuaEntityHandle& entity, LuaBindingsContext& context, T value, bool add) {
    if constexpr (std::is_same_v<T, ScriptComponent>) {
        Require(bool(context.enqueueSceneCommand), "Scene command queue unavailable");
        // General-purpose identity survives destruction of the calling script instance.
        const auto checked = LuaEntityHandle::FromScene(*entity.GetScene(), entity.Resolve());
        context.enqueueSceneCommand([checked, &context, value = std::move(value), add]() mutable {
            if (!checked.IsValid() || !context.GetScene() || !checked.BelongsTo(*context.GetScene())) return;
            Commit(checked, context, std::move(value), add);
        });
    } else Commit(entity, context, std::move(value), add);
}
void Set(const LuaEntityHandle& entity, const std::string& type, sol::optional<sol::table> data, LuaBindingsContext& context, bool add) {
    auto& scene = Checked(entity, context);
    Dispatch(type, [&](auto tag) {
        using T = typename decltype(tag)::type;
        const auto* old = scene.TryGetComponent<T>(entity.Resolve());
        Require(add ? old == nullptr : old != nullptr, add ? "Component already exists" : "Component is missing");
        T value = old ? *old : T{};
        // An omitted table is valid for default-valued components only.
        if (data) { Fields f{*data, true}; Codec(f, value, context); }
        else if constexpr (std::is_same_v<T, ScriptComponent> || std::is_same_v<T, MeshRenderer>)
            throw std::runtime_error("Asset component data is required");
        Write(entity, context, std::move(value), add);
    });
}
}

void RegisterComponentBindings(sol::state& lua, LuaBindingsContext& context) {
    sol::table entity = lua["Entity"];
    entity.set_function("HasComponent", [&context](const LuaEntityHandle& e, const std::string& type) {
        auto& scene = Checked(e, context);
        return Dispatch(type, [&](auto tag) { return scene.HasComponent<typename decltype(tag)::type>(e.Resolve()); });
    });
    entity.set_function("GetComponent", [&context](sol::this_state state, const LuaEntityHandle& e, const std::string& type) -> sol::object {
        auto& scene = Checked(e, context); sol::state_view lua(state);
        return Dispatch(type, [&](auto tag) -> sol::object {
            using T = typename decltype(tag)::type;
            const auto* component = scene.TryGetComponent<T>(e.Resolve());
            if (!component) return sol::make_object(lua, sol::nil);
            T copy = *component;
            Fields f{lua.create_table(), false}; Codec(f, copy, context);
            return sol::make_object(lua, f.table);
        });
    });
    entity.set_function("SetComponent", [&context](const LuaEntityHandle& e, const std::string& type, sol::table data) { Set(e, type, data, context, false); });
    entity.set_function("AddComponent", [&context](const LuaEntityHandle& e, const std::string& type, sol::optional<sol::table> data) { Set(e, type, data, context, true); });
    entity.set_function("RemoveComponent", [&context](const LuaEntityHandle& e, const std::string& type) {
        auto& scene = Checked(e, context);
        return Dispatch(type, [&](auto tag) {
            using T = typename decltype(tag)::type;
            if (!scene.HasComponent<T>(e.Resolve())) return false;
            Require(bool(context.enqueueSceneCommand), "Scene command queue unavailable");
            const auto handle = LuaEntityHandle::FromScene(scene, e.Resolve());
            context.enqueueSceneCommand([handle, &context] {
                if (handle.IsValid() && context.GetScene() && handle.BelongsTo(*context.GetScene()))
                    handle.GetScene()->RemoveComponent<T>(handle.Resolve());
            });
            return true;
        });
    });
    entity.set_function("SetActiveCamera", [&context](const LuaEntityHandle& e) {
        auto& scene = Checked(e, context);
        Require(scene.HasComponent<Camera>(e.Resolve()), "Camera component is missing");
        scene.SetActiveCamera(e.Resolve());
    });
    entity.set_function("IsActiveCamera", [&context](const LuaEntityHandle& e) { return Checked(e, context).GetActiveCamera() == e.Resolve(); });
    entity.set_function("SetMaterialParameters", [&context](const LuaEntityHandle& e, sol::table data) {
        Checked(e, context); Require(context.engine != nullptr, "Engine unavailable");
        // Validate the complete patch before EditEntityMaterial allocates an override.
        Vector3 color{}; float shininess = 0;
        Fields f{data, true}; f("specularColor", color); f("shininess", shininess); f.Finish();
        Require(color.x >= 0 && color.y >= 0 && color.z >= 0 && shininess >= 0, "Invalid material parameters");
        const sol::object colorField = data["specularColor"], shineField = data["shininess"];
        const bool hasColor = colorField.valid() && colorField.get_type() != sol::type::nil;
        const bool hasShininess = shineField.valid() && shineField.get_type() != sol::type::nil;
        std::string error;
        const bool edited = context.engine->EditEntityMaterial(e.Resolve(), [&](MaterialParameters& p) {
            if (hasColor) p.parameters.specularColor = color;
            if (hasShininess) p.parameters.shininess = shininess;
        }, error);
        Require(edited, error.c_str());
    });
    entity.set_function("GetMaterialParameters", [&context](sol::this_state state, const LuaEntityHandle& e) {
        auto& scene = Checked(e, context);
        Require(context.engine != nullptr, "Engine unavailable");
        const auto* renderer = scene.TryGetComponent<MeshRenderer>(e.Resolve());
        Require(renderer != nullptr, "MeshRenderer component is missing");
        auto* rendering = context.engine->GetSubsystem<RenderingSubsystem>();
        Require(rendering != nullptr, "Rendering subsystem unavailable");
        auto& resources = rendering->GetResourceManager();
        auto handle = context.engine->GetEntityMaterialOverride(e.Resolve());
        if (!handle) {
            auto asset = context.engine->GetSceneAssets().Peek(renderer->materialInstance);
            Require(bool(asset), "Material instance is not ready");
            handle = asset->instance;
        }
        const auto* instance = resources.TryGetMaterialInstance(handle);
        Require(instance != nullptr, "Material instance is unavailable");
        const auto* material = resources.TryGetMaterial(instance->material);
        Require(material != nullptr, "Material is unavailable");
        const auto parameters = ResolveMaterialParameters(*material, *instance);
        sol::state_view lua(state);
        sol::table result = lua.create_table();
        result["specularColor"] = Vector3(parameters.parameters.specularColor);
        result["shininess"] = parameters.parameters.shininess;
        return result;
    });
}

std::function<void(const LuaEntityHandle&)> PrepareLuaSpawnComponents(LuaBindingsContext& context, sol::optional<sol::table> options) {
    std::vector<std::function<void(const LuaEntityHandle&)>> actions;
    if (options) {
        sol::object components = (*options)["components"];
        if (components.valid() && components.get_type() != sol::type::nil) {
            Require(components.get_type() == sol::type::table, "components must be a table keyed by component type");
            for (const auto& entry : components.as<sol::table>()) {
                Require(entry.first.get_type() == sol::type::string && entry.second.get_type() == sol::type::table, "Invalid spawn component entry");
                Dispatch(entry.first.as<std::string>(), [&](auto tag) {
                    using T = typename decltype(tag)::type;
                    T value{}; Fields f{entry.second.as<sol::table>(), true}; Codec(f, value, context);
                    if constexpr (std::is_same_v<T, ScriptComponent>)
                        Require(bool(context.enqueueSceneCommand), "Scene command queue unavailable");
                    actions.emplace_back([&context, value](const LuaEntityHandle& e) { Write(e, context, value, true); });
                });
            }
        }
    }
    return [actions = std::move(actions)](const LuaEntityHandle& e) { for (const auto& action : actions) action(e); };
}
}
