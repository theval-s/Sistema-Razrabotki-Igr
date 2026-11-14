#pragma once

#include <map>
#include <string>
#include <vector>
#include <engine/engine_export.hpp>
#include <engine/scene/StateValue.hpp>

namespace engine {
enum class UiValueKind { Boolean, Number, String, Object, Array };

// Recursive value schema, copied and frozen at registration. Object uses fields only;
// Array uses exactly one entry in elementType; scalars use neither container.
// This representation supports an explicitly typed empty array without nullable shared trees.
struct UiValueSchema {
    UiValueKind kind = UiValueKind::Object;
    std::map<std::string, UiValueSchema> fields;
    std::vector<UiValueSchema> elementType;
};

struct UiModelDesc {
    std::string name;
    UiValueSchema schema; // root must be Object; exact field set, no optional/null values
    StateValue initialValues;
};

// Inference rejects empty arrays (including nested ones); use an explicit schema for those.
// Empty objects are legal. Numbers normalize to finite double with exact integer conversion.
// Schema/value validation is transactional; outputs remain unchanged on failure.
[[nodiscard]] ENGINE_API bool InferUiSchema(const StateValue& value, UiValueSchema& schema,
                                           std::string& error);
[[nodiscard]] ENGINE_API bool ValidateUiValue(const UiValueSchema& schema, const StateValue& value,
                                             std::string& error);
} // namespace engine
