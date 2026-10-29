#include <engine/scene/StateValue.hpp>
#include <cmath>
#include <set>
#include <nlohmann/json.hpp>

namespace engine {
namespace {
using Json = nlohmann::json;
StateValue Decode(const Json& j) {
    if (j.is_null()) return nullptr;
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_unsigned()) return j.get<std::uint64_t>();
    if (j.is_number_integer()) return j.get<std::int64_t>();
    if (j.is_number_float()) {
        const double n = j.get<double>();
        if (!std::isfinite(n)) throw std::invalid_argument("non-finite state number");
        return n;
    }
    if (j.is_string()) return j.get<std::string>();
    if (j.is_array()) {
        StateValue::Array values;
        for (const auto& item : j) values.push_back(Decode(item));
        return values;
    }
    if (j.is_object()) {
        StateValue::Object fields;
        for (auto it = j.begin(); it != j.end(); ++it) fields.emplace(it.key(), Decode(it.value()));
        return fields;
    }
    throw std::invalid_argument("unsupported JSON state value");
}
Json Encode(const StateValue& v) {
    return std::visit([](const auto& data) -> Json {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, StateValue::Object>) {
            Json result = Json::object();
            for (const auto& [key, item] : data) result[key] = Encode(item);
            return result;
        } else if constexpr (std::is_same_v<T, StateValue::Array>) {
            Json result = Json::array();
            for (const auto& item : data) result.push_back(Encode(item));
            return result;
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(data)) throw std::invalid_argument("non-finite state number");
            return data;
        } else return data;
    }, v.value);
}
}
bool ParseStateValue(std::string_view json, StateValue& value, std::string& error) {
    try {
        std::vector<std::set<std::string>> keys;
        auto callback = [&keys](int depth, Json::parse_event_t event, Json& current) {
            if (depth > 32) throw std::invalid_argument("state JSON nesting is too deep");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key && !keys.back().insert(current.get<std::string>()).second)
                throw std::invalid_argument("duplicate state field: " + current.get<std::string>());
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        };
        auto parsed = Decode(Json::parse(json.begin(), json.end(), callback));
        value = std::move(parsed); error.clear(); return true;
    }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool SerializeStateValue(const StateValue& value, std::string& json, std::string& error) {
    try { auto text = Encode(value).dump(); json = std::move(text); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace engine
