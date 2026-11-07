#include <engine/scripting/LuaBindings.hpp>

#include <engine/scripting/LuaEntityHandle.hpp>

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

            "SetParent", &LuaEntityHandle::SetParent,
            "SetParentKeepingWorld",
            &LuaEntityHandle::SetParentKeepingWorld
        );
    }

    void RegisterLuaBindings(
        sol::state& lua,
        LuaBindingsContext& context
    ) {
        RegisterEntityHandleBindings(lua);
        RegisterSceneBindings(lua, context);
    }

} // namespace engine