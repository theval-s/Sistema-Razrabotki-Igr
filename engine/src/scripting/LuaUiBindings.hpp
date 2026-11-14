#pragma once
#include <engine/scripting/LuaStateValue.hpp>
#include <engine/ui/UiModel.hpp>

namespace engine::lua_ui_detail {
inline bool ReadSchema(const StateValue& value, UiValueSchema& schema, std::string& error, unsigned depth = 0) {
    if (depth > 64) { error = "UI schema exceeds maximum depth (64)"; return false; }
    if (const auto* scalar = std::get_if<std::string>(&value.value)) {
        if (*scalar == "boolean") schema.kind = UiValueKind::Boolean;
        else if (*scalar == "number") schema.kind = UiValueKind::Number;
        else if (*scalar == "string") schema.kind = UiValueKind::String;
        else { error = "UI schema scalar must be boolean, number or string"; return false; }
        return true;
    }
    const auto* object = std::get_if<StateValue::Object>(&value.value);
    if (!object || object->size() != 2) {
        error = "UI schema requires exactly type and fields (object) or element (array)"; return false;
    }
    if (value.At("type").String() == "object" && value.HasValue("fields")) {
        const auto* fields = std::get_if<StateValue::Object>(&value.At("fields").value);
        if (!fields) { error = "UI object schema fields must be an object"; return false; }
        schema.kind = UiValueKind::Object;
        for (const auto& [key, child] : *fields)
            if (!ReadSchema(child, schema.fields[key], error, depth + 1)) return false;
        return true;
    }
    if (value.At("type").String() == "array" && value.HasValue("element")) {
        schema.kind = UiValueKind::Array;
        schema.elementType.resize(1);
        return ReadSchema(value.At("element"), schema.elementType.front(), error, depth + 1);
    }
    error = "UI schema requires type=object/fields or type=array/element"; return false;
}
inline bool Normalize(StateValue& value, const UiValueSchema* schema, std::string& error, unsigned depth = 0) {
    if (depth > 64) { error = "UI value exceeds maximum depth (64)"; return false; }
    if (std::holds_alternative<std::nullptr_t>(value.value)) { error = "UI values cannot contain nil/null"; return false; }
    if (const auto* integer = std::get_if<std::int64_t>(&value.value)) {
        const double number = static_cast<double>(*integer);
        if (number < -9223372036854775808.0 || number >= 9223372036854775808.0 ||
            static_cast<std::int64_t>(number) != *integer) {
            error = "UI integer cannot round-trip exactly through double"; return false;
        }
        value.value = number;
    }
    if (schema && schema->kind == UiValueKind::Array) {
        if (const auto* object = std::get_if<StateValue::Object>(&value.value); object && object->empty())
            value.value = StateValue::Array{};
    }
    if (auto* object = std::get_if<StateValue::Object>(&value.value)) {
        for (auto& [key, child] : *object) {
            const UiValueSchema* field = nullptr;
            if (schema) {
                const auto found = schema->fields.find(key);
                if (found != schema->fields.end()) field = &found->second;
            }
            if (!Normalize(child, field, error, depth + 1)) return false;
        }
    } else if (auto* array = std::get_if<StateValue::Array>(&value.value)) {
        const auto* element = schema && schema->kind == UiValueKind::Array && schema->elementType.size() == 1
            ? &schema->elementType.front() : nullptr;
        for (auto& child : *array) if (!Normalize(child, element, error, depth + 1)) return false;
    }
    return true;
}
inline bool ReadValue(const sol::object& object, const UiValueSchema* schema, StateValue& value, std::string& error) {
    // Native CreateModel validates the full schema/value; SetModelValue validates
    // against the frozen subtree. Re-validating a subtree as a root here wrongly
    // applies Rml top-level variable-name restrictions to nested object members
    // (for example an item's "size" field), although creation accepted that schema.
    return LuaToStateValue(object, value, error) && Normalize(value, schema, error);
}
} // namespace engine::lua_ui_detail
