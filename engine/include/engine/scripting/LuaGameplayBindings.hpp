#pragma once

#include <sol/sol.hpp>
#include <engine/engine_export.hpp>
#include <engine/scripting/LuaBindingsContext.hpp>

namespace engine {

/// Call after RegisterLuaBindings and the host's Vector3 registration.
/// The context must outlive the Lua state, just as for RegisterLuaBindings.
/// Input.GetContext/DefineContext(name) share a checked activation wrapper.
/// Capture(priority = 0, blockKeyboard = false, blockMouse = false) explicitly
/// reactivates it; Release/Reset releases it. Gameplay is retained by the Lua
/// registration so default Input queries are active before their first call.
/// BindAction uses PhysicalKey; BindMouseAction uses MouseButton. MouseDelta
/// and MouseWheel are relative frame values. MousePosition is in viewport/client
/// render-surface pixels and is zero when mouse-blocked, reset, or focus is lost.
/// Math Normalize returns a copy and rejects zero/nonfinite values. Quaternion
/// Euler(pitch,yaw,roll), YawPitchRoll(yaw,pitch,roll), and AxisAngle use radians;
/// composition and rotation follow SimpleMath. Physics poses are parent-local,
/// require normalized rotations, and all commands return (bool, errorString).
ENGINE_API void RegisterGameplayBindings(sol::state& lua, LuaBindingsContext& context);

} // namespace engine
