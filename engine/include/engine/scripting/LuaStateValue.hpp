#pragma once

#include <engine/scene/StateValue.hpp>
#include <sol/sol.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace engine {
namespace lua_state_detail {
inline bool Read(const sol::object& object, StateValue& value, std::string& error,
                 std::unordered_set<const void*>& path, unsigned depth) {
    if (depth > 64) { error = "script state exceeds maximum depth (64)"; return false; }
    switch (object.get_type()) {
    case sol::type::nil: value = StateValue(nullptr); return true;
    case sol::type::boolean: value = StateValue(object.as<bool>()); return true;
    case sol::type::string: value = StateValue(object.as<std::string>()); return true;
    case sol::type::number: {
        const double number = object.as<double>();
        if (!std::isfinite(number)) { error = "script state numbers must be finite"; return false; }
        object.push();
        const bool integer = lua_isinteger(object.lua_state(), -1);
        const auto integerValue = lua_tointeger(object.lua_state(), -1);
        lua_pop(object.lua_state(), 1);
        value = integer ? StateValue(static_cast<std::int64_t>(integerValue)) : StateValue(number);
        return true;
    }
    case sol::type::table: break;
    default: error = "script state supports only nil, booleans, finite numbers, strings and tables"; return false;
    }
    sol::table table = object.as<sol::table>();
    table.push();
    const void* identity = lua_topointer(table.lua_state(), -1);
    lua_pop(table.lua_state(), 1);
    if (!path.insert(identity).second) { error = "cyclic script state table"; return false; }
    struct Pop { std::unordered_set<const void*>& path; const void* id; ~Pop() { path.erase(id); } } pop{path, identity};
    StateValue::Object fields;
    std::map<std::size_t, StateValue> items;
    for (const auto& pair : table) {
        StateValue child;
        if (!Read(pair.second, child, error, path, depth + 1)) return false;
        if (pair.first.get_type() == sol::type::string) fields.emplace(pair.first.as<std::string>(), std::move(child));
        else if (pair.first.get_type() == sol::type::number) {
            const double index = pair.first.as<double>();
            if (!std::isfinite(index) || index < 1 || std::floor(index) != index ||
                index >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
                error = "script state array keys must be contiguous positive integers"; return false;
            }
            items.emplace(static_cast<std::size_t>(index), std::move(child));
        } else { error = "script state object keys must be strings"; return false; }
    }
    if (!fields.empty() && !items.empty()) { error = "mixed script state array/object table"; return false; }
    if (!items.empty()) {
        StateValue::Array array;
        for (auto& [index, child] : items) {
            if (index != array.size() + 1) { error = "sparse script state array"; return false; }
            array.push_back(std::move(child));
        }
        value = StateValue(std::move(array));
    } else value = StateValue(std::move(fields)); // {} always means an empty object.
    return true;
}
inline sol::object Write(sol::state_view lua, const StateValue& value, unsigned depth) {
    if (depth > 64) throw std::runtime_error("script state exceeds maximum depth (64)");
    return std::visit([&](const auto& item) -> sol::object {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, StateValue::Array>) {
            sol::table table = lua.create_table();
            for (std::size_t i = 0; i < item.size(); ++i) table.raw_set(i + 1, Write(lua, item[i], depth + 1));
            return sol::make_object(lua, table);
        } else if constexpr (std::is_same_v<T, StateValue::Object>) {
            sol::table table = lua.create_table();
            for (const auto& [key, child] : item) table.raw_set(key, Write(lua, child, depth + 1));
            return sol::make_object(lua, table);
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
            // Nil would silently erase an object key or create a sparse array.
            if (depth != 0) throw std::runtime_error("null cannot be represented as a Lua table entry");
            return sol::make_object(lua, sol::nil);
        }
        else {
            if constexpr (std::is_same_v<T, double>)
                if (!std::isfinite(item)) throw std::runtime_error("script state numbers must be finite");
            if constexpr (std::is_same_v<T, std::uint64_t>)
                if (item > static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()))
                    throw std::runtime_error("script state integer exceeds Lua integer range");
            return sol::make_object(lua, item);
        }
    }, value.value);
}
} // namespace lua_state_detail

inline bool LuaToStateValue(const sol::object& object, StateValue& value, std::string& error) {
    std::unordered_set<const void*> path;
    StateValue result;
    if (!lua_state_detail::Read(object, result, error, path, 0)) return false;
    value = std::move(result);
    error.clear();
    return true;
}
inline sol::object StateValueToLua(sol::state_view lua, const StateValue& value) {
    return lua_state_detail::Write(lua, value, 0);
}
} // namespace engine
