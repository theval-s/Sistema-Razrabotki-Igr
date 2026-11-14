#include <engine/scene/SceneDocument.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/scene/Scene.hpp>
#include "../physics/PhysicsValidation.hpp"

namespace engine {
namespace {
using Json = nlohmann::json;
using Vector3 = DirectX::SimpleMath::Vector3;
using Quaternion = DirectX::SimpleMath::Quaternion;

void Fields(const Json& j, std::initializer_list<std::string_view> allowed) {
    if (!j.is_object()) throw std::invalid_argument("expected JSON object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool found = false;
        for (const auto field : allowed) found |= it.key() == field;
        if (!found) throw std::invalid_argument("unknown field: " + it.key());
    }
}

float Number(const Json& j) {
    if (!j.is_number()) throw std::invalid_argument("expected finite number");
    const auto value = j.get<float>();
    if (!std::isfinite(value)) throw std::invalid_argument("non-finite scene number");
    return value;
}

template <std::size_t N>
std::array<float, N> Numbers(const Json& j) {
    if (!j.is_array() || j.size() != N) throw std::invalid_argument("wrong numeric array size");
    std::array<float, N> values{};
    for (std::size_t i = 0; i < N; ++i) values[i] = Number(j[i]);
    return values;
}

Json Encode(const Vector3& value) { return Json::array({value.x, value.y, value.z}); }
Json Encode(const Quaternion& value) { return Json::array({value.x, value.y, value.z, value.w}); }
Json Encode(const std::optional<EntityUuid>& value) { return value ? Json(value->ToString()) : Json(nullptr); }

AssetReference Reference(const Json& j) {
    AssetReference result;
    std::string error;
    if (!ParseAssetReference(j.dump(), result, error)) throw std::invalid_argument(error);
    return result;
}

Json Encode(const AssetReference& reference) {
    std::string json, error;
    if (!SerializeAssetReference(reference, json, error)) throw std::invalid_argument(error);
    return Json::parse(json);
}

std::optional<EntityUuid> OptionalId(const Json& j) {
    if (j.is_null()) return std::nullopt;
    auto id = j.get<std::string>();
    auto parsed = EntityUuid::Parse(id);
    if (!parsed) throw std::invalid_argument("invalid entity UUID: " + id);
    return parsed;
}

SceneEntityDefinition DecodeEntity(const Json& j) {
    Fields(j, {"id", "name", "tags", "parent", "editorFolder", "components"});
    SceneEntityDefinition entity;
    const auto id = j.at("id").get<std::string>();
    const auto parsed = EntityUuid::Parse(id);
    if (!parsed) throw std::invalid_argument("invalid entity UUID: " + id);
    entity.id = *parsed;
    entity.name = j.at("name").get<std::string>();
    if (j.contains("tags")) {
        if (!j.at("tags").is_array()) throw std::invalid_argument("tags must be an array");
        std::set<std::string> seen;
        for (const auto& item : j.at("tags")) {
            if (!item.is_string() || item.get<std::string>().empty()) throw std::invalid_argument("tag must be a nonempty string");
            const auto tag = item.get<std::string>();
            if (!seen.insert(tag).second) throw std::invalid_argument("duplicate tag: " + tag);
            entity.tags.push_back(tag);
        }
    }
    if (j.contains("parent")) entity.parent = OptionalId(j.at("parent"));
    if (j.contains("editorFolder")) {
        if (!j.at("editorFolder").is_string() || !Scene::IsValidEditorFolder(j.at("editorFolder").get<std::string>()))
            throw std::invalid_argument("editorFolder must be folder names separated by '/'");
        entity.editorFolder = j.at("editorFolder").get<std::string>();
    }
    const auto& components = j.at("components");
    Fields(components, {"Transform", "MeshRenderer", "Camera", "DirectionalLight", "PointLight", "SpotLight", "Controller", "Bounds", "GamePersistence", "Script", "BehaviorTree", "RigidBody", "Collider", "NavigationRegion", "NavigationSource"});
    if (components.contains("RigidBody")) {
        const auto& body = components.at("RigidBody");
        Fields(body, {"motionType", "mass", "friction", "restitution"});
        RigidBody value;
        const auto type = body.at("motionType").get<std::string>();
        if (type == "Dynamic") value.motionType = MotionType::Dynamic;
        else if (type == "Kinematic") value.motionType = MotionType::Kinematic;
        else if (type != "Static") throw std::invalid_argument("invalid RigidBody motionType");
        value.mass = Number(body.at("mass"));
        value.friction = Number(body.at("friction"));
        value.restitution = Number(body.at("restitution"));
        entity.rigidBody = value;
    }
    if (components.contains("NavigationRegion")) {
        const auto& json = components.at("NavigationRegion");
        Fields(json, {"halfExtents", "cellSize", "cellHeight", "agentHeight", "agentRadius", "agentClimb", "agentSlope"});
        const auto extents = Numbers<3>(json.at("halfExtents"));
        if (extents[0] <= 0 || extents[1] <= 0 || extents[2] <= 0)
            throw std::invalid_argument("NavigationRegion half extents must be positive");
        entity.navigationRegion = NavigationRegion{{extents[0], extents[1], extents[2]},
            {Number(json.at("cellSize")), Number(json.at("cellHeight")), Number(json.at("agentHeight")),
             Number(json.at("agentRadius")), Number(json.at("agentClimb")), Number(json.at("agentSlope"))}};
    }
    if (components.contains("NavigationSource")) {
        const auto& json = components.at("NavigationSource");
        Fields(json, {"useCollider"});
        entity.navigationSource = NavigationSource{json.at("useCollider").get<bool>()};
    }
    if (components.contains("Collider")) {
        const auto& collider = components.at("Collider");
        Collider value;
        const auto shape = collider.at("shape").get<std::string>();
        if (shape == "Cube") value.shape = ColliderShape::Cube;
        else if (shape == "Box") value.shape = ColliderShape::Box;
        else if (shape == "Sphere") value.shape = ColliderShape::Sphere;
        else if (shape == "Capsule") value.shape = ColliderShape::Capsule;
        else if (shape == "ConvexMesh") value.shape = ColliderShape::ConvexMesh;
        else if (shape == "TriangleMesh") value.shape = ColliderShape::TriangleMesh;
        else throw std::invalid_argument("invalid Collider shape");
        const auto center = Numbers<3>(collider.at("center"));
        value.center = {center[0], center[1], center[2]};
        if (!collider.at("showDebug").is_boolean()) throw std::invalid_argument("Collider showDebug must be boolean");
        value.showDebug = collider.at("showDebug").get<bool>();
        if (collider.contains("isTrigger")) {
            if (!collider.at("isTrigger").is_boolean()) throw std::invalid_argument("Collider isTrigger must be boolean");
            value.isTrigger = collider.at("isTrigger").get<bool>();
        }
        switch (value.shape) {
        case ColliderShape::Cube:
        case ColliderShape::Box: {
            Fields(collider, {"shape", "center", "showDebug", "isTrigger", "halfExtents"});
            const auto extents = Numbers<3>(collider.at("halfExtents"));
            value.halfExtents = {extents[0], extents[1], extents[2]};
            break;
        }
        case ColliderShape::Sphere:
            Fields(collider, {"shape", "center", "showDebug", "isTrigger", "radius"});
            value.radius = Number(collider.at("radius"));
            break;
        case ColliderShape::Capsule:
            Fields(collider, {"shape", "center", "showDebug", "isTrigger", "radius", "halfHeight"});
            value.radius = Number(collider.at("radius"));
            value.halfHeight = Number(collider.at("halfHeight"));
            break;
        case ColliderShape::ConvexMesh:
        case ColliderShape::TriangleMesh:
            Fields(collider, {"shape", "center", "showDebug", "isTrigger"});
            break;
        }
        entity.collider = value;
    }
    if (components.contains("Script")) {
        const auto& script = components.at("Script");
        Fields(script, {"script", "enabled", "properties"});
        ScriptComponent value;
        if (!script.at("script").is_null()) value.script = Reference(script.at("script"));
        value.enabled = script.at("enabled").get<bool>();
        if (script.contains("properties")) {
            StateValue propertyData;
            std::string error;
            if (!script.at("properties").is_object() || !ParseStateValue(script.at("properties").dump(), propertyData, error))
                throw std::invalid_argument("Script properties must be an object: " + error);
            value.properties = propertyData.Fields();
            if (!ValidateScriptProperties(value.properties, error)) throw std::invalid_argument(error);
        }
        entity.script = std::move(value);
    }
    if (components.contains("BehaviorTree")) {
        const auto& data = components.at("BehaviorTree");
        Fields(data, {"tree", "enabled", "parameters"});
        ai::BehaviorTreeComponent value;
        if (!data.at("tree").is_null()) value.tree = Reference(data.at("tree"));
        value.enabled = data.value("enabled", true);
        if (data.contains("parameters")) {
            StateValue parameters;
            std::string error;
            if (!data.at("parameters").is_object() ||
                !ParseStateValue(data.at("parameters").dump(), parameters, error))
                throw std::invalid_argument("BehaviorTree parameters must be an object: " + error);
            value.parameters = parameters.Fields();
            if (!ValidateScriptProperties(value.parameters, error)) throw std::invalid_argument(error);
        }
        entity.behaviorTree = std::move(value);
    }
    if (components.contains("GamePersistence")) {
        const auto& persist = components.at("GamePersistence");
        Fields(persist, {"transform", "controllerState", "scriptState"});
        entity.persistence = GamePersistence{persist.at("transform").get<bool>(), persist.at("controllerState").get<bool>()};
        if (persist.contains("scriptState")) entity.persistence->scriptState = persist.at("scriptState").get<bool>();
    }
    if (components.contains("Bounds")) {
        const auto& bounds = components.at("Bounds");
        Fields(bounds, {"min", "max"});
        const auto min = Numbers<3>(bounds.at("min"));
        const auto max = Numbers<3>(bounds.at("max"));
        for (int i = 0; i < 3; ++i) if (min[i] > max[i]) throw std::invalid_argument("invalid Bounds: min > max");
        entity.bounds = Bounds{{{min[0], min[1], min[2]}, {max[0], max[1], max[2]}}};
    }
    if (components.contains("Controller")) {
        const auto& controller = components.at("Controller");
        Fields(controller, {"type", "version", "enabled", "data"});
        ControllerDefinition value;
        value.type = controller.at("type").get<std::string>();
        if (value.type.empty() || !controller.at("version").is_number_integer() ||
            !controller.at("enabled").is_boolean() || !controller.contains("data"))
            throw std::invalid_argument("invalid controller envelope");
        value.version = controller.at("version").get<int>();
        if (value.version < 1) throw std::invalid_argument("invalid controller version");
        value.enabled = controller.at("enabled").get<bool>();
        std::string error;
        if (!ParseStateValue(controller.at("data").dump(), value.data, error)) throw std::invalid_argument(error);
        entity.controller = std::move(value);
    }
    const auto& transform = components.at("Transform");
    Fields(transform, {"position", "rotation", "scale"});
    const auto position = Numbers<3>(transform.at("position"));
    const auto rotation = Numbers<4>(transform.at("rotation"));
    const auto scale = Numbers<3>(transform.at("scale"));
    entity.transform.position = {position[0], position[1], position[2]};
    entity.transform.rotation = {rotation[0], rotation[1], rotation[2], rotation[3]};
    entity.transform.scale = {scale[0], scale[1], scale[2]};
    if (entity.transform.rotation.LengthSquared() < 1e-8f)
        throw std::invalid_argument("zero-length rotation quaternion");

    if (components.contains("MeshRenderer")) {
        const auto& renderer = components.at("MeshRenderer");
        Fields(renderer, {"mesh", "materialInstance"});
        entity.renderer = SceneEntityDefinition::Mesh{Reference(renderer.at("mesh")),
                                                       Reference(renderer.at("materialInstance"))};
    }
    if (components.contains("Camera")) {
        const auto& camera = components.at("Camera");
        Fields(camera, {"fovY", "nearPlane", "farPlane"});
        Camera value{Number(camera.at("fovY")), Number(camera.at("nearPlane")), Number(camera.at("farPlane"))};
        if (!(value.fovY > 0 && value.fovY < 3.14159265f && value.nearPlane > 0 && value.farPlane > value.nearPlane))
            throw std::invalid_argument("invalid camera projection");
        entity.camera = value;
    }
    if (components.contains("DirectionalLight")) {
        const auto& light = components.at("DirectionalLight");
        Fields(light, {"color", "intensity"});
        const auto color = Numbers<3>(light.at("color"));
        const float intensity = Number(light.at("intensity"));
        if (intensity < 0 || color[0] < 0 || color[1] < 0 || color[2] < 0)
            throw std::invalid_argument("negative light color or intensity");
        entity.directionalLight = DirectionalLight{{color[0], color[1], color[2]}, intensity};
    }
    if (components.contains("PointLight")) {
        const auto& light = components.at("PointLight");
        Fields(light, {"color", "intensity", "range"});
        const auto color = Numbers<3>(light.at("color"));
        const float intensity = Number(light.at("intensity"));
        const float range = Number(light.at("range"));
        if (intensity < 0 || range <= 0 || color[0] < 0 || color[1] < 0 || color[2] < 0)
            throw std::invalid_argument("invalid point light color, intensity or range");
        entity.pointLight = PointLight{{color[0], color[1], color[2]}, intensity, range};
    }
    if (components.contains("SpotLight")) {
        const auto& light = components.at("SpotLight");
        Fields(light, {"color", "intensity", "range", "innerHalfAngle", "outerHalfAngle"});
        const auto color = Numbers<3>(light.at("color"));
        const float intensity = Number(light.at("intensity"));
        const float range = Number(light.at("range"));
        const float inner = Number(light.at("innerHalfAngle"));
        const float outer = Number(light.at("outerHalfAngle"));
        if (intensity < 0 || range <= 0 || color[0] < 0 || color[1] < 0 || color[2] < 0 ||
            inner < 0 || inner >= outer || outer >= 1.57079632679f)
            throw std::invalid_argument("invalid spot light color, intensity, range or half angles");
        entity.spotLight = SpotLight{{color[0], color[1], color[2]}, intensity, range, inner, outer};
    }
    return entity;
}

void Validate(const SceneDocument& document) {
    std::unordered_map<EntityUuid, std::size_t, EntityUuidHash> ids;
    for (std::size_t i = 0; i < document.entities.size(); ++i)
        if (!ids.emplace(document.entities[i].id, i).second)
            throw std::invalid_argument("duplicate entity UUID: " + document.entities[i].id.ToString());
    for (const auto& entity : document.entities) {
        std::unordered_set<EntityUuid, EntityUuidHash> ancestors;
        const SceneEntityDefinition* current = &entity;
        while (current->parent) {
            if (!ancestors.insert(current->id).second) throw std::invalid_argument("hierarchy cycle at " + entity.id.ToString());
            const auto it = ids.find(*current->parent);
            if (it == ids.end()) throw std::invalid_argument("unknown parent UUID: " + current->parent->ToString());
            current = &document.entities[it->second];
        }
    }
    for (const auto& entity : document.entities)
        if (entity.parent && !entity.editorFolder.empty())
            throw std::invalid_argument("editorFolder is only allowed on root entities: " + entity.id.ToString());
    std::set<std::string> folders;
    for (const auto& folder : document.editorFolders) {
        if (!Scene::IsValidEditorFolder(folder)) throw std::invalid_argument("invalid editor folder: " + folder);
        if (!folders.insert(folder).second) throw std::invalid_argument("duplicate editor folder: " + folder);
    }
    for (const auto& entity : document.entities) {
        if (entity.rigidBody.has_value() != entity.collider.has_value())
            throw std::invalid_argument("RigidBody and Collider must be provided together");
        if (!entity.rigidBody) continue;
        std::string error;
        if (!detail::ValidatePhysics(*entity.rigidBody, *entity.collider, error)) throw std::invalid_argument(error);
        if ((entity.collider->shape == ColliderShape::ConvexMesh || entity.collider->shape == ColliderShape::TriangleMesh) && !entity.renderer)
            throw std::invalid_argument("mesh collider requires MeshRenderer");
        if (entity.parent && entity.rigidBody->motionType == MotionType::Dynamic)
            throw std::invalid_argument("dynamic physics bodies must be root entities");
        if (entity.transform.scale.x <= 0 || entity.transform.scale.y <= 0 || entity.transform.scale.z <= 0)
            throw std::invalid_argument("physics entities require positive local scale");
        auto matrix = entity.transform.ToMatrix();
        const auto* current = &entity;
        while (current->parent) {
            current = &document.entities[ids.at(*current->parent)];
            matrix *= current->transform.ToMatrix();
        }
        detail::PhysicsPose pose;
        if (!detail::GetPhysicsPose(matrix, pose, error)) throw std::invalid_argument(error);
        if (!detail::Finite(entity.collider->halfExtents * pose.scale) || !detail::Finite(entity.collider->center * pose.scale) ||
            !detail::Finite(pose.scale * entity.collider->radius) || !detail::Finite(pose.scale * entity.collider->halfHeight))
            throw std::invalid_argument("scaled collider is not finite");
    }
    if (document.activeCamera) {
        const auto it = ids.find(*document.activeCamera);
        if (it == ids.end() || !document.entities[it->second].camera)
            throw std::invalid_argument("activeCamera must identify a Camera entity");
    }
    if (document.directionalLight) {
        const auto it = ids.find(*document.directionalLight);
        if (it == ids.end() || !document.entities[it->second].directionalLight)
            throw std::invalid_argument("directionalLight must identify a DirectionalLight entity");
    }
}

SceneDocument Decode(const Json& j) {
    Fields(j, {"version", "entities", "activeCamera", "directionalLight", "editorFolders"});
    if (j.at("version") != 1) throw std::invalid_argument("unsupported scene version");
    SceneDocument result;
    if (!j.at("entities").is_array()) throw std::invalid_argument("entities must be an array");
    for (const auto& item : j.at("entities")) result.entities.push_back(DecodeEntity(item));
    if (j.contains("activeCamera")) result.activeCamera = OptionalId(j.at("activeCamera"));
    if (j.contains("directionalLight")) result.directionalLight = OptionalId(j.at("directionalLight"));
    if (j.contains("editorFolders")) {
        if (!j.at("editorFolders").is_array()) throw std::invalid_argument("editorFolders must be an array");
        for (const auto& folder : j.at("editorFolders")) {
            if (!folder.is_string()) throw std::invalid_argument("editor folder must be a string");
            result.editorFolders.push_back(folder.get<std::string>());
        }
    }
    Validate(result);
    return result;
}

Json Encode(const SceneDocument& document) {
    Json root = {{"version", 1}, {"entities", Json::array()}, {"activeCamera", Encode(document.activeCamera)},
                 {"directionalLight", Encode(document.directionalLight)}};
    for (const auto& entity : document.entities) {
        Json components = {{"Transform", {{"position", Encode(entity.transform.position)},
                                            {"rotation", Encode(entity.transform.rotation)},
                                            {"scale", Encode(entity.transform.scale)}}}};
        if (entity.renderer)
            components["MeshRenderer"] = {{"mesh", Encode(entity.renderer->mesh)},
                                          {"materialInstance", Encode(entity.renderer->materialInstance)}};
        if (entity.camera)
            components["Camera"] = {{"fovY", entity.camera->fovY}, {"nearPlane", entity.camera->nearPlane},
                                    {"farPlane", entity.camera->farPlane}};
        if (entity.directionalLight)
            components["DirectionalLight"] = {{"color", Encode(entity.directionalLight->color)},
                                                {"intensity", entity.directionalLight->intensity}};
        if (entity.pointLight)
            components["PointLight"] = {{"color", Encode(entity.pointLight->color)},
                                         {"intensity", entity.pointLight->intensity}, {"range", entity.pointLight->range}};
        if (entity.spotLight)
            components["SpotLight"] = {{"color", Encode(entity.spotLight->color)},
                                        {"intensity", entity.spotLight->intensity}, {"range", entity.spotLight->range},
                                        {"innerHalfAngle", entity.spotLight->innerHalfAngle},
                                        {"outerHalfAngle", entity.spotLight->outerHalfAngle}};
        if (entity.controller)
            components["Controller"] = {{"type", entity.controller->type}, {"version", entity.controller->version},
                                         {"enabled", entity.controller->enabled}, {"data", [&] {
                                             std::string json, error;
                                             if (!SerializeStateValue(entity.controller->data, json, error))
                                                 throw std::invalid_argument(error);
                                             return Json::parse(json);
                                         }()}};
        if (entity.rigidBody) {
            const auto& body = *entity.rigidBody;
            const char* motion = body.motionType == MotionType::Dynamic ? "Dynamic" :
                                 body.motionType == MotionType::Kinematic ? "Kinematic" : "Static";
            if (body.motionType != MotionType::Static && body.motionType != MotionType::Dynamic && body.motionType != MotionType::Kinematic)
                throw std::invalid_argument("invalid RigidBody motionType");
            components["RigidBody"] = {{"motionType", motion}, {"mass", body.mass}, {"friction", body.friction}, {"restitution", body.restitution}};
        }
        if (entity.navigationRegion) {
            const auto& region = *entity.navigationRegion;
            const auto& s = region.settings;
            components["NavigationRegion"] = {{"halfExtents", Encode(region.halfExtents)},
                {"cellSize", s.cellSize}, {"cellHeight", s.cellHeight}, {"agentHeight", s.agentHeight},
                {"agentRadius", s.agentRadius}, {"agentClimb", s.agentClimb}, {"agentSlope", s.agentSlope}};
        }
        if (entity.navigationSource)
            components["NavigationSource"] = {{"useCollider", entity.navigationSource->useCollider}};
        if (entity.collider) {
            const auto& c = *entity.collider;
            const char* shape = "Box";
            switch (c.shape) {
            case ColliderShape::Cube: shape = "Cube"; break;
            case ColliderShape::Box: break;
            case ColliderShape::Sphere: shape = "Sphere"; break;
            case ColliderShape::Capsule: shape = "Capsule"; break;
            case ColliderShape::ConvexMesh: shape = "ConvexMesh"; break;
            case ColliderShape::TriangleMesh: shape = "TriangleMesh"; break;
            }
            auto& json = components["Collider"];
            json = {{"shape", shape}, {"center", Encode(c.center)}, {"showDebug", c.showDebug}, {"isTrigger", c.isTrigger}};
            if (c.shape == ColliderShape::Cube || c.shape == ColliderShape::Box) json["halfExtents"] = Encode(c.halfExtents);
            if (c.shape == ColliderShape::Sphere || c.shape == ColliderShape::Capsule) json["radius"] = c.radius;
            if (c.shape == ColliderShape::Capsule) json["halfHeight"] = c.halfHeight;
        }
        if (entity.bounds)
            components["Bounds"] = {{"min", Encode(entity.bounds->local.min)}, {"max", Encode(entity.bounds->local.max)}};
        if (entity.persistence)
            components["GamePersistence"] = {{"transform", entity.persistence->transform},
                                              {"controllerState", entity.persistence->controllerState},
                                              {"scriptState", entity.persistence->scriptState}};
        if (entity.script) {
            const auto& reference = entity.script->script;
            if (!reference.id.IsValid() && !reference.path.empty())
                throw std::invalid_argument("invalid Script reference");
            components["Script"] = {{"script", reference.id.IsValid() ? Encode(reference) : Json(nullptr)},
                                    {"enabled", entity.script->enabled}};
            std::string properties, error;
            if (!ValidateScriptProperties(entity.script->properties, error) ||
                !SerializeStateValue(StateValue(entity.script->properties), properties, error)) throw std::invalid_argument(error);
            components["Script"]["properties"] = Json::parse(properties);
        }
        if (entity.behaviorTree) {
            const auto& behavior = *entity.behaviorTree;
            if (!behavior.tree.id.IsValid() && !behavior.tree.path.empty())
                throw std::invalid_argument("invalid BehaviorTree reference");
            std::string parameters, error;
            if (!ValidateScriptProperties(behavior.parameters, error) ||
                !SerializeStateValue(StateValue(behavior.parameters), parameters, error))
                throw std::invalid_argument(error);
            components["BehaviorTree"] = {{"tree", behavior.tree.id.IsValid() ? Encode(behavior.tree) : Json(nullptr)},
                                           {"enabled", behavior.enabled},
                                           {"parameters", Json::parse(parameters)}};
        }
        Json encoded = {{"id", entity.id.ToString()}, {"name", entity.name}, {"tags", entity.tags},
                        {"parent", Encode(entity.parent)}, {"components", std::move(components)}};
        // Only when used, so scenes without folders keep their exact format.
        if (!entity.editorFolder.empty()) encoded["editorFolder"] = entity.editorFolder;
        root["entities"].push_back(std::move(encoded));
    }
    if (!document.editorFolders.empty()) root["editorFolders"] = document.editorFolders;
    return root;
}

// The renderer's authored references, or its loaded assets' if it has none; nothing if they aren't file-backed.
std::optional<SceneEntityDefinition::Mesh> RendererReferences(const MeshRenderer& renderer, const AssetSystem& assets) {
    auto authored = [&](const AssetReference& ref, CatalogAssetKind kind) -> std::optional<AssetReference> {
        if (!ref.id.IsValid()) return std::nullopt;
        if (const auto* entry = assets.GetCatalog().Find(ref.id, kind))
            return AssetReference{ref.id, entry->path};
        return ref; // missing asset: keep the hand-editable recovery hint
    };
    auto mesh = authored(renderer.meshReference, CatalogAssetKind::Mesh);
    if (!mesh) mesh = assets.ReferenceFor(renderer.mesh);
    auto material = authored(renderer.materialReference, CatalogAssetKind::MaterialInstance);
    if (!material) material = assets.ReferenceFor(renderer.materialInstance);
    if (!mesh || !material) return std::nullopt;
    return SceneEntityDefinition::Mesh{*mesh, *material};
}

MeshRenderer RequestRenderer(const SceneEntityDefinition::Mesh& references, AssetSystem& assets) {
    return {assets.RequestMesh(references.mesh), assets.RequestMaterialInstance(references.materialInstance),
            references.mesh, references.materialInstance};
}

SceneEntityDefinition CaptureDefinition(const Scene& scene, const AssetSystem& assets, const entt::entity entity) {
    SceneEntityDefinition definition;
    definition.id = scene.GetId(entity);
    definition.name = scene.GetName(entity);
    const auto tags = scene.GetTags(entity);
    definition.tags.assign(tags.begin(), tags.end());
    if (const auto* controller = scene.GetControllerDefinition(entity)) definition.controller = *controller;
    if (const auto* body = scene.TryGetComponent<RigidBody>(entity)) definition.rigidBody = *body;
    if (const auto* region = scene.TryGetComponent<NavigationRegion>(entity)) definition.navigationRegion = *region;
    if (const auto* source = scene.TryGetComponent<NavigationSource>(entity)) definition.navigationSource = *source;
    if (const auto* box = scene.TryGetComponent<Collider>(entity)) definition.collider = *box;
    if (const auto* bounds = scene.TryGetComponent<Bounds>(entity)) definition.bounds = *bounds;
    if (const auto* persistence = scene.TryGetComponent<GamePersistence>(entity)) definition.persistence = *persistence;
    if (const auto* script = scene.TryGetComponent<ScriptComponent>(entity)) definition.script = *script;
    if (const auto* tree = scene.TryGetComponent<ai::BehaviorTreeComponent>(entity)) definition.behaviorTree = *tree;
    if (auto parent = scene.GetParent(entity); parent != entt::null)
        definition.parent = scene.GetId(parent);
    definition.editorFolder = scene.GetEditorFolder(entity);
    definition.transform = scene.GetTransform(entity);
    if (const auto* renderer = scene.TryGetComponent<MeshRenderer>(entity)) {
        definition.renderer = RendererReferences(*renderer, assets);
        if (!definition.renderer)
            throw std::invalid_argument("renderer has no file-backed mesh or material instance on " + definition.id.ToString());
    }
    if (const auto* component = scene.TryGetComponent<Camera>(entity)) definition.camera = *component;
    if (const auto* component = scene.TryGetComponent<DirectionalLight>(entity)) definition.directionalLight = *component;
    if (const auto* component = scene.TryGetComponent<PointLight>(entity)) definition.pointLight = *component;
    if (const auto* component = scene.TryGetComponent<SpotLight>(entity)) definition.spotLight = *component;
    return definition;
}

template <class T>
void MatchComponent(Scene& scene, const entt::entity entity, const std::optional<T>& wanted) {
    const T* current = std::as_const(scene).TryGetComponent<T>(entity);
    if (!wanted) {
        if (current) scene.RemoveComponent<T>(entity);
    } else if (!current) {
        scene.AddComponent<T>(entity, *wanted);
    } else if (!(*current == *wanted)) {
        scene.Modify<T>(entity, [&wanted](T& component) { component = *wanted; });
    }
}

// Everything but the parent, onto `entity`, which already has its parent. Equal parts are left alone.
void ApplyEntity(const SceneEntityDefinition& definition, AssetSystem& assets, Scene& destination, const entt::entity entity) {
    destination.SetName(entity, definition.name);
    if (const auto tags = destination.GetTags(entity); !std::ranges::equal(tags, definition.tags)) {
        const std::vector<std::string> had(tags.begin(), tags.end());
        for (const auto& tag : had) destination.RemoveTag(entity, tag);
        for (const auto& tag : definition.tags) destination.AddTag(entity, tag);
    }
    // Refused under a parent, e.g. a copied root pasted as a child.
    destination.SetEditorFolder(entity, definition.editorFolder);
    if (!(destination.GetTransform(entity) == definition.transform)) destination.SetTransform(entity, definition.transform);
    const auto* renderer = std::as_const(destination).TryGetComponent<MeshRenderer>(entity);
    if (!definition.renderer) {
        if (renderer) destination.RemoveComponent<MeshRenderer>(entity);
    } else if (!renderer) {
        destination.AddComponent<MeshRenderer>(entity, RequestRenderer(*definition.renderer, assets));
    } else if (RendererReferences(*renderer, assets) != definition.renderer) {
        destination.Modify<MeshRenderer>(entity, [&](MeshRenderer& changed) { changed = RequestRenderer(*definition.renderer, assets); });
    }
    MatchComponent(destination, entity, definition.camera);
    MatchComponent(destination, entity, definition.directionalLight);
    MatchComponent(destination, entity, definition.pointLight);
    MatchComponent(destination, entity, definition.spotLight);
    MatchComponent(destination, entity, definition.rigidBody);
    MatchComponent(destination, entity, definition.collider);
    MatchComponent(destination, entity, definition.navigationRegion);
    MatchComponent(destination, entity, definition.navigationSource);
    MatchComponent(destination, entity, definition.bounds);
    MatchComponent(destination, entity, definition.persistence);
    MatchComponent(destination, entity, definition.script);
    MatchComponent(destination, entity, definition.behaviorTree);
    const auto* controller = destination.GetControllerDefinition(entity);
    if (!definition.controller) {
        if (controller) destination.RemoveController(entity);
    } else if (!controller || !(*controller == *definition.controller)) {
        destination.AttachController(entity, *definition.controller);
    }
}

Json Read(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 32) throw std::invalid_argument("scene JSON nesting is too deep");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate JSON field: " + value.get<std::string>());
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text.begin(), text.end(), callback);
}
} // namespace

bool ParseSceneDocument(std::string_view json, SceneDocument& result, std::string& error) {
    try { auto decoded = Decode(Read(json)); result = std::move(decoded); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}

bool SerializeSceneDocument(const SceneDocument& document, std::string& json, std::string& error) {
    try {
        const auto text = Encode(document).dump(2);
        [[maybe_unused]] const auto checked = Decode(Read(text));
        json = text;
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool CaptureSceneDocument(const Scene& scene, const AssetSystem& assets, SceneDocument& result, std::string& error,
                          bool ignoreTransientComponents) {
    try {
        if (!ignoreTransientComponents && scene.HasUnsupportedComponents())
            throw std::invalid_argument("scene contains components without a version 1 codec");
        SceneDocument captured;
        auto settings = CaptureSceneSettings(scene);
        captured.activeCamera = settings.activeCamera;
        captured.directionalLight = settings.directionalLight;
        captured.editorFolders = std::move(settings.editorFolders);
        auto visit = [&](auto&& self, const entt::entity entity) -> void {
            captured.entities.push_back(CaptureDefinition(scene, assets, entity));
            for (const auto child : scene.GetChildren(entity)) self(self, child);
        };
        for (const auto root : scene.GetRootEntities()) visit(visit, root);
        std::string encoded;
        if (!SerializeSceneDocument(captured, encoded, error)) return false;
        result = std::move(captured);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

void PopulateScene(const SceneDocument& document, AssetSystem& assets, Scene& destination) {
    for (const auto& definition : document.entities)
        destination.CreateEntityWithUuid(definition.id, definition.name);
    for (const auto& definition : document.entities) {
        const auto entity = destination.FindByUuid(definition.id);
        if (definition.parent) destination.SetParent(entity, destination.FindByUuid(*definition.parent));
        ApplyEntity(definition, assets, destination, entity);
    }
    if (document.activeCamera) destination.SetActiveCamera(destination.FindByUuid(*document.activeCamera));
    if (document.directionalLight) destination.SetDirectionalLight(destination.FindByUuid(*document.directionalLight));
    destination.SetEditorFolders(document.editorFolders);
}

bool CaptureEntities(const Scene& scene, const AssetSystem& assets, std::span<const entt::entity> entities,
                     std::vector<SceneEntityDefinition>& result, std::string& error) {
    try {
        // An entity is taken with its subtree only when no ancestor of it is taken too.
        const std::unordered_set<entt::entity> picked(entities.begin(), entities.end());
        const auto underPicked = [&](entt::entity entity) {
            for (auto parent = scene.GetParent(entity); parent != entt::null; parent = scene.GetParent(parent))
                if (picked.contains(parent)) return true;
            return false;
        };
        std::unordered_set<entt::entity> taken;
        std::vector<SceneEntityDefinition> captured;
        auto visit = [&](auto&& self, const entt::entity entity) -> void {
            if (scene.HasUnsupportedComponents(entity))
                throw std::invalid_argument("'" + scene.GetName(entity) + "' has components without a version 1 codec");
            captured.push_back(CaptureDefinition(scene, assets, entity));
            for (const auto child : scene.GetChildren(entity)) self(self, child);
        };
        for (const auto entity : entities) {
            if (!scene.IsValid(entity) || underPicked(entity) || !taken.insert(entity).second) continue;
            visit(visit, entity);
        }
        result = std::move(captured);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

std::vector<entt::entity> InstantiateEntities(std::span<const SceneEntityDefinition> definitions, AssetSystem& assets,
                                              Scene& destination, const bool freshIds) {
    // Captured UUID -> created entity, for parents inside the set.
    std::unordered_map<EntityUuid, entt::entity, EntityUuidHash> created;
    std::vector<entt::entity> entities;
    entities.reserve(definitions.size());
    for (const auto& definition : definitions) {
        const auto entity = freshIds ? destination.CreateEntity(definition.name)
                                     : destination.CreateEntityWithUuid(definition.id, definition.name);
        if (entity != entt::null) created.emplace(definition.id, entity);
        entities.push_back(entity);
    }
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& definition = definitions[i];
        const auto entity = entities[i];
        if (entity == entt::null) continue;
        if (definition.parent) {
            const auto inside = created.find(*definition.parent);
            const auto parent = inside != created.end() ? inside->second : destination.FindByUuid(*definition.parent);
            if (parent != entt::null) destination.SetParent(entity, parent);
        }
        ApplyEntity(definition, assets, destination, entity);
    }
    return entities;
}

bool CaptureEntity(const Scene& scene, const AssetSystem& assets, const entt::entity entity,
                   SceneEntityDefinition& result, std::string& error) {
    try {
        if (!scene.IsValid(entity)) throw std::invalid_argument("not a scene entity");
        result = CaptureDefinition(scene, assets, entity);
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

std::vector<entt::entity> ApplyEntityDefinitions(std::span<const SceneEntityDefinition> definitions, AssetSystem& assets,
                                                 Scene& scene) {
    std::vector<entt::entity> entities;
    entities.reserve(definitions.size());
    for (const auto& definition : definitions) {
        const auto found = scene.FindByUuid(definition.id);
        entities.push_back(found != entt::null ? found : scene.CreateEntityWithUuid(definition.id, definition.name));
    }
    const auto parentOf = [&scene](const SceneEntityDefinition& definition) {
        return definition.parent ? scene.FindByUuid(*definition.parent) : entt::entity{entt::null};
    };
    // Moving entities go to the root first: reparenting them one by one could make a cycle on the way.
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (entities[i] != entt::null && scene.GetParent(entities[i]) != parentOf(definitions[i]))
            scene.SetParent(entities[i], entt::null);
    }
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (entities[i] == entt::null) continue;
        scene.SetParent(entities[i], parentOf(definitions[i]));
        ApplyEntity(definitions[i], assets, scene, entities[i]);
    }
    return entities;
}

SceneSettings CaptureSceneSettings(const Scene& scene) {
    SceneSettings settings;
    if (const auto camera = scene.GetActiveCamera(); camera != entt::null) settings.activeCamera = scene.GetId(camera);
    if (const auto light = scene.GetDirectionalLight(); light != entt::null) settings.directionalLight = scene.GetId(light);
    const auto folders = scene.GetEditorFolders();
    settings.editorFolders.assign(folders.begin(), folders.end());
    return settings;
}

void ApplySceneSettings(const SceneSettings& settings, Scene& scene) {
    const SceneSettings current = CaptureSceneSettings(scene);
    const auto find = [&scene](const std::optional<EntityUuid>& id) { return id ? scene.FindByUuid(*id) : entt::entity{entt::null}; };
    if (current.activeCamera != settings.activeCamera) scene.SetActiveCamera(find(settings.activeCamera));
    if (current.directionalLight != settings.directionalLight) scene.SetDirectionalLight(find(settings.directionalLight));
    scene.SetEditorFolders(settings.editorFolders);
}
} // namespace engine
