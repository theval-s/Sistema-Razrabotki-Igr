#include <engine/scripting/LuaBindings.hpp>

#include <engine/scripting/LuaEntityHandle.hpp>
#include <engine/scripting/LuaComponentBindings.hpp>

namespace engine {

    void RegisterEntityHandleBindings(sol::state& lua) {

        using Vector3 = DirectX::SimpleMath::Vector3;
        using Quaternion = DirectX::SimpleMath::Quaternion;

        lua.new_usertype<Quaternion>(
            "Quaternion",
            sol::constructors<
            Quaternion(),
            Quaternion(float, float, float, float)
            >(),
            "x", &Quaternion::x,
            "y", &Quaternion::y,
            "z", &Quaternion::z,
            "w", &Quaternion::w
        );

        lua.new_usertype<LuaEntityHandle>(
            "Entity",
            sol::no_constructor,

            "IsValid", &LuaEntityHandle::IsValid,

            "GetId", &LuaEntityHandle::GetId,
            "GetName", &LuaEntityHandle::GetName,
            "SetName", &LuaEntityHandle::SetName,

            "GetPosition", &LuaEntityHandle::GetPosition,
            "SetPosition", &LuaEntityHandle::SetPosition,

            "GetRotation", &LuaEntityHandle::GetRotation,
            "SetRotation", &LuaEntityHandle::SetRotation,

            "GetScale", &LuaEntityHandle::GetScale,
            "SetScale", &LuaEntityHandle::SetScale,

            "GetTags", &LuaEntityHandle::GetTags,
            "HasTag", &LuaEntityHandle::HasTag,
            "AddTag", &LuaEntityHandle::AddTag,
            "RemoveTag", &LuaEntityHandle::RemoveTag,

            "GetParent", &LuaEntityHandle::GetParent,
            "GetChildren", &LuaEntityHandle::GetChildren,

            "GetWorldPosition", &LuaEntityHandle::GetWorldPosition,
            "SetWorldPosition", &LuaEntityHandle::SetWorldPosition,
            "GetWorldRotation", &LuaEntityHandle::GetWorldRotation,
            "SetWorldRotation", &LuaEntityHandle::SetWorldRotation,
            "GetWorldScale", &LuaEntityHandle::GetWorldScale,
            "SetWorldScale", &LuaEntityHandle::SetWorldScale,
            "Forward", &LuaEntityHandle::Forward,
            "Right", &LuaEntityHandle::Right,
            "Up", &LuaEntityHandle::Up,
            sol::meta_function::equal_to, &LuaEntityHandle::Equals,
            "SetParent", [](const LuaEntityHandle& self, sol::optional<LuaEntityHandle> parent) {
                return parent ? self.SetParent(*parent) : self.Unparent();
            },
            "SetParentKeepingWorld",
            [](const LuaEntityHandle& self, sol::optional<LuaEntityHandle> parent) {
                return parent ? self.SetParentKeepingWorld(*parent) : self.Unparent(true);
            }
        );
    }

    void RegisterLuaBindings(
        sol::state& lua,
        LuaBindingsContext& context
    ) {
        RegisterEntityHandleBindings(lua);
        RegisterComponentBindings(lua, context);
        RegisterSceneBindings(lua, context);
    }

} // namespace engine
