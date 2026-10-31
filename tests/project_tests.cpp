#include <engine/project/Project.hpp>
#include <engine/core/FileSystem.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-project-" + engine::NewUuid());
    std::filesystem::path file = root / "example.sriproject";
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Write(std::string_view text) const {
        std::string error;
        ASSERT_TRUE(engine::WriteFileAtomic(file, text, error)) << error;
    }
};

engine::AutoloadDefinition Definition(std::string name = "Hello", bool enabled = true) {
    return {std::move(name), {engine::NewUuid(), "game/scripts/missing.lua"}, enabled};
}

TEST(ProjectAutoloads, OlderProjectWithoutListLoadsAndPreservesKnownSettings) {
    Fixture fixture;
    fixture.Write(R"({"version":1,"name":"Old Project","startupScene":"game/scenes/start.scene.json"})");
    std::string error = "previous error";
    auto project = engine::Project::Load(fixture.file, error);
    ASSERT_TRUE(project.has_value()) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(project->autoloads.empty());
    EXPECT_EQ(project->name, "Old Project");
    EXPECT_EQ(project->startupScene, "game/scenes/start.scene.json");
    project->autoloads = {Definition()};
    ASSERT_TRUE(project->Save(error)) << error;
    project = engine::Project::Load(fixture.file, error);
    ASSERT_TRUE(project.has_value()) << error;
    EXPECT_EQ(project->name, "Old Project");
    EXPECT_EQ(project->startupScene, "game/scenes/start.scene.json");
}

TEST(ProjectAutoloads, RoundTripsOrderEnabledAndReferencesWithoutExistingAssets) {
    Fixture fixture;
    engine::Project project;
    project.file = fixture.file;
    project.name = "Autoload Test";
    project.startupScene = "game/scenes/start.scene.json";
    project.autoloads = {Definition("GameState"), Definition("_Hello2", false)};
    std::string error;
    ASSERT_TRUE(project.Save(error)) << error;
    const auto loaded = engine::Project::Load(fixture.file, error);
    ASSERT_TRUE(loaded.has_value()) << error;
    ASSERT_EQ(loaded->autoloads.size(), 2u);
    for (std::size_t i = 0; i < project.autoloads.size(); ++i) {
        EXPECT_EQ(loaded->autoloads[i].name, project.autoloads[i].name);
        EXPECT_EQ(loaded->autoloads[i].enabled, project.autoloads[i].enabled);
        EXPECT_EQ(loaded->autoloads[i].script.id, project.autoloads[i].script.id);
        EXPECT_EQ(loaded->autoloads[i].script.path, project.autoloads[i].script.path);
    }
    EXPECT_FALSE(std::filesystem::exists(project.ContentDir()));
}

TEST(ProjectAutoloads, RejectsMalformedJsonFieldsAndReferences) {
    Fixture fixture;
    const std::string reference = R"({"id":"a70a453f-f40b-433c-b1ae-bf3c62b3d746","path":"game/hello.lua"})";
    const std::string prefix = R"({"version":1,"autoloads":)";
    const std::string entry = R"({"name":"Hello","enabled":true,"script":)" + reference + "}";
    for (const auto& entries : {
             std::string("null"), std::string("{}"), std::string("[null]"), std::string("[{}]"),
             "[" + entry + "," + entry + "]",
             R"([{"name":"Hello","enabled":1,"script":)" + reference + "}]",
             R"([{"name":42,"enabled":true,"script":)" + reference + "}]",
             std::string(R"([{"name":"Hello","enabled":true,"script":{"id":"broken","path":"game/a.lua"}}])"),
             std::string(R"([{"name":"Hello","enabled":true,"script":{"id":"a70a453f-f40b-433c-b1ae-bf3c62b3d746","path":"../a.lua"}}])")}) {
        SCOPED_TRACE(entries);
        fixture.Write(prefix + entries + "}");
        std::string error;
        EXPECT_FALSE(engine::Project::Load(fixture.file, error).has_value());
        EXPECT_FALSE(error.empty());
    }
}

TEST(ProjectAutoloads, SharedValidatorRejectsInvalidNamesAndDuplicateIdsIncludingDisabledEntries) {
    std::string error;
    for (const auto name : {"", "2Hello", "Hello world", "Hello.World", "Hello-World", "\xc3\xa9"}) {
        const std::vector<engine::AutoloadDefinition> definitions = {Definition(name)};
        EXPECT_FALSE(engine::ValidateAutoloadDefinitions(definitions, error)) << name;
    }
    std::vector<engine::AutoloadDefinition> definitions = {Definition("Hello"), Definition("Hello", false)};
    EXPECT_FALSE(engine::ValidateAutoloadDefinitions(definitions, error));
    definitions[1].name = "Other";
    definitions[1].script.id = definitions[0].script.id;
    EXPECT_FALSE(engine::ValidateAutoloadDefinitions(definitions, error));
    definitions[1].script.id = engine::NewUuid();
    EXPECT_TRUE(engine::ValidateAutoloadDefinitions(definitions, error)) << error;
}

TEST(ProjectAutoloads, InvalidSavePreservesExistingFile) {
    Fixture fixture;
    fixture.Write(R"({"version":1,"name":"Keep me"})");
    std::string before, after, error;
    ASSERT_TRUE(engine::ReadTextFile(fixture.file, before, error));
    auto project = engine::Project::Load(fixture.file, error);
    ASSERT_TRUE(project.has_value()) << error;
    project->autoloads = {Definition("Not an identifier")};
    EXPECT_FALSE(project->Save(error));
    ASSERT_TRUE(engine::ReadTextFile(fixture.file, after, error));
    EXPECT_EQ(after, before);
}

}  // namespace
