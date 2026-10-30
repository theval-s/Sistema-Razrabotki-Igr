#include <engine/scene/SceneDocument.hpp>

#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/scene/Scene.hpp>

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
Json Encode(const std::optional<std::string>& value) { return value ? Json(*value) : Json(nullptr); }

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

std::optional<std::string> OptionalId(const Json& j) {
    if (j.is_null()) return std::nullopt;
    auto id = j.get<std::string>();
    if (!IsUuid(id)) throw std::invalid_argument("invalid entity UUID: " + id);
    return id;
}

SceneEntityDefinition DecodeEntity(const Json& j) {
    Fields(j, {"id", "name", "tags", "parent", "components"});
    SceneEntityDefinition entity;
    entity.id = j.at("id").get<std::string>();
    if (!IsUuid(entity.id)) throw std::invalid_argument("invalid entity UUID: " + entity.id);
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
    const auto& components = j.at("components");
    Fields(components, {"Transform", "MeshRenderer", "Camera", "DirectionalLight", "PointLight", "SpotLight", "Controller", "Bounds", "GamePersistence"});
    if (components.contains("GamePersistence")) {
        const auto& persist = components.at("GamePersistence");
        Fields(persist, {"transform", "controllerState"});
        entity.persistence = GamePersistence{persist.at("transform").get<bool>(), persist.at("controllerState").get<bool>()};
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
    std::unordered_map<std::string, std::size_t> ids;
    for (std::size_t i = 0; i < document.entities.size(); ++i)
        if (!ids.emplace(document.entities[i].id, i).second)
            throw std::invalid_argument("duplicate entity UUID: " + document.entities[i].id);
    for (const auto& entity : document.entities) {
        std::set<std::string> ancestors;
        const SceneEntityDefinition* current = &entity;
        while (current->parent) {
            if (!ancestors.insert(current->id).second) throw std::invalid_argument("hierarchy cycle at " + entity.id);
            const auto it = ids.find(*current->parent);
            if (it == ids.end()) throw std::invalid_argument("unknown parent UUID: " + *current->parent);
            current = &document.entities[it->second];
        }
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
    Fields(j, {"version", "entities", "activeCamera", "directionalLight"});
    if (j.at("version") != 1) throw std::invalid_argument("unsupported scene version");
    SceneDocument result;
    if (!j.at("entities").is_array()) throw std::invalid_argument("entities must be an array");
    for (const auto& item : j.at("entities")) result.entities.push_back(DecodeEntity(item));
    if (j.contains("activeCamera")) result.activeCamera = OptionalId(j.at("activeCamera"));
    if (j.contains("directionalLight")) result.directionalLight = OptionalId(j.at("directionalLight"));
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
        if (entity.bounds)
            components["Bounds"] = {{"min", Encode(entity.bounds->local.min)}, {"max", Encode(entity.bounds->local.max)}};
        if (entity.persistence)
            components["GamePersistence"] = {{"transform", entity.persistence->transform},
                                              {"controllerState", entity.persistence->controllerState}};
        root["entities"].push_back({{"id", entity.id}, {"name", entity.name}, {"tags", entity.tags},
                                     {"parent", Encode(entity.parent)},
                                     {"components", std::move(components)}});
    }
    return root;
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
        const auto camera = scene.GetActiveCamera();
        if (camera != entt::null) captured.activeCamera = scene.GetId(camera);
        const auto light = scene.GetDirectionalLight();
        if (light != entt::null) captured.directionalLight = scene.GetId(light);
        auto visit = [&](auto&& self, const entt::entity entity) -> void {
            SceneEntityDefinition definition;
            definition.id = scene.GetId(entity);
            definition.name = scene.GetName(entity);
            const auto tags = scene.GetTags(entity);
            definition.tags.assign(tags.begin(), tags.end());
            if (const auto* controller = scene.GetControllerDefinition(entity)) definition.controller = *controller;
            if (const auto* bounds = scene.TryGetComponent<Bounds>(entity)) definition.bounds = *bounds;
            if (const auto* persistence = scene.TryGetComponent<GamePersistence>(entity)) definition.persistence = *persistence;
            if (auto parent = scene.GetParent(entity); parent != entt::null)
                definition.parent = scene.GetId(parent);
            definition.transform = scene.GetTransform(entity);
            if (const auto* renderer = scene.TryGetComponent<MeshRenderer>(entity)) {
                auto authored = [&](const AssetReference& ref, CatalogAssetKind kind)
                    -> std::optional<AssetReference> {
                    if (!ref.id.IsValid()) return std::nullopt;
                    if (const auto* entry = assets.GetCatalog().Find(ref.id, kind))
                        return AssetReference{ref.id, entry->path};
                    return ref; // missing asset: keep the hand-editable recovery hint
                };
                auto mesh = authored(renderer->meshReference, CatalogAssetKind::Mesh);
                if (!mesh) mesh = assets.ReferenceFor(renderer->mesh);
                auto material = authored(renderer->materialReference, CatalogAssetKind::MaterialInstance);
                if (!material) material = assets.ReferenceFor(renderer->materialInstance);
                if (!mesh || !material)
                    throw std::invalid_argument("renderer has no file-backed mesh or material instance on " + definition.id);
                definition.renderer = SceneEntityDefinition::Mesh{*mesh, *material};
            }
            if (const auto* component = scene.TryGetComponent<Camera>(entity)) definition.camera = *component;
            if (const auto* component = scene.TryGetComponent<DirectionalLight>(entity)) definition.directionalLight = *component;
            if (const auto* component = scene.TryGetComponent<PointLight>(entity)) definition.pointLight = *component;
            if (const auto* component = scene.TryGetComponent<SpotLight>(entity)) definition.spotLight = *component;
            captured.entities.push_back(std::move(definition));
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
        destination.CreateEntityWithId(definition.id, definition.name);
    for (const auto& definition : document.entities) {
        const auto entity = destination.FindById(definition.id);
        for (const auto& tag : definition.tags) destination.AddTag(entity, tag);
        if (definition.parent) destination.SetParent(entity, destination.FindById(*definition.parent));
        destination.GetTransform(entity) = definition.transform;
        if (definition.renderer) destination.AddComponent<MeshRenderer>(entity,
            assets.RequestMesh(definition.renderer->mesh),
            assets.RequestMaterialInstance(definition.renderer->materialInstance),
            definition.renderer->mesh, definition.renderer->materialInstance);
        if (definition.camera) destination.AddComponent<Camera>(entity, *definition.camera);
        if (definition.directionalLight) destination.AddComponent<DirectionalLight>(entity, *definition.directionalLight);
        if (definition.pointLight) destination.AddComponent<PointLight>(entity, *definition.pointLight);
        if (definition.spotLight) destination.AddComponent<SpotLight>(entity, *definition.spotLight);
        if (definition.bounds) destination.AddComponent<Bounds>(entity, *definition.bounds);
        if (definition.persistence) destination.AddComponent<GamePersistence>(entity, *definition.persistence);
        if (definition.controller) destination.AttachController(entity, *definition.controller);
    }
    if (document.activeCamera) destination.SetActiveCamera(destination.FindById(*document.activeCamera));
    if (document.directionalLight) destination.SetDirectionalLight(destination.FindById(*document.directionalLight));
}
} // namespace engine
