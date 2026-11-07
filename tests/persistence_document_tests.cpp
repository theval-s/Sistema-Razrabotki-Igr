#include <engine/scene/GameDocument.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <limits>

namespace {
using Json = nlohmann::json;
using namespace engine;

GameDocument Document() {
    GameDocument doc;
    doc.scene = {NewUuid(), "game/scenes/persistence.scene"};
    GameEntityState entity;
    entity.id = NewUuid();
    entity.script = GameScriptState{NewUuid(), 3, StateValue::Object{
        {"score", 42}, {"active", true}, {"title", "saved"},
        {"items", StateValue::Array{1, "two", StateValue::Object{}}}}};
    doc.entities.push_back(std::move(entity));
    doc.autoloads.emplace("_Session2", GameScriptState{NewUuid(), 1, StateValue::Array{false, 2.5}});
    return doc;
}

Json Encoded(const GameDocument& doc) {
    std::string text, error;
    EXPECT_TRUE(SerializeGameDocument(doc, text, error)) << error;
    return Json::parse(text);
}

void Reject(const Json& invalid) {
    auto result = Document();
    const auto before = Encoded(result);
    std::string error;
    EXPECT_FALSE(ParseGameDocument(invalid.dump(), result, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Encoded(result), before);
}

void RejectSerialization(const GameDocument& invalid) {
    std::string text = "unchanged", error;
    EXPECT_FALSE(SerializeGameDocument(invalid, text, error));
    EXPECT_EQ(text, "unchanged");
    EXPECT_FALSE(error.empty());
}

StateValue Nested(unsigned depth) {
    StateValue value = StateValue::Object{};
    for (unsigned i = 0; i < depth; ++i) value = StateValue::Array{std::move(value)};
    return value;
}
} // namespace

TEST(PersistenceDocument, ScriptOnlyEntityAndAutoloadRoundTripV1) {
    const auto source = Document();
    const auto json = Encoded(source);
    EXPECT_EQ(json.at("version"), 1);
    GameDocument parsed;
    std::string error;
    ASSERT_TRUE(ParseGameDocument(json.dump(), parsed, error)) << error;
    ASSERT_EQ(parsed.entities.size(), 1u);
    ASSERT_TRUE(parsed.entities[0].script);
    EXPECT_EQ(parsed.entities[0].id, source.entities[0].id);
    EXPECT_EQ(parsed.entities[0].script->asset, source.entities[0].script->asset);
    EXPECT_EQ(parsed.entities[0].script->version, 3);
    EXPECT_EQ(parsed.entities[0].script->data.At("score").Number(), 42);
    EXPECT_EQ(parsed.autoloads.at("_Session2").asset, source.autoloads.at("_Session2").asset);
    EXPECT_EQ(Encoded(parsed), json);
    EXPECT_TRUE(error.empty());
}

TEST(PersistenceDocument, OldV1WithoutScriptFieldsStillParses) {
    auto source = Document();
    source.entities[0].script.reset();
    source.entities[0].transform = Transform{};
    source.autoloads.clear();
    const auto json = Encoded(source);
    EXPECT_FALSE(json.contains("autoloads"));
    EXPECT_FALSE(json.at("entities")[0].contains("script"));
    auto parsed = Document();
    std::string error;
    ASSERT_TRUE(ParseGameDocument(json.dump(), parsed, error)) << error;
    EXPECT_FALSE(parsed.entities[0].script);
    EXPECT_TRUE(parsed.autoloads.empty());
}

TEST(PersistenceDocument, EmptyTablesAndIntegerRangesRetainTheirJsonShape) {
    auto doc = Document();
    doc.entities[0].script->version = std::numeric_limits<int>::max();
    doc.entities[0].script->data = StateValue::Object{
        {"array", StateValue::Array{}}, {"object", StateValue::Object{}},
        {"signed", std::numeric_limits<std::int64_t>::min()},
        {"unsigned", std::numeric_limits<std::uint64_t>::max()}};
    doc.autoloads.begin()->second.data = StateValue::Array{};
    const auto json = Encoded(doc);
    GameDocument parsed;
    std::string error;
    ASSERT_TRUE(ParseGameDocument(json.dump(), parsed, error)) << error;
    EXPECT_TRUE(std::holds_alternative<StateValue::Array>(parsed.autoloads.begin()->second.data.value));
    EXPECT_TRUE(std::holds_alternative<StateValue::Object>(parsed.entities[0].script->data.At("object").value));
    EXPECT_EQ(parsed.entities[0].script->data.At("unsigned").Unsigned(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(Encoded(parsed), json);
}

TEST(PersistenceDocument, RejectsScriptIdentityAndVersionInBothEnvelopes) {
    const auto valid = Encoded(Document());
    for (const bool autoload : {false, true}) {
        for (const auto& field : {"asset", "version", "data"}) {
            auto json = valid;
            auto& script = autoload ? json["autoloads"]["_Session2"] : json["entities"][0]["script"];
            script.erase(field);
            Reject(json);
        }
        for (const auto& asset : Json::array({"", "invalid", "00000000-0000-0000-0000-000000000000", 12, nullptr})) {
            auto json = valid;
            auto& script = autoload ? json["autoloads"]["_Session2"] : json["entities"][0]["script"];
            script["asset"] = asset;
            Reject(json);
        }
        for (const auto& version : Json::array({0, -1, 1.0, 1.5, true, "1", nullptr,
                                               std::uint64_t{2147483648ULL}, std::numeric_limits<std::uint64_t>::max()})) {
            auto json = valid;
            auto& script = autoload ? json["autoloads"]["_Session2"] : json["entities"][0]["script"];
            script["version"] = version;
            Reject(json);
        }
    }
    auto doc = Document();
    doc.entities[0].script->asset = AssetUuid{};
    RejectSerialization(doc);
    doc = Document();
    doc.autoloads.begin()->second.version = 0;
    RejectSerialization(doc);
}

TEST(PersistenceDocument, RejectsScalarsNullAndNonfiniteScriptDataTransactionally) {
    const auto valid = Encoded(Document());
    for (const bool autoload : {false, true}) {
        for (const auto& data : Json::array({nullptr, true, 12, "scalar", Json::array({nullptr}),
                                           Json{{"nested", Json::array({Json{{"bad", nullptr}}})}}})) {
            auto json = valid;
            auto& script = autoload ? json["autoloads"]["_Session2"] : json["entities"][0]["script"];
            script["data"] = data;
            Reject(json);
        }
    }
    for (const double number : {std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto doc = Document();
        doc.entities[0].script->data["bad"] = StateValue::Array{number};
        RejectSerialization(doc);
    }
    auto doc = Document();
    doc.autoloads.begin()->second.data = StateValue::Object{{"bad", nullptr}};
    RejectSerialization(doc);
}

TEST(PersistenceDocument, DepthLimitMatchesLuaRelativeToDataRoot) {
    auto doc = Document();
    doc.entities[0].script->data = Nested(64);
    doc.autoloads.begin()->second.data = Nested(64);
    const auto valid = Encoded(doc);
    GameDocument parsed;
    std::string error;
    ASSERT_TRUE(ParseGameDocument(valid.dump(), parsed, error)) << error;
    EXPECT_EQ(Encoded(parsed), valid);
    doc.entities[0].script->data = Nested(65);
    RejectSerialization(doc);
    auto invalid = valid;
    invalid["autoloads"]["_Session2"]["data"] = Json::array({valid["autoloads"]["_Session2"]["data"]});
    Reject(invalid);
}

TEST(PersistenceDocument, AutoloadNamesUseAsciiIdentifierRules) {
    const auto valid = Encoded(Document());
    for (const auto& name : {"", "2Session", "has space", "has-dash", "a.b", "caf\xc3\xa9"}) {
        auto json = valid;
        json["autoloads"] = Json::object();
        json["autoloads"][name] = valid["autoloads"]["_Session2"];
        Reject(json);
        auto doc = Document();
        const auto state = doc.autoloads.begin()->second;
        doc.autoloads.clear();
        doc.autoloads.emplace(name, state);
        RejectSerialization(doc);
    }
    auto invalid = valid;
    invalid["autoloads"] = Json::array();
    Reject(invalid);
}

TEST(PersistenceDocument, RejectsUnknownAndDuplicateFieldsAndInvalidEntityIdentity) {
    const auto valid = Encoded(Document());
    auto invalid = valid;
    invalid["entities"][0]["script"]["unknown"] = true;
    Reject(invalid);
    invalid = valid;
    invalid["entities"].push_back(invalid["entities"][0]);
    Reject(invalid);
    invalid = valid;
    invalid["entities"][0]["id"] = "bad";
    Reject(invalid);
    std::string text = valid.dump();
    const auto offset = text.find("\"asset\":");
    ASSERT_NE(offset, std::string::npos);
    text.insert(offset, "\"asset\":\"duplicate\",");
    auto parsed = Document();
    const auto before = Encoded(parsed);
    std::string error;
    EXPECT_FALSE(ParseGameDocument(text, parsed, error));
    EXPECT_EQ(Encoded(parsed), before);
}

TEST(PersistenceDocument, SceneScriptPolicyDefaultsFalseAndRoundTrips) {
    EXPECT_FALSE(GamePersistence{}.scriptState);
    SceneDocument scene;
    SceneEntityDefinition entity;
    entity.id = NewUuid();
    entity.name = "Persistent";
    entity.persistence = GamePersistence{true, false, true};
    scene.entities.push_back(entity);
    std::string text, error;
    ASSERT_TRUE(SerializeSceneDocument(scene, text, error)) << error;
    SceneDocument parsed;
    ASSERT_TRUE(ParseSceneDocument(text, parsed, error)) << error;
    ASSERT_TRUE(parsed.entities[0].persistence);
    EXPECT_TRUE(parsed.entities[0].persistence->scriptState);
    auto json = Json::parse(text);
    json["entities"][0]["components"]["GamePersistence"].erase("scriptState");
    ASSERT_TRUE(ParseSceneDocument(json.dump(), parsed, error)) << error;
    EXPECT_FALSE(parsed.entities[0].persistence->scriptState);
    for (const auto& invalid : Json::array({nullptr, 1, "true"})) {
        json["entities"][0]["components"]["GamePersistence"]["scriptState"] = invalid;
        EXPECT_FALSE(ParseSceneDocument(json.dump(), parsed, error));
        EXPECT_FALSE(parsed.entities[0].persistence->scriptState);
    }
}
