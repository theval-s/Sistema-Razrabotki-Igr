#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <engine/engine_export.hpp>

namespace engine {
// A JSON-compatible tree; controller code needs no JSON library or text parsing.
// Objects and arrays can be freely nested. Reads of missing fields or values
// of a different type return empty/default values; use HasValue to distinguish
// a missing field from a present field (including one containing JSON null).
struct StateValue {
    using Object = std::map<std::string, StateValue>;
    using Array = std::vector<StateValue>;
    std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object> value = Object{};

    StateValue() = default;
    StateValue(std::nullptr_t) : value(nullptr) {}
    StateValue(bool v) : value(v) {}
    StateValue(int v) : value(std::int64_t(v)) {}
    StateValue(std::int64_t v) : value(v) {}
    StateValue(std::uint64_t v) : value(v) {}
    StateValue(double v) : value(v) {}
    StateValue(std::string v) : value(std::move(v)) {}
    StateValue(const char* v) : value(std::string(v)) {}
    StateValue(Array v) : value(std::move(v)) {}
    StateValue(Object v) : value(std::move(v)) {}

    [[nodiscard]] const StateValue* Find(std::string_view key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) return nullptr;
        const auto it = object->find(std::string(key));
        return it == object->end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool HasValue(std::string_view key) const { return Find(key) != nullptr; }
    [[nodiscard]] const StateValue& At(std::string_view key) const {
        if (const auto* found = Find(key)) return *found;
        static const StateValue missing{nullptr};
        return missing;
    }
    [[nodiscard]] StateValue& operator[](std::string key) {
        auto* object = std::get_if<Object>(&value);
        if (!object) object = &value.emplace<Object>();
        return (*object)[std::move(key)];
    }
    [[nodiscard]] double Number() const {
        if (const auto* v = std::get_if<double>(&value)) return *v;
        if (const auto* v = std::get_if<std::int64_t>(&value)) return static_cast<double>(*v);
        if (const auto* v = std::get_if<std::uint64_t>(&value)) return static_cast<double>(*v);
        return 0.0;
    }
    [[nodiscard]] std::uint64_t Unsigned() const {
        if (const auto* v = std::get_if<std::uint64_t>(&value)) return *v;
        if (const auto* v = std::get_if<std::int64_t>(&value); v && *v >= 0) return static_cast<std::uint64_t>(*v);
        return 0;
    }
    [[nodiscard]] bool Boolean() const {
        if (const auto* v = std::get_if<bool>(&value)) return *v;
        return false;
    }
    [[nodiscard]] const std::string& String() const {
        if (const auto* v = std::get_if<std::string>(&value)) return *v;
        static const std::string empty;
        return empty;
    }
    [[nodiscard]] const Array& Items() const {
        if (const auto* v = std::get_if<Array>(&value)) return *v;
        static const Array empty;
        return empty;
    }
    [[nodiscard]] const Object& Fields() const {
        if (const auto* v = std::get_if<Object>(&value)) return *v;
        static const Object empty;
        return empty;
    }
};

[[nodiscard]] ENGINE_API bool ParseStateValue(std::string_view json, StateValue& value, std::string& error);
[[nodiscard]] ENGINE_API bool SerializeStateValue(const StateValue& value, std::string& json, std::string& error);
} // namespace engine
