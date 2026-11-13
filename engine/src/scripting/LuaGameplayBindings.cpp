#include <engine/navigation/NavigationMesh.hpp>
#include <engine/scripting/LuaGameplayBindings.hpp>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace engine {
namespace {
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using DirectX::SimpleMath::Quaternion;
using Result = std::tuple<bool, std::string>;

bool Finite(Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool Finite(Vector3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Finite(Quaternion q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}
template<class V> V Normalized(V v) {
    if (!Finite(v) || !std::isfinite(v.LengthSquared()) || v.LengthSquared() <= 0)
        throw sol::error("normalization requires a finite, nonzero value");
    v.Normalize();
    return v;
}

// This sentinel owns a nonblocking activation and provides the subsystem's weak
// lifetime check. No definition or context pointer is retained across Lua calls.
struct InputOwner {
    InputSubsystem* input;
    InputSubsystem::Context lifetime;
    explicit InputOwner(InputSubsystem& value) : input(&value) {
        value.DefineContext("Gameplay");
        lifetime = value.Activate("Gameplay");
    }
    InputSubsystem* Get() const { return lifetime ? input : nullptr; }
};

struct GameplayInputContext {
    std::shared_ptr<InputOwner> owner;
    std::string name;
    InputSubsystem::Context lease;

    bool Capture(int priority, bool keyboard, bool mouse) {
        auto* input = owner->Get();
        if (!input) return false;
        lease.Reset();
        lease = input->Activate(name, priority, keyboard, mouse);
        return static_cast<bool>(lease);
    }
    InputSubsystem::ContextDefinition& Definition() const {
        auto* input = owner->Get();
        if (!input) throw sol::error("input subsystem is unavailable");
        return input->DefineContext(name);
    }
};

struct InputBindings {
    LuaBindingsContext& context;
    std::shared_ptr<InputOwner> owner;
    std::shared_ptr<GameplayInputContext> gameplay;
    std::unordered_map<std::string, std::weak_ptr<GameplayInputContext>> contexts;

    explicit InputBindings(LuaBindingsContext& value) : context(value) { Refresh(); }
    void Refresh() {
        auto* input = context.engine ? context.engine->GetSubsystem<InputSubsystem>() : nullptr;
        if (owner && owner->Get() == input) return;
        owner.reset();
        gameplay.reset();
        contexts.clear();
        if (input) {
            owner = std::make_shared<InputOwner>(*input);
            gameplay = Get("Gameplay");
        }
    }
    std::shared_ptr<GameplayInputContext> Get(const std::string& name) {
        if (name.empty()) throw sol::error("input context name must not be empty");
        if (!owner || !owner->Get()) return {};
        if (auto found = contexts[name].lock()) return found;
        auto result = std::make_shared<GameplayInputContext>();
        result->owner = owner;
        result->name = name;
        owner->Get()->DefineContext(name);
        result->lease = owner->Get()->Activate(name); // never implicitly blocks host contexts
        contexts[name] = result;
        return result;
    }
    std::shared_ptr<GameplayInputContext> Default() { Refresh(); return gameplay; }
};

bool ValidKey(int key) {
    return (key >= int(PhysicalKey::A) && key <= int(PhysicalKey::Num0)) ||
        (key >= int(PhysicalKey::Enter) && key <= int(PhysicalKey::Space)) ||
        (key >= int(PhysicalKey::F1) && key <= int(PhysicalKey::F12)) ||
        (key >= int(PhysicalKey::Delete) && key <= int(PhysicalKey::Up)) ||
        key == int(PhysicalKey::NumpadEnter) ||
        (key >= int(PhysicalKey::LeftControl) && key <= int(PhysicalKey::RightMeta));
}
PhysicalKey CheckedKey(int key) {
    if (!ValidKey(key)) throw sol::error("invalid physical key");
    return static_cast<PhysicalKey>(key);
}

template<class V, class Table> void VectorMath(Table& table) {
    table[sol::meta_function::addition] = [](V a, V b) { return a + b; };
    table[sol::meta_function::subtraction] = [](V a, V b) { return a - b; };
    table[sol::meta_function::unary_minus] = [](V a) { return -a; };
    table[sol::meta_function::multiplication] = sol::overload(
        [](V a, float s) { return a * s; }, [](float s, V a) { return a * s; });
    table[sol::meta_function::division] = [](V a, float s) {
        if (!std::isfinite(s) || s == 0) throw sol::error("divisor must be finite and nonzero");
        return a / s;
    };
    table[sol::meta_function::equal_to] = [](V a, V b) { return a == b; };
    table["Length"] = [](V a) { return a.Length(); };
    table["LengthSquared"] = [](V a) { return a.LengthSquared(); };
    table["Normalize"] = [](V a) { return Normalized(a); };
    table["Normalized"] = [](V a) { return Normalized(a); };
    table["Dot"] = [](V a, V b) { return a.Dot(b); };
    table["Lerp"] = [](V a, V b, float t) { return V::Lerp(a, b, t); };
}

void RegisterMath(sol::state& lua) {
    auto v2 = lua.new_usertype<Vector2>("Vector2",
        sol::constructors<Vector2(), Vector2(float, float)>(), "x", &Vector2::x, "y", &Vector2::y);
    VectorMath<Vector2>(v2);
    sol::usertype<Vector3> v3 = lua["Vector3"];
    VectorMath<Vector3>(v3);
    v3["Cross"] = [](Vector3 a, Vector3 b) { return a.Cross(b); };
    sol::usertype<Quaternion> q = lua["Quaternion"];
    q["Identity"] = [] { return Quaternion::Identity; };
    q["Euler"] = [](float pitch, float yaw, float roll) {
        return Quaternion::CreateFromYawPitchRoll(yaw, pitch, roll);
    };
    q["YawPitchRoll"] = [](float yaw, float pitch, float roll) {
        return Quaternion::CreateFromYawPitchRoll(yaw, pitch, roll);
    };
    q["AxisAngle"] = [](Vector3 axis, float radians) {
        if (!std::isfinite(radians)) throw sol::error("angle must be finite");
        return Quaternion::CreateFromAxisAngle(Normalized(axis), radians);
    };
    q["Length"] = [](Quaternion a) { return a.Length(); };
    q["Normalize"] = [](Quaternion a) { return Normalized(a); };
    q["Normalized"] = [](Quaternion a) { return Normalized(a); };
    q["Slerp"] = [](Quaternion a, Quaternion b, float t) {
        if (!std::isfinite(t)) throw sol::error("interpolation parameter must be finite");
        return Normalized(Quaternion::Slerp(Normalized(a), Normalized(b), t));
    };
    q["Rotate"] = [](Quaternion a, Vector3 v) { return Vector3::Transform(v, Normalized(a)); };
    q["Compose"] = [](Quaternion a, Quaternion b) { return Normalized(a) * Normalized(b); };
    q[sol::meta_function::multiplication] = sol::overload(
        [](Quaternion a, Quaternion b) { return Normalized(a) * Normalized(b); },
        [](Quaternion a, Vector3 v) { return Vector3::Transform(v, Normalized(a)); });
}

void RegisterInput(sol::state& lua, LuaBindingsContext& context) {
    lua.new_usertype<GameplayInputContext>("InputContext", sol::no_constructor,
        "IsValid", [](const GameplayInputContext& c) { return static_cast<bool>(c.lease); },
        "Pressed", [](const GameplayInputContext& c, const std::string& n) { return c.lease.Pressed(n); },
        "Released", [](const GameplayInputContext& c, const std::string& n) { return c.lease.Released(n); },
        "Down", [](const GameplayInputContext& c, const std::string& n) { return c.lease.Down(n); },
        "Axis1D", [](const GameplayInputContext& c, const std::string& n) { return c.lease.Axis1D(n); },
        "Axis2D", [](const GameplayInputContext& c, const std::string& n) { return c.lease.Axis2D(n); },
        "MousePosition", [](const GameplayInputContext& c) { return c.lease.MousePosition(); },
        "MouseDelta", [](const GameplayInputContext& c) { return c.lease.MouseDelta(); },
        "MouseWheel", [](const GameplayInputContext& c) { return c.lease.WheelDelta(); },
        "Release", [](GameplayInputContext& c) { c.lease.Reset(); },
        "Reset", [](GameplayInputContext& c) { c.lease.Reset(); },
        "Capture", [](GameplayInputContext& c, sol::optional<int> priority,
                       sol::optional<bool> keyboard, sol::optional<bool> mouse) {
            return c.Capture(priority.value_or(0), keyboard.value_or(false), mouse.value_or(false));
        },
        "BindAction", [](GameplayInputContext& c, std::string name, int key) {
            c.Definition().BindAction(std::move(name), InputSubsystem::Binding::Keyboard(CheckedKey(key)));
        },
        "BindMouseAction", [](GameplayInputContext& c, std::string name, int button) {
            if (button < 0 || button >= int(MouseButton::Count)) throw sol::error("invalid mouse button");
            c.Definition().BindAction(std::move(name), InputSubsystem::Binding::Mouse(static_cast<MouseButton>(button)));
        },
        "BindAxis1D", [](GameplayInputContext& c, std::string name, int key, float value) {
            if (!std::isfinite(value)) throw sol::error("axis value must be finite");
            c.Definition().BindAxis1D(std::move(name), CheckedKey(key), value);
        },
        "BindAxis2D", [](GameplayInputContext& c, std::string name, int key, Vector2 value) {
            if (!Finite(value)) throw sol::error("axis value must be finite");
            c.Definition().BindAxis2D(std::move(name), CheckedKey(key), value);
        });
    auto input = lua.create_named_table("Input");
    auto state = std::make_shared<InputBindings>(context);
    input.set_function("GetContext", [state](const std::string& name) { state->Refresh(); return state->Get(name); });
    input.set_function("DefineContext", [state](const std::string& name) { state->Refresh(); return state->Get(name); });
    input.set_function("Pressed", [state](const std::string& n) { auto c = state->Default(); return c && c->lease.Pressed(n); });
    input.set_function("Released", [state](const std::string& n) { auto c = state->Default(); return c && c->lease.Released(n); });
    input.set_function("Down", [state](const std::string& n) { auto c = state->Default(); return c && c->lease.Down(n); });
    input.set_function("Axis1D", [state](const std::string& n) { auto c = state->Default(); return c ? c->lease.Axis1D(n) : 0.f; });
    input.set_function("Axis2D", [state](const std::string& n) { auto c = state->Default(); return c ? c->lease.Axis2D(n) : Vector2{}; });
    input.set_function("MouseDelta", [state] { auto c = state->Default(); return c ? c->lease.MouseDelta() : Vector2{}; });
    input.set_function("MousePosition", [state] { auto c = state->Default(); return c ? c->lease.MousePosition() : Vector2{}; });
    input.set_function("MouseWheel", [state] { auto c = state->Default(); return c ? c->lease.WheelDelta() : 0.f; });
    auto keys = lua.create_named_table("PhysicalKey");
    const char* letters[] = {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
    for (int i = 0; i < 26; ++i) keys[letters[i]] = int(PhysicalKey::A) + i;
#define LUA_KEY(name) keys[#name] = int(PhysicalKey::name)
    LUA_KEY(Num0); LUA_KEY(Num1); LUA_KEY(Num2); LUA_KEY(Num3); LUA_KEY(Num4);
    LUA_KEY(Num5); LUA_KEY(Num6); LUA_KEY(Num7); LUA_KEY(Num8); LUA_KEY(Num9);
    LUA_KEY(F1); LUA_KEY(F2); LUA_KEY(F3); LUA_KEY(F4); LUA_KEY(F5); LUA_KEY(F6);
    LUA_KEY(F7); LUA_KEY(F8); LUA_KEY(F9); LUA_KEY(F10); LUA_KEY(F11); LUA_KEY(F12);
    LUA_KEY(Enter); LUA_KEY(Escape); LUA_KEY(Backspace); LUA_KEY(Tab); LUA_KEY(Space);
    LUA_KEY(Delete); LUA_KEY(Right); LUA_KEY(Left); LUA_KEY(Down); LUA_KEY(Up); LUA_KEY(NumpadEnter);
    LUA_KEY(LeftControl); LUA_KEY(LeftShift); LUA_KEY(LeftAlt); LUA_KEY(LeftMeta);
    LUA_KEY(RightControl); LUA_KEY(RightShift); LUA_KEY(RightAlt); LUA_KEY(RightMeta);
#undef LUA_KEY
    auto buttons = lua.create_named_table("MouseButton");
    buttons["Left"] = int(MouseButton::Left);
    buttons["Right"] = int(MouseButton::Right);
    buttons["Middle"] = int(MouseButton::Middle);
    input["PhysicalKey"] = keys;
    input["MouseButton"] = buttons;
}

Result PhysicsCommand(LuaBindingsContext& context, sol::object entity, sol::object vector,
                      sol::object rotation, int command) {
    auto* physics = context.engine ? context.engine->GetSubsystem<PhysicsSubsystem>() : nullptr;
    if (!physics) return {false, "physics subsystem is unavailable"};
    if (!entity.is<LuaEntityHandle>()) return {false, "expected an Entity handle"};
    const auto& handle = entity.as<const LuaEntityHandle&>();
    auto* scene = context.GetScene();
    if (!scene || !handle.IsValid() || !handle.BelongsTo(*scene))
        return {false, "entity is invalid or does not belong to the current scene"};
    if (!vector.is<Vector3>() || !Finite(vector.as<Vector3>()))
        return {false, "expected a finite Vector3"};
    Quaternion q = Quaternion::Identity;
    if (command >= 2) {
        if (!rotation.is<Quaternion>()) return {false, "expected a normalized Quaternion"};
        q = rotation.as<Quaternion>();
        if (!Finite(q) || std::abs(q.LengthSquared() - 1.f) > 1.e-4f)
            return {false, "rotation must be finite and normalized"};
    }
    std::string error;
    bool ok = false;
    const auto e = handle.Resolve();
    const auto v = vector.as<Vector3>();
    switch (command) {
    case 0: ok = physics->SetLinearVelocity(e, v, error); break;
    case 1: ok = physics->AddImpulse(e, v, error); break;
    case 2: ok = physics->MoveKinematic(e, v, q, error); break;
    case 3: ok = physics->Teleport(e, v, q, error); break;
    }
    return {ok, error};
}
} // namespace

void RegisterGameplayBindings(sol::state& lua, LuaBindingsContext& context) {
    RegisterMath(lua);
    RegisterInput(lua, context);
    auto physics = lua.create_named_table("Physics");
    physics.set_function("SetLinearVelocity", [&context](sol::object e, sol::object v, sol::this_state s) {
        return PhysicsCommand(context, e, v, sol::make_object(s, sol::nil), 0);
    });
    physics.set_function("AddImpulse", [&context](sol::object e, sol::object v, sol::this_state s) {
        return PhysicsCommand(context, e, v, sol::make_object(s, sol::nil), 1);
    });
    physics.set_function("MoveKinematic", [&context](sol::object e, sol::object v, sol::object q) {
        return PhysicsCommand(context, e, v, q, 2);
    });
    physics.set_function("Teleport", [&context](sol::object e, sol::object v, sol::object q) {
        return PhysicsCommand(context, e, v, q, 3);
    });
    auto navigation = lua.create_named_table("Navigation");
    navigation.set_function("FindPath", [&context](Vector3 start, Vector3 goal) {
        return sol::as_table(context.engine
            ? context.engine->GetNavigationMesh().FindPath(start, goal)
            : std::vector<Vector3>{});
    });
    auto time = lua.create_named_table("Time");
    time.set_function("Delta", [&context] { return context.engine ? context.engine->GetFrame().deltaSeconds : 0.; });
    time.set_function("SimulationTime", [&context] { return context.engine ? context.engine->GetFrame().simulationTimeSeconds : 0.; });
    time.set_function("RealDelta", [&context] { return context.engine ? context.engine->GetFrame().realDeltaSeconds : 0.; });
    time.set_function("RealTime", [&context] { return context.engine ? context.engine->GetFrame().realTimeSeconds : 0.; });
    time.set_function("IsPaused", [&context] { return context.engine && context.engine->IsPaused(); });
}

} // namespace engine
