
#include <engine/scripting/LuaSceneBindings.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <limits>

#include <cstdint>
#include <vector>

#include <engine/scene/Scene.hpp>
#include <engine/scene/StateValue.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>

namespace engine {

    namespace {

        bool LuaObjectToStateValue(
            const sol::object& object,
            StateValue& result,
            std::string& error,
            std::size_t depth
        );

        bool LuaTableToStateValue(
            const sol::table& table,
            StateValue& result,
            std::string& error,
            std::size_t depth
        ) {
            if (depth > 64) {
                error = "Lua table nesting exceeds maximum depth";
                return false;
            }

            StateValue::Object object;
            StateValue::Array array;

            bool hasStringKeys = false;
            bool hasIntegerKeys = false;

            std::size_t count = 0;
            std::size_t maxIndex = 0;

            for (const auto& entry : table) {
                const sol::object& key = entry.first;

                if (key.get_type() == sol::type::string) {
                    hasStringKeys = true;
                }
                else if (key.get_type() == sol::type::number) {
                    const double index = key.as<double>();

                    if (!std::isfinite(index) ||
                        index < 1.0 ||
                        std::floor(index) != index ||
                        index > static_cast<double>(
                            std::numeric_limits<std::size_t>::max()
                            )) {
                        error = "Lua table contains an invalid array index";
                        return false;
                    }

                    hasIntegerKeys = true;

                    const auto arrayIndex =
                        static_cast<std::size_t>(index);

                    maxIndex = std::max(maxIndex, arrayIndex);
                }
                else {
                    error = "Lua table keys must be strings or positive integers";
                    return false;
                }

                ++count;
            }
            if (hasStringKeys && hasIntegerKeys) {
                error = "Mixed Lua table keys are not supported";
                return false;
            }
            if (hasIntegerKeys) {
                if (maxIndex != count) {
                    error = "Sparse Lua arrays are not supported";
                    return false;
                }

                array.resize(count);

                for (const auto& entry : table) {
                    const std::size_t index =
                        static_cast<std::size_t>(
                            entry.first.as<double>()
                            );

                    if (!LuaObjectToStateValue(
                        entry.second,
                        array[index - 1],
                        error,
                        depth + 1
                    )) {
                        return false;
                    }
                }

                result = StateValue(std::move(array));
            }
            else {
                for (const auto& entry : table) {
                    const std::string key =
                        entry.first.as<std::string>();

                    StateValue value;

                    if (!LuaObjectToStateValue(
                        entry.second,
                        value,
                        error,
                        depth + 1
                    )) {
                        return false;
                    }

                    object.emplace(key, std::move(value));
                }

                result = StateValue(std::move(object));
            }

            return true;
        }

        bool LuaObjectToStateValue(
            const sol::object& object,
            StateValue& result,
            std::string& error,
            std::size_t depth
        ) {
            switch (object.get_type()) {
            case sol::type::nil:
            case sol::type::none:
                result = StateValue(nullptr);
                return true;

            case sol::type::boolean:
                result = StateValue(object.as<bool>());
                return true;

            case sol::type::number: {
                const double number = object.as<double>();

                if (!std::isfinite(number)) {
                    error = "Lua number must be finite";
                    return false;
                }

                result = StateValue(number);
                return true;
            }

            case sol::type::string:
                result = StateValue(object.as<std::string>());
                return true;

            case sol::type::table:
                return LuaTableToStateValue(
                    object.as<sol::table>(),
                    result,
                    error,
                    depth
                );

            default:
                error = "Unsupported Lua value type";
                return false;
            }
        }

        struct LuaSpawnOptions {
            DirectX::SimpleMath::Vector3 position{ 0, 0, 0 };
            DirectX::SimpleMath::Quaternion rotation{ 0, 0, 0, 1 };
            DirectX::SimpleMath::Vector3 scale{ 1, 1, 1 };

            LuaEntityHandle parent;
        };

        bool IsFinite(const DirectX::SimpleMath::Vector3& value) {
            return std::isfinite(value.x) &&
                std::isfinite(value.y) &&
                std::isfinite(value.z);
        }
        bool IsFinite(const DirectX::SimpleMath::Quaternion& value) {
            return std::isfinite(value.x) &&
                std::isfinite(value.y) &&
                std::isfinite(value.z) &&
                std::isfinite(value.w);
        }

        void NormalizeRotation(
            DirectX::SimpleMath::Quaternion& rotation
        ) {
            if (!IsFinite(rotation)) {
                throw std::runtime_error(
                    "Rotation components must be finite"
                );
            }

            const float lengthSquared =
                rotation.x * rotation.x +
                rotation.y * rotation.y +
                rotation.z * rotation.z +
                rotation.w * rotation.w;

            if (lengthSquared <= 1e-12f) {
                throw std::runtime_error(
                    "Rotation quaternion must have non-zero length"
                );
            }

            const float inverseLength =
                1.0f / std::sqrt(lengthSquared);

            rotation.x *= inverseLength;
            rotation.y *= inverseLength;
            rotation.z *= inverseLength;
            rotation.w *= inverseLength;
        }
        void ValidateSpawnOptions(LuaSpawnOptions& options) {
            if (!IsFinite(options.position)) {
                throw std::runtime_error(
                    "Position components must be finite"
                );
            }

            if (!IsFinite(options.scale)) {
                throw std::runtime_error(
                    "Scale components must be finite"
                );
            }

            NormalizeRotation(options.rotation);
        }

    } // namespace

    void RegisterSceneBindings(
        sol::state& lua,
        LuaBindingsContext& context
    ) {
        sol::table scene = lua["Scene"];

        // ---------------------------------------------------------
        // Scene.FindByName(name)
        // ---------------------------------------------------------

        scene.set_function(
            "FindByName",
            [&context](const std::string& name) -> LuaEntityHandle {
                Scene* current = context.GetScene();

                if (!current)
                    return {};

                const auto entity = current->FindFirstByName(name);

                if (entity == entt::null)
                    return {};

                return LuaEntityHandle::FromScene(*current, entity);
            }
        );

        // ---------------------------------------------------------
        // Scene.FindAllByName(name)
        // ---------------------------------------------------------

        scene.set_function(
            "FindAllByName",
            [&context](
                sol::this_state state,
                const std::string& name
                ) {
                    sol::state_view lua(state);
                    sol::table result = lua.create_table();

                    Scene* current = context.GetScene();

                    if (!current)
                        return result;

                    const auto entities = current->FindAllByName(name);

                    for (const auto entity : entities) {
                        result.add(
                            LuaEntityHandle::FromScene(*current, entity)
                        );
                    }

                    return result;
            }
        );

        // ---------------------------------------------------------
        // Scene.FindByTag(tag)
        // ---------------------------------------------------------

        scene.set_function(
            "FindByTag",
            [&context](
                sol::this_state state,
                const std::string& tag
                ) {
                    sol::state_view lua(state);
                    sol::table result = lua.create_table();

                    Scene* current = context.GetScene();

                    if (!current)
                        return result;

                    for (const auto entity : current->FindWithTag(tag)) {
                        result.add(
                            LuaEntityHandle::FromScene(*current, entity)
                        );
                    }

                    return result;
            }
        );

        // ---------------------------------------------------------
        // Scene.FindById(id)
        // ---------------------------------------------------------

        scene.set_function(
            "FindById",
            [&context](const std::string& id) -> LuaEntityHandle {
                Scene* current = context.GetScene();

                if (!current)
                    return {};

                const auto entity = current->FindById(id);

                if (entity == entt::null)
                    return {};

                return LuaEntityHandle::FromScene(*current, entity);
            }
        );

        // ---------------------------------------------------------
        // Scene.FindByUuid(uuid)
        // ---------------------------------------------------------

        scene.set_function(
            "FindByUuid",
            [&context](const std::string& id) -> LuaEntityHandle {
                Scene* current = context.GetScene();

                if (!current)
                    return {};

                const auto uuid = EntityUuid::Parse(id);

                if (!uuid)
                    return {};

                const auto entity = current->FindByUuid(*uuid);

                if (entity == entt::null)
                    return {};

                return LuaEntityHandle::FromScene(*current, entity);
            }
        );

        // ---------------------------------------------------------
        // Scene.Spawn(name, options)
        // ---------------------------------------------------------

        scene.set_function(
            "Spawn",
            [&context](
                const std::string& name,
                sol::optional<sol::table> options
                ) -> LuaEntityHandle {

                    Scene* current = context.GetScene();

                    if (!current) {
                        throw std::runtime_error("No active scene");
                    }

                    LuaSpawnOptions config;
                    if (options) {
                        sol::table data = *options;

                        sol::object position = data["position"];
                        sol::object rotation = data["rotation"];
                        sol::object scale = data["scale"];
                        sol::object parent = data["parent"];

                        if (position.valid() &&
                            position.get_type() != sol::type::nil) {
                            config.position =
                                position.as<DirectX::SimpleMath::Vector3>();
                        }

                        if (rotation.valid() &&
                            rotation.get_type() != sol::type::nil) {
                            config.rotation =
                                rotation.as<DirectX::SimpleMath::Quaternion>();
                        }

                        if (scale.valid() &&
                            scale.get_type() != sol::type::nil) {
                            config.scale =
                                scale.as<DirectX::SimpleMath::Vector3>();
                        }

                        if (parent.valid() &&
                            parent.get_type() != sol::type::nil) {
                            config.parent = parent.as<LuaEntityHandle>();

                            if (!config.parent.IsValid()) {
                                throw std::runtime_error(
                                    "Invalid parent entity"
                                );
                            }
                        }
                    }


                    ValidateSpawnOptions(config);
                    entt::entity parentEntity = entt::null;

                    if (config.parent.IsValid()) {
                        if (!config.parent.BelongsTo(*current)) {
                            throw std::runtime_error(
                                "Parent belongs to another scene"
                            );
                        }
                        parentEntity = config.parent.Resolve();
                    }

                    const auto entity = current->CreateEntity(
                        name,
                        parentEntity
                    );

                    if (entity == entt::null) {
                        throw std::runtime_error(
                            "Failed to spawn entity"
                        );
                    }

                    Transform transform;

                    transform.position = config.position;
                    transform.rotation = config.rotation;
                    transform.scale = config.scale;

                    current->SetTransform(entity, transform);

                    return LuaEntityHandle::FromScene(*current, entity);
            }
        );

        // ---------------------------------------------------------
        // Scene.Destroy(entity)
        // ---------------------------------------------------------

        scene.set_function(
            "Destroy",
            [&context](LuaEntityHandle entity) {
                entity.GetScene()->DestroyEntity(entity.Resolve());
            }
        );

        // ---------------------------------------------------------
        // Scene.AttachController(entity, type, data)
        // ---------------------------------------------------------

        scene.set_function(
            "AttachController",
            [&context](
                LuaEntityHandle entity,
                const std::string& type,
                sol::optional<sol::table> data
                ) {

                    Scene* current = context.GetScene();

                    if (!current || !entity.BelongsTo(*current)) {
                        throw std::runtime_error(
                            "Invalid entity"
                        );
                    }

                    StateValue controllerData{
                        StateValue::Object{}
                    };

                    std::string error;

                    if (data) {
                        if (!LuaTableToStateValue(
                            *data,
                            controllerData,
                            error,
                            0
                        )) {
                            throw std::runtime_error(error);
                        }
                    }

                    ControllerDefinition definition;

                    definition.type = type;
                    definition.version = 1;
                    definition.enabled = true;
                    definition.data = std::move(controllerData);
                    if (!current->ValidateController(definition, error)) {
                        throw std::runtime_error(error);
                    }
                    if (!context.enqueueSceneCommand) {
                        throw std::runtime_error(
                            "Scene command queue unavailable"
                        );
                    }

                    context.enqueueSceneCommand(
                        [entity, definition = std::move(definition)]() mutable {

                            if (!entity.IsValid())
                                return;

                            Scene* scene = entity.GetScene();

                            if (!scene)
                                return;

                            scene->AttachController(
                                entity.Resolve(),
                                std::move(definition)
                            );
                        }
                    );
            }
        );
    }

} // namespace engine
