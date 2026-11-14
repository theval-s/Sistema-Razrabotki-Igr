#include "UiModelStorage.hpp"
#include <engine/core/Utf.hpp>
#include <cmath>
#include <limits>
#include <algorithm>
#include <bit>

namespace engine::ui_detail {
bool Identifier(std::string_view s) {
    auto alpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !s.empty() && s.front() != '_' && alpha(s.front()) && std::all_of(s.begin(), s.end(), [&](unsigned char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}
namespace {
bool VariableIdentifier(std::string_view s) {
    if (!Identifier(s)) return false;
    std::string lower(s); for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return lower != "it" && lower != "it_index" && lower != "ev" && lower != "true" && lower != "false" && lower != "size" && lower != "literal";
}
}
bool Utf8(std::string_view s) {
    return utf::IsValidUtf8(s);
}
namespace {
bool Number(const StateValue& v, double& d) {
    auto exact = [](std::uint64_t n) { auto bits = std::bit_width(n); return bits <= 53 || (n & ((std::uint64_t{1} << (bits - 53)) - 1)) == 0; };
    if (auto p = std::get_if<double>(&v.value)) { d = *p; return std::isfinite(d); }
    if (auto p = std::get_if<std::int64_t>(&v.value)) { d = static_cast<double>(*p); return exact(*p < 0 ? std::uint64_t(-(*p + 1)) + 1 : std::uint64_t(*p)); }
    if (auto p = std::get_if<std::uint64_t>(&v.value)) { d = static_cast<double>(*p); return exact(*p); }
    return false;
}
bool Shape(const UiValueSchema& s, unsigned depth, std::string& e) {
    if (depth > 64) { e = "UI schema exceeds nesting limit (64)"; return false; }
    if (s.kind == UiValueKind::Object) {
        if (!s.elementType.empty()) { e = "Object schema cannot have elementType"; return false; }
        for (auto& [k, child] : s.fields) if (!(depth == 0 ? VariableIdentifier(k) : Identifier(k)) || !Shape(child, depth + 1, e)) { if (e.empty()) e = "Invalid UI member identifier: " + k; return false; }
    } else if (s.kind == UiValueKind::Array) {
        if (!s.fields.empty() || s.elementType.size() != 1) { e = "Array schema requires exactly one element schema and no fields"; return false; }
        return Shape(s.elementType.front(), depth + 1, e);
    } else if (s.kind != UiValueKind::Boolean && s.kind != UiValueKind::Number && s.kind != UiValueKind::String) { e = "Unknown UI schema kind"; return false; }
    else if (!s.fields.empty() || !s.elementType.empty()) { e = "Scalar schema cannot contain fields or elementType"; return false; }
    return true;
}
bool Convert(const UiValueSchema& s, const StateValue& v, StateValue& out, unsigned depth, std::string& e) {
    if (depth > 64) { e = "UI value exceeds nesting limit (64)"; return false; }
    switch (s.kind) {
    case UiValueKind::Boolean: if (auto p = std::get_if<bool>(&v.value)) { out = *p; return true; } break;
    case UiValueKind::Number: { double d; if (Number(v, d)) { out = d; return true; } break; }
    case UiValueKind::String: if (auto p = std::get_if<std::string>(&v.value); p && Utf8(*p)) { out = *p; return true; } break;
    case UiValueKind::Object:
        if (auto p = std::get_if<StateValue::Object>(&v.value); p && p->size() == s.fields.size()) {
            StateValue::Object result;
            for (auto& [k, schema] : s.fields) { auto it = p->find(k); if (it == p->end() || !Convert(schema, it->second, result[k], depth + 1, e)) { if (e.empty()) e = "Missing UI object field: " + k; return false; } }
            out = std::move(result); return true;
        } break;
    case UiValueKind::Array:
        if (auto p = std::get_if<StateValue::Array>(&v.value); p && p->size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            StateValue::Array result(p->size());
            for (std::size_t i = 0; i < p->size(); ++i) if (!Convert(s.elementType.front(), (*p)[i], result[i], depth + 1, e)) return false;
            out = std::move(result); return true;
        } break;
    }
    e = "UI value does not match its frozen schema (numbers must be finite and exact, strings UTF-8)"; return false;
}
bool Infer(const StateValue& v, UiValueSchema& s, unsigned depth, std::string& e) {
    if (depth > 64) { e = "UI value exceeds nesting limit (64)"; return false; }
    double d;
    if (std::holds_alternative<bool>(v.value)) s.kind = UiValueKind::Boolean;
    else if (Number(v, d)) s.kind = UiValueKind::Number;
    else if (auto p = std::get_if<std::string>(&v.value); p && Utf8(*p)) s.kind = UiValueKind::String;
    else if (auto p = std::get_if<StateValue::Object>(&v.value)) {
        s.kind = UiValueKind::Object;
        for (auto& [k, child] : *p) if (!(depth == 0 ? VariableIdentifier(k) : Identifier(k)) || !Infer(child, s.fields[k], depth + 1, e)) { if (e.empty()) e = "Invalid UI member identifier: " + k; return false; }
    } else if (auto p = std::get_if<StateValue::Array>(&v.value); p && !p->empty()) {
        s.kind = UiValueKind::Array; s.elementType.resize(1);
        if (!Infer(p->front(), s.elementType.front(), depth + 1, e)) return false;
        StateValue copy;
        for (auto& child : *p) if (!Convert(s.elementType.front(), child, copy, depth + 1, e)) return false;
    } else { e = "Cannot infer UI schema: null, invalid scalar, or ambiguous empty array"; return false; }
    return true;
}
}
struct ModelStorage::Node { std::weak_ptr<ModelStorage> storage; UiModelPath path; const UiValueSchema* schema; };
struct ModelStorage::Definition final : Rml::VariableDefinition {
    explicit Definition(const UiValueSchema& schema) : VariableDefinition(schema.kind == UiValueKind::Array ? Rml::DataVariableType::Array : schema.kind == UiValueKind::Object ? Rml::DataVariableType::Struct : Rml::DataVariableType::Scalar), schema(schema) {}
    const UiValueSchema& schema;
    bool Get(void* p, Rml::Variant& v) override {
        auto& n = *static_cast<Node*>(p); auto s = n.storage.lock(); StateValue copy; std::string error;
        if (!s || !s->Read(n.path, copy, error)) return false;
        if (auto x = std::get_if<bool>(&copy.value)) v = *x;
        else if (auto x = std::get_if<double>(&copy.value)) v = *x;
        else if (auto x = std::get_if<std::string>(&copy.value)) v = *x;
        else return false;
        return true;
    }
    bool Set(void* p, const Rml::Variant& v) override {
        auto& n = *static_cast<Node*>(p); auto s = n.storage.lock(); StateValue copy; std::string error;
        if (!s) return false;
        if (schema.kind == UiValueKind::Boolean) { bool x; if (!v.GetInto(x)) return false; copy = x; }
        else if (schema.kind == UiValueKind::Number) { double x; if (!v.GetInto(x)) return false; copy = x; }
        else if (schema.kind == UiValueKind::String) { Rml::String x; if (!v.GetInto(x)) return false; copy = std::move(x); }
        else return false;
        return s->Write(n.path, copy, error);
    }
    int Size(void* p) override { auto& n = *static_cast<Node*>(p); auto s = n.storage.lock(); std::size_t size = 0; std::string e; return s && s->ArraySize(n.path, size, e) ? static_cast<int>(size) : 0; }
    Rml::DataVariable Child(void* p, const Rml::DataAddressEntry& a) override {
        auto& n = *static_cast<Node*>(p); auto s = n.storage.lock(); if (!s || !s->Resolve(n.path)) return {};
        auto path = n.path; const UiValueSchema* child = nullptr;
        if (schema.kind == UiValueKind::Object) { auto it = schema.fields.find(a.name); if (it == schema.fields.end()) return {}; path.emplace_back(a.name); child = &it->second; }
        else if (schema.kind == UiValueKind::Array) { if (a.name == "size") return Rml::MakeLiteralIntVariable(Size(p)); if (a.index < 0 || a.index >= Size(p)) return {}; path.emplace_back(static_cast<std::uint32_t>(a.index)); child = &schema.elementType.front(); }
        else return {};
        auto node = s->Path(path, *child); return {s->definitions_.at(child).get(), node};
    }
    Rml::StringList ReflectMemberNames() override { Rml::StringList result; for (auto& [k, v] : schema.fields) result.push_back(k); return result; }
};
ModelStorage::ModelStorage(UiValueSchema schema, StateValue values) : schema_(std::move(schema)) { std::string e; Convert(schema_, values, values_, 0, e); }
ModelStorage::~ModelStorage() = default;
const StateValue* ModelStorage::Resolve(std::span<const UiModelPathPart> path, const UiValueSchema** schema) const {
    auto* v = &values_; auto* s = &schema_;
    for (auto& part : path) {
        if (auto key = std::get_if<std::string>(&part)) { if (s->kind != UiValueKind::Object) return nullptr; auto it = s->fields.find(*key); v = v->Find(*key); if (it == s->fields.end() || !v) return nullptr; s = &it->second; }
        else { auto index = std::get<std::uint32_t>(part); if (s->kind != UiValueKind::Array || index >= v->Items().size()) return nullptr; v = &v->Items()[index]; s = &s->elementType.front(); }
    }
    if (schema) *schema = s; return v;
}
bool ModelStorage::Read(std::span<const UiModelPathPart> path, StateValue& copy, std::string& e) const { if (auto v = Resolve(path)) { copy = *v; return true; } e = "UI model path no longer exists"; return false; }
bool ModelStorage::Write(std::span<const UiModelPathPart> path, const StateValue& value, std::string& e) {
    const UiValueSchema* schema; auto old = Resolve(path, &schema); if (!old) { e = "UI model path no longer exists"; return false; }
    StateValue copy; if (!Convert(*schema, value, copy, 0, e)) return false;
    *const_cast<StateValue*>(old) = std::move(copy);
    if (path.empty()) for (auto& [k, v] : schema_.fields) dirty_.insert(k);
    else if (auto key = std::get_if<std::string>(&path.front())) dirty_.insert(*key);
    return true;
}
bool ModelStorage::ArraySize(std::span<const UiModelPathPart> path, std::size_t& size, std::string& e) const { auto v = Resolve(path); if (!v || !std::holds_alternative<StateValue::Array>(v->value)) { e = "UI model path is not an array"; return false; } size = v->Items().size(); return true; }
std::vector<std::string> ModelStorage::TakeDirtyVariables() { std::vector<std::string> result(dirty_.begin(), dirty_.end()); dirty_.clear(); return result; }
ModelStorage::Node* ModelStorage::Path(const UiModelPath& path, const UiValueSchema& schema) { auto& n = nodes_[path]; if (!n) n = std::make_unique<Node>(Node{weak_from_this(), path, &schema}); return n.get(); }
bool ModelStorage::Bind(Rml::DataModelConstructor& constructor, std::string& e) {
    auto define = [&](auto&& self, const UiValueSchema& schema) -> void { definitions_[&schema] = std::make_unique<Definition>(schema); for (auto& [k, child] : schema.fields) self(self, child); for (auto& child : schema.elementType) self(self, child); };
    define(define, schema_);
    for (auto& [k, schema] : schema_.fields) if (!constructor.BindCustomDataVariable(k, {definitions_.at(&schema).get(), Path({k}, schema)})) { e = "Rml refused UI variable: " + k; return false; }
    return true;
}
}
namespace engine {
bool InferUiSchema(const StateValue& value, UiValueSchema& schema, std::string& error) { UiValueSchema result; if (!ui_detail::Infer(value, result, 0, error)) return false; schema = std::move(result); return true; }
bool ValidateUiValue(const UiValueSchema& schema, const StateValue& value, std::string& error) { StateValue copy; return ui_detail::Shape(schema, 0, error) && ui_detail::Convert(schema, value, copy, 0, error); }
}
