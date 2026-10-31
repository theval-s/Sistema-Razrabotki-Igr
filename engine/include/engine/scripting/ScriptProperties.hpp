#pragma once
#include <engine/scene/StateValue.hpp>
#include <string_view>

namespace engine {
/// Flat string/boolean/finite-number values; no Lua references in authored data.
using ScriptPropertyMap = StateValue::Object;
ENGINE_API bool ValidateScriptProperties(const ScriptPropertyMap& values, std::string& error);
ENGINE_API bool CompatibleScriptProperty(const StateValue& a, const StateValue& b);
/// Reads a top-level literal Properties declaration, without executing the source.
ENGINE_API bool ReadScriptProperties(std::string_view source, ScriptPropertyMap& defaults, std::string& error);
}
