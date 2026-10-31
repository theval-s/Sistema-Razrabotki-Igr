#include "autoload_dialog.hpp"
#include <editor/scene_document.hpp>
#include <editor/selection.hpp>
#include <editor/lua_definitions.hpp>

#include <QApplication>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

engine::Project SampleProject() {
    engine::Project project;
    project.autoloads = {
        {"First", {engine::NewUuid(), "game/first.lua"}, true},
        {"Second", {engine::NewUuid(), "game/second.lua"}, false},
    };
    return project;
}

TEST(AutoloadDialog, CancelPreservesProjectAndReorderingPreservesEditsAndReferences) {
    engine::Engine engine;
    const auto project = SampleProject();
    editor::AutoloadDialog dialog(engine, project);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("AutoloadTable"));
    auto* down = dialog.findChild<QPushButton*>(QStringLiteral("MoveAutoloadDown"));
    ASSERT_NE(table, nullptr);
    ASSERT_NE(down, nullptr);
    ASSERT_EQ(table->rowCount(), 2);
    table->item(0, 1)->setText(QStringLiteral("Renamed"));
    table->item(0, 0)->setCheckState(Qt::Unchecked);
    table->selectRow(0);
    down->click();
    const auto edited = dialog.definitions();
    ASSERT_EQ(edited.size(), 2u);
    EXPECT_EQ(edited[0].name, "Second");
    EXPECT_EQ(edited[1].name, "Renamed");
    EXPECT_FALSE(edited[1].enabled);
    EXPECT_EQ(edited[1].script.id, project.autoloads[0].script.id);
    dialog.reject();
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
    EXPECT_EQ(project.autoloads[0].name, "First");
    EXPECT_TRUE(project.autoloads[0].enabled);
}

TEST(AutoloadDialog, RemoveAndSaveOnlyChangeTheDialogCopy) {
    engine::Engine engine;
    const auto project = SampleProject();
    editor::AutoloadDialog dialog(engine, project);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("AutoloadTable"));
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("RemoveAutoload"));
    ASSERT_NE(table, nullptr);
    ASSERT_NE(remove, nullptr);
    table->selectRow(0);
    remove->click();
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    const auto edited = dialog.definitions();
    ASSERT_EQ(edited.size(), 1u);
    EXPECT_EQ(edited[0].name, "Second");
    EXPECT_EQ(edited[0].script.id, project.autoloads[1].script.id);
    EXPECT_EQ(project.autoloads.size(), 2u);
}

TEST(LuaDefinitions, GeneratesApiAndNamesAndMergesSettingsWithoutLosingUserOptions) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto project = SampleProject();
    project.file = engine::FromUtf8(directory.path().toStdString()) / "test.sriproject";
    std::string error, definitions, config;
    ASSERT_TRUE(engine::CreateNewFile(project.Root() / ".luarc.json", R"({
        "workspace.library": ["my-library"], "diagnostics.globals": ["MyGlobal"], "runtime.version": "Lua 5.4"
    })", error));
    ASSERT_TRUE(editor::refreshLuaDefinitions(project, error)) << error;
    ASSERT_TRUE(engine::ReadTextFile(project.Root() / ".sri/lua/autoloads.d.lua", definitions, error));
    EXPECT_NE(definitions.find("---@field First table<string, any>\n"), std::string::npos);
    EXPECT_NE(definitions.find("---@field Second table<string, any>?"), std::string::npos);
    ASSERT_TRUE(engine::ReadTextFile(project.Root() / ".sri/lua/engine.d.lua", definitions, error));
    EXPECT_NE(definitions.find("function Log.info(message)"), std::string::npos);
    EXPECT_NE(definitions.find("function Entity:SetPosition(position)"), std::string::npos);
    EXPECT_NE(definitions.find("Properties = nil"), std::string::npos);
    EXPECT_NE(definitions.find("---@type table<string, string|number|boolean>"), std::string::npos);
    ASSERT_TRUE(engine::ReadTextFile(project.Root() / ".luarc.json", config, error));
    auto object = QJsonDocument::fromJson(QByteArray::fromStdString(config)).object();
    const auto libraries = object.value("workspace.library").toArray();
    ASSERT_EQ(libraries.size(), 2);
    EXPECT_EQ(libraries[0].toString(), QStringLiteral("my-library"));
    EXPECT_EQ(libraries[1].toString(), QStringLiteral(".sri/lua"));
    EXPECT_EQ(object.value("diagnostics.globals").toArray()[0].toString(), QStringLiteral("MyGlobal"));
    project.autoloads.erase(project.autoloads.begin());
    project.autoloads[0].name = "Renamed";
    ASSERT_TRUE(editor::refreshLuaDefinitions(project, error)) << error;
    ASSERT_TRUE(engine::ReadTextFile(project.Root() / ".sri/lua/autoloads.d.lua", definitions, error));
    EXPECT_EQ(definitions.find("---@field First"), std::string::npos);
    EXPECT_EQ(definitions.find("---@field Second"), std::string::npos);
    EXPECT_NE(definitions.find("---@field Renamed"), std::string::npos);
    std::string unchanged;
    ASSERT_TRUE(engine::ReadTextFile(project.Root() / ".luarc.json", unchanged, error));
    EXPECT_EQ(unchanged, config);
}

TEST(LuaDefinitions, NewProjectGetsLua54ConfigurationWithoutTouchingContent) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto project = SampleProject();
    project.file = engine::FromUtf8(directory.path().toStdString()) / "test.sriproject";
    std::string error, text;
    ASSERT_TRUE(editor::refreshLuaDefinitions(project, error)) << error;
    ASSERT_TRUE(engine::ReadTextFile(project.Root() / ".luarc.json", text, error));
    const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
    EXPECT_EQ(object.value("runtime.version").toString(), QStringLiteral("Lua 5.4"));
    EXPECT_EQ(object.value("runtime.path").toArray()[0].toString(), QStringLiteral("content/?.lua"));
    EXPECT_EQ(object.value("workspace.library").toArray()[0].toString(), QStringLiteral(".sri/lua"));
    EXPECT_FALSE(std::filesystem::exists(project.ContentDir()));
}

TEST(LuaDefinitions, InvalidAndCommentedConfigurationsArePreservedAndDefinitionsStillRefresh) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto project = SampleProject();
    project.file = engine::FromUtf8(directory.path().toStdString()) / "test.sriproject";
    std::string error, text;
    const auto config = project.Root() / ".luarc.json";
    ASSERT_TRUE(engine::CreateNewFile(config, "{broken", error));
    EXPECT_FALSE(editor::refreshLuaDefinitions(project, error));
    ASSERT_TRUE(engine::ReadTextFile(config, text, error));
    EXPECT_EQ(text, "{broken");
    EXPECT_TRUE(std::filesystem::exists(project.Root() / ".sri/lua/autoloads.d.lua"));
    std::filesystem::remove(config);
    ASSERT_TRUE(engine::CreateNewFile(project.Root() / ".luarc.jsonc", "// User's config\n{}", error));
    EXPECT_FALSE(editor::refreshLuaDefinitions(project, error));
    EXPECT_NE(error.find("workspace.library"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(config));
}

TEST(LuaDefinitions, NestedSettingsRetainTheirExistingLibrariesAndRuntimeChoices) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto project = SampleProject();
    project.file = engine::FromUtf8(directory.path().toStdString()) / "test.sriproject";
    std::string error, text;
    const auto config = project.Root() / ".luarc.json";
    ASSERT_TRUE(engine::CreateNewFile(config,
        R"({"workspace":{"library":["existing"]},"runtime":{"version":"Lua 5.4","path":["custom/?.lua"]}})", error));
    ASSERT_TRUE(editor::refreshLuaDefinitions(project, error)) << error;
    ASSERT_TRUE(engine::ReadTextFile(config, text, error));
    const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
    EXPECT_FALSE(object.contains("workspace.library"));
    EXPECT_FALSE(object.contains("runtime.path"));
    EXPECT_FALSE(object.contains("runtime.version"));
    const auto libraries = object.value("workspace").toObject().value("library").toArray();
    ASSERT_EQ(libraries.size(), 2);
    EXPECT_EQ(libraries[0].toString(), QStringLiteral("existing"));
    EXPECT_EQ(libraries[1].toString(), QStringLiteral(".sri/lua"));
    EXPECT_EQ(object.value("runtime").toObject().value("path").toArray()[0].toString(), QStringLiteral("custom/?.lua"));
}

TEST(AutoloadLogging, LuaUsesTheSharedDefaultLogger) {
    std::vector<std::string> messages;
    const auto logger = spdlog::default_logger();
    const auto previousLevel = logger->level();
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& message) {
        messages.emplace_back(message.payload.data(), message.payload.size());
    });
    struct RestoreLogger {
        std::shared_ptr<spdlog::logger> logger;
        std::shared_ptr<spdlog::sinks::sink> sink;
        spdlog::level::level_enum level;
        ~RestoreLogger() { std::erase(logger->sinks(), sink); logger->set_level(level); }
    } restore{logger, sink, previousLevel};
    logger->sinks().push_back(sink);
    logger->set_level(spdlog::level::info);
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("Log.info('Hello world')", "logging-test", error)) << error;
    EXPECT_EQ(std::count(messages.begin(), messages.end(), "[Lua] Hello world"), 1);
}

TEST(EditorPlayDocument, RuntimeChangesDoNotOverwriteOriginalDirtyStateOrSceneFile) {
    engine::Engine engine;
    engine::Project project;
    editor::Selection selection(engine.GetScene());
    editor::SceneDocument document(engine, project, selection);
    document.markClean();
    document.beginRuntime();
    engine.GetScene().CreateEntity("Runtime spawn");
    EXPECT_FALSE(document.isDirty());
    std::string error;
    EXPECT_FALSE(document.saveAs("must-not-write.scene.json", error));
    EXPECT_FALSE(std::filesystem::exists("must-not-write.scene.json"));
    document.endRuntime();
    EXPECT_FALSE(document.isDirty());
    document.markDirty();
    document.beginRuntime();
    engine.GetScene().Clear();
    document.endRuntime();
    EXPECT_TRUE(document.isDirty());
    EXPECT_TRUE(document.filePath().empty());
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
