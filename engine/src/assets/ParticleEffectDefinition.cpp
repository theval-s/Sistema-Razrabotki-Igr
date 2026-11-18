#include <engine/assets/ParticleEffectDefinition.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace engine {
namespace {
using Json = nlohmann::json;
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument(message);
}
void Fields(const Json& object, std::initializer_list<std::string_view> fields) {
    Require(object.is_object(), "expected particle effect object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const auto field : fields) known |= it.key() == field;
        Require(known, "unknown particle effect field: " + it.key());
    }
}
float Number(const Json& j) {
    Require(j.is_number(), "expected finite particle number");
    const auto value = j.get<float>();
    Require(std::isfinite(value), "non-finite particle number");
    return value;
}
std::uint32_t Unsigned(const Json& j) {
    Require(j.is_number_integer() && j >= 0 && j <= std::numeric_limits<std::uint32_t>::max(),
        "expected unsigned 32-bit particle integer");
    return j.get<std::uint32_t>();
}
DirectX::SimpleMath::Vector3 Vector(const Json& j) {
    Require(j.is_array() && j.size() == 3, "expected three particle vector components");
    return {Number(j[0]), Number(j[1]), Number(j[2])};
}
Json Encode(const DirectX::SimpleMath::Vector3& v) { return Json::array({v.x, v.y, v.z}); }
void Validate(const ParticleEffectSettings& s) {
    const auto nonnegative = [](float v) { return std::isfinite(v) && v >= 0; };
    Require(s.appearance != 1, "particle appearance 1 is reserved");
    Require(s.maxAlive >= 1, "particle maxAlive must be at least 1");
    Require(std::isfinite(s.lifetime) && s.lifetime > 0, "particle lifetime must be positive");
    Require(nonnegative(s.lifetimeVariation) && s.lifetimeVariation < 1 &&
        s.lifetime * (1 - s.lifetimeVariation) > 0 &&
        std::isfinite(s.lifetime * (1 + s.lifetimeVariation)), "invalid particle lifetime variation");
    Require(nonnegative(s.speedVariation) && s.speedVariation <= 1 &&
        std::isfinite(s.speed * (1 + s.speedVariation)), "invalid particle speed variation");
    for (const float v : {s.rate, s.speed, s.radius, s.spread, s.drag, s.sizeStart, s.sizeEnd,
        s.softDistance, s.shadowDensity, s.colorStart.x, s.colorStart.y, s.colorStart.z,
        s.colorEnd.x, s.colorEnd.y, s.colorEnd.z})
        Require(nonnegative(v), "particle settings require finite nonnegative values");
    for (const float v : {s.opacityStart, s.opacityEnd, s.fadeIn})
        Require(nonnegative(v) && v <= 1, "particle opacity/fadeIn must be in [0,1]");
    Require(std::isfinite(s.acceleration.x) && std::isfinite(s.acceleration.y) &&
        std::isfinite(s.acceleration.z), "particle acceleration must be finite");
    Require(s.blend == ParticleBlendMode::Alpha || s.blend == ParticleBlendMode::Additive,
        "invalid particle blend mode");
}
ParticleEffectDefinition Decode(std::string_view text) {
    std::vector<std::set<std::string>> keys;
    const auto callback = [&keys](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 16, "particle effect JSON nesting is too deep");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Require(keys.back().insert(value.get<std::string>()).second, "duplicate particle effect field");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    const auto root = Json::parse(text.begin(), text.end(), callback);
    Fields(root, {"version", "texture", "settings"});
    Require(root.at("version").is_number_integer() && root.at("version") == 1, "expected particle effect version 1");
    ParticleEffectDefinition result;
    if (!root.at("texture").is_null()) {
        std::string error;
        Require(ParseAssetReference(root.at("texture").dump(), result.texture, error), error);
    }
    const auto& j = root.at("settings");
    Fields(j, {"appearance", "maxAlive", "rate", "lifetime", "speed", "radius", "spread",
        "lifetimeVariation", "speedVariation", "acceleration", "drag", "sizeStart", "sizeEnd",
        "colorStart", "colorEnd", "opacityStart", "opacityEnd", "fadeIn", "softDistance",
        "shadowDensity", "blend", "castShadows", "randomRotation"});
    auto& s = result.settings;
#define FLOAT_FIELD(field) if (j.contains(#field)) s.field = Number(j.at(#field))
    FLOAT_FIELD(rate); FLOAT_FIELD(lifetime); FLOAT_FIELD(speed); FLOAT_FIELD(radius); FLOAT_FIELD(spread);
    FLOAT_FIELD(lifetimeVariation); FLOAT_FIELD(speedVariation); FLOAT_FIELD(drag);
    FLOAT_FIELD(sizeStart); FLOAT_FIELD(sizeEnd); FLOAT_FIELD(opacityStart); FLOAT_FIELD(opacityEnd);
    FLOAT_FIELD(fadeIn); FLOAT_FIELD(softDistance); FLOAT_FIELD(shadowDensity);
#undef FLOAT_FIELD
    if (j.contains("appearance")) s.appearance = Unsigned(j.at("appearance"));
    if (j.contains("maxAlive")) s.maxAlive = Unsigned(j.at("maxAlive"));
    if (j.contains("acceleration")) s.acceleration = Vector(j.at("acceleration"));
    if (j.contains("colorStart")) s.colorStart = Vector(j.at("colorStart"));
    if (j.contains("colorEnd")) s.colorEnd = Vector(j.at("colorEnd"));
    if (j.contains("castShadows")) s.castShadows = j.at("castShadows").get<bool>();
    if (j.contains("randomRotation")) s.randomRotation = j.at("randomRotation").get<bool>();
    if (j.contains("blend")) {
        const auto blend = j.at("blend").get<std::string>();
        Require(blend == "alpha" || blend == "additive", "invalid particle blend string");
        s.blend = blend == "alpha" ? ParticleBlendMode::Alpha : ParticleBlendMode::Additive;
    }
    Validate(s);
    return result;
}
} // namespace

bool ValidateParticleEffectSettings(const ParticleEffectSettings& value, std::string& error) {
    try { Validate(value); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool ParseParticleEffectDefinition(std::string_view text, ParticleEffectDefinition& value, std::string& error) {
    try { auto result = Decode(text); value = std::move(result); error.clear(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
bool SerializeParticleEffectDefinition(const ParticleEffectDefinition& value, std::string& text, std::string& error) {
    try {
        Require(value.version == 1, "expected particle effect version 1");
        Validate(value.settings);
        Json texture = nullptr;
        if (value.texture.id.IsValid() || !value.texture.path.empty()) {
            std::string reference;
            Require(SerializeAssetReference(value.texture, reference, error), error);
            texture = Json::parse(reference);
        }
        const auto& s = value.settings;
        Json settings = {{"appearance", s.appearance}, {"maxAlive", s.maxAlive},
            {"rate", s.rate}, {"lifetime", s.lifetime}, {"speed", s.speed}, {"radius", s.radius},
            {"spread", s.spread}, {"lifetimeVariation", s.lifetimeVariation}, {"speedVariation", s.speedVariation},
            {"acceleration", Encode(s.acceleration)}, {"drag", s.drag}, {"sizeStart", s.sizeStart},
            {"sizeEnd", s.sizeEnd}, {"colorStart", Encode(s.colorStart)}, {"colorEnd", Encode(s.colorEnd)},
            {"opacityStart", s.opacityStart}, {"opacityEnd", s.opacityEnd}, {"fadeIn", s.fadeIn},
            {"softDistance", s.softDistance}, {"shadowDensity", s.shadowDensity},
            {"blend", s.blend == ParticleBlendMode::Alpha ? "alpha" : "additive"},
            {"castShadows", s.castShadows}, {"randomRotation", s.randomRotation}};
        auto encoded = Json{{"version", 1}, {"texture", texture}, {"settings", settings}}.dump(2);
        Decode(encoded);
        text = std::move(encoded); error.clear(); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace engine
