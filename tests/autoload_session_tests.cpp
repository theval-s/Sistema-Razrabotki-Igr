#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

struct GameRootScope {
    std::filesystem::path previous = engine::AssetPathResolver::GetGameRoot();
    explicit GameRootScope(const std::filesystem::path& root) { engine::AssetPathResolver::SetGameRoot(root); }
    ~GameRootScope() { engine::AssetPathResolver::SetGameRoot(previous); }
};

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-autoload-" + engine::NewUuid());
    GameRootScope gameRoot{root};
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::ScriptSubsystem* scripts = host.AddSubsystem<engine::ScriptSubsystem>();

    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() {
        host.Shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    engine::AutoloadDefinition Add(const char* name, std::string_view source, bool enabled = true) const {
        const auto file = root / (std::string(name) + ".lua");
        const engine::AssetMetadata metadata{engine::NewUuid(), engine::CatalogAssetKind::Script};
        std::string error;
        EXPECT_TRUE(engine::CreateNewFile(file, source, error)) << error;
        EXPECT_TRUE(engine::WriteAssetMetadata(file, metadata, error)) << error;
        return {name, {metadata.id, "game/" + std::string(name) + ".lua"}, enabled};
    }
};

struct LogCapture {
    std::vector<std::string> messages;
    std::vector<std::string> errors;
    std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
    spdlog::level::level_enum previous = logger->level();
    std::shared_ptr<spdlog::sinks::sink> sink =
        std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& message) {
            messages.emplace_back(message.payload.data(), message.payload.size());
            if (message.level == spdlog::level::err) errors.push_back(messages.back());
        });
    LogCapture() { logger->sinks().push_back(sink); logger->set_level(spdlog::level::info); }
    ~LogCapture() { std::erase(logger->sinks(), sink); logger->set_level(previous); }
    auto Count(const char* message) const { return std::count(messages.begin(), messages.end(), message); }
};

TEST(ScriptAutoloads, InitializeDoesNotStartLuaAndCompatibilityOverloadStillWorks) {
    Fixture fixture;
    ASSERT_NE(fixture.scripts, nullptr);
    ASSERT_TRUE(fixture.host.Initialize());
    EXPECT_EQ(fixture.host.GetSubsystem<engine::ScriptSubsystem>(), fixture.scripts);
    EXPECT_FALSE(fixture.scripts->IsPlaying());
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    std::string error;
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, error)) << error;
    EXPECT_TRUE(fixture.scripts->IsPlaying());
    EXPECT_EQ(fixture.scripts->GetRoot(), std::filesystem::canonical(fixture.root));
    fixture.scripts->EndPlay();
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
}

TEST(ScriptAutoloads, NamedInstancesShareLiveFieldsAndFunctionsAndRemainAvailableDuringTeardown) {
    Fixture fixture;
    const std::vector<engine::AutoloadDefinition> definitions{
        fixture.Add("GameState", R"(
            player_score = 10
            local secret = 5
            function AddScore(amount) player_score = player_score + amount + secret end
            function OnStart() assert(Autoloads.GameState.player_score == 10) end
            function OnUpdate(dt) assert(player_score == 20) end
            function OnDestroy()
                assert(Autoloads.GameState.player_score == 20)
                assert(Autoloads.Legacy == nil)
            end
        )"),
        fixture.Add("Legacy", R"(
            return {
                value = 3,
                Add = function(self, amount) self.value = self.value + amount end,
                OnStart = function(self)
                    assert(Autoloads.Legacy == self)
                    assert(Autoloads.GameState.secret == nil)
                    Autoloads.GameState.AddScore(5)
                end,
                OnDestroy = function(self) assert(Autoloads.GameState.player_score == 20) end
            }
        )"),
        fixture.Add("Disabled", "error('must not run')", false),
    };
    std::string error;
    ASSERT_TRUE(fixture.host.Initialize());
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    auto& runtime = fixture.scripts->GetRuntime();
    ASSERT_TRUE(runtime.ExecuteString(R"(
        assert(Autoloads.Disabled == nil)
        assert(Autoloads.GameState.player_score == 20)
        Autoloads.Legacy:Add(7)
        assert(Autoloads.Legacy.value == 10)
    )", "named-autoloads", error)) << error;
    LogCapture capture;
    engine::FrameContext frame; frame.deltaSeconds = 0.25;
    fixture.scripts->Update(frame);
    fixture.scripts->EndPlay();
    EXPECT_TRUE(capture.errors.empty());
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(next(Autoloads) == nil)", "fresh-autoloads", error)) << error;
}

TEST(ScriptAutoloads, StartOrderUsesOneStateAndSharedModulesAndStopReversesOrder) {
    Fixture fixture;
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(fixture.root / "shared.lua", "return {sequence = 0}", error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(fixture.root / "shared.lua",
        {engine::NewUuid(), engine::CatalogAssetKind::Script}, error)) << error;
    const std::vector<engine::AutoloadDefinition> definitions = {
        fixture.Add("First", R"(
            local shared = require('shared')
            return {
                OnStart = function(self)
                    assert(shared.sequence == 0)
                    shared.sequence = 1
                    self.updates = 0
                    Log.info('first-start')
                end,
                OnUpdate = function(self, dt)
                    assert(dt == 0.25)
                    self.updates = self.updates + 1
                    Log.info('first-update-' .. self.updates)
                end,
                OnDestroy = function(self) Log.info('first-stop') end
            }
        )"),
        fixture.Add("Second", R"(
            local shared = require('shared')
            return {
                OnStart = function(self)
                    assert(shared.sequence == 1)
                    shared.sequence = 2
                    Log.info('second-start')
                end,
                OnDestroy = function(self) Log.info('second-stop') end
            }
        )"),
        fixture.Add("Disabled", "error('disabled script must never execute')", false),
    };
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    ASSERT_GE(capture.messages.size(), 2u);
    EXPECT_EQ(capture.messages[capture.messages.size() - 2], "[Lua] first-start");
    EXPECT_EQ(capture.messages.back(), "[Lua] second-start");
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, error));
    ASSERT_TRUE(fixture.scripts->GetRuntime().ExecuteString(
        "assert(require('shared').sequence == 2)", "active-session", error)) << error;
    engine::FrameContext frame;
    frame.deltaSeconds = 0.25;
    fixture.scripts->Update(frame);
    EXPECT_EQ(capture.Count("[Lua] first-update-1"), 1);
    fixture.scripts->EndPlay();
    const auto secondStop = std::find(capture.messages.begin(), capture.messages.end(), "[Lua] second-stop");
    const auto firstStop = std::find(capture.messages.begin(), capture.messages.end(), "[Lua] first-stop");
    ASSERT_NE(secondStop, capture.messages.end());
    ASSERT_NE(firstStop, capture.messages.end());
    EXPECT_LT(secondStop, firstStop);
    fixture.scripts->EndPlay();
    EXPECT_EQ(capture.Count("[Lua] first-stop"), 1);
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    fixture.scripts->EndPlay();
    EXPECT_EQ(capture.Count("[Lua] first-start"), 2);  // A fresh require cache on the second Play.
}

TEST(ScriptAutoloads, ResolvesUuidAfterMoveUsingSharedCatalogEvenWhenSceneCatalogIsEmpty) {
    Fixture fixture;
    const auto definition = fixture.Add("Moved", "return {OnStart = function(self) Log.info('moved-start') end}");
    const auto original = fixture.root / "Moved.lua";
    const auto moved = fixture.root / "renamed.lua";
    std::filesystem::rename(original, moved);
    std::filesystem::rename(engine::AssetMetadataPath(original), engine::AssetMetadataPath(moved));
    ASSERT_TRUE(fixture.host.Initialize());
    std::string error;
    std::filesystem::create_directory(fixture.root / "empty");
    {
        GameRootScope emptyRoot{fixture.root / "empty"};
        ASSERT_TRUE(fixture.host.GetSceneAssets().RescanCatalog(error)) << error;
    }
    ASSERT_EQ(fixture.host.GetSceneAssets().GetCatalog().Find(definition.script.id), nullptr);
    ASSERT_NE(fixture.host.GetAssets().GetCatalog().Find(definition.script.id), nullptr);
    const std::vector<engine::AutoloadDefinition> definitions{definition};
    LogCapture capture;
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    EXPECT_EQ(capture.Count("[Lua] moved-start"), 1);
    fixture.scripts->EndPlay();
}

TEST(ScriptAutoloads, PreflightRejectsMissingWrongKindAndOutsideRootBeforeRunningEarlierScripts) {
    Fixture fixture;
    const auto first = fixture.Add("First", "return {OnStart = function(self) Log.info('must-not-start') end}");
    const auto second = fixture.Add("Second", "return {}");
    const auto wrongFile = fixture.root / "wrong.scene.json";
    const engine::AssetMetadata wrongMetadata{engine::NewUuid(), engine::CatalogAssetKind::Scene};
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(wrongFile, "{}", error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(wrongFile, wrongMetadata, error)) << error;
    std::filesystem::create_directory(fixture.root / "restricted");
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    std::vector<engine::AutoloadDefinition> definitions{first, {"Missing", {engine::NewUuid(), first.script.path}, true}};
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("Missing"), std::string::npos);
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    definitions[1] = {"WrongKind", {wrongMetadata.id, "game/wrong.scene.json"}, true};
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("WrongKind"), std::string::npos);
    definitions[1] = second;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root / "restricted", definitions, error));
    EXPECT_NE(error.find("escapes"), std::string::npos);
    std::filesystem::remove(fixture.root / "Second.lua");  // Stale catalog entry.
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("Second"), std::string::npos);
    EXPECT_FALSE(fixture.scripts->IsPlaying());
    EXPECT_EQ(capture.Count("[Lua] must-not-start"), 0);
    definitions[1].enabled = false;
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    fixture.scripts->EndPlay();
}

TEST(ScriptAutoloads, StartupErrorCleansUpAndAllowsRetry) {
    Fixture fixture;
    const auto first = fixture.Add("First", "return {OnDestroy = function(self) Log.info('rollback-first') end}");
    const auto failed = fixture.Add("Broken", "return {OnStart = function(self) error('startup failed') end}");
    const std::vector<engine::AutoloadDefinition> definitions{first, failed};
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    std::string error;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("Broken"), std::string::npos);
    EXPECT_NE(error.find("startup failed"), std::string::npos);
    EXPECT_FALSE(fixture.scripts->IsPlaying());
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    EXPECT_TRUE(fixture.scripts->GetRoot().empty());
    EXPECT_EQ(capture.Count("[Lua] rollback-first"), 1);
    // Creation failures also unwind instances already started before the bad asset.
    ASSERT_TRUE(engine::WriteFileAtomic(fixture.root / "Broken.lua", "return 42", error)) << error;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("Broken"), std::string::npos);
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    EXPECT_EQ(capture.Count("[Lua] rollback-first"), 2);
    const std::vector<engine::AutoloadDefinition> retry{first};
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, retry, error)) << error;
    fixture.scripts->EndPlay();
}

TEST(ScriptAutoloads, LuaFailureLogsOwnerUuidAndSourceLineExactlyOnce) {
    Fixture fixture;
    const auto definition = fixture.Add("Broken", "return {\nOnStart = function(self)\nerror('start-failure')\nend\n}");
    const std::vector<engine::AutoloadDefinition> definitions{definition};
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    std::string error;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("autoload=Broken"), std::string::npos);
    EXPECT_NE(error.find("script=" + definition.script.id.ToString()), std::string::npos);
    EXPECT_NE(error.find(engine::ToUtf8(std::filesystem::canonical(fixture.root / "Broken.lua"))), std::string::npos);
    EXPECT_NE(error.find("Broken.lua:3:"), std::string::npos);
    EXPECT_EQ(error.find("entity="), std::string::npos);
    ASSERT_EQ(capture.errors.size(), 1u);
    EXPECT_EQ(capture.errors.front(), "[Lua] " + error);
    fixture.scripts->EndPlay();
    EXPECT_EQ(capture.errors.size(), 1u);
}

TEST(ScriptAutoloads, SyntaxFailureAlsoIdentifiesAutoloadAndLogsOnce) {
    Fixture fixture;
    const auto definition = fixture.Add("Syntax", "return {\nOnStart = function(self)\nlocal = 1\nend\n}");
    const std::vector<engine::AutoloadDefinition> definitions{definition};
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    std::string error;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("autoload=Syntax"), std::string::npos);
    EXPECT_NE(error.find(definition.script.id.ToString()), std::string::npos);
    EXPECT_NE(error.find("Syntax.lua:3:"), std::string::npos);
    ASSERT_EQ(capture.errors.size(), 1u);
    EXPECT_EQ(capture.errors.front(), "[Lua] " + error);
}

TEST(ScriptAutoloads, PreflightFailuresAreLoggedBySubsystemOnceWithoutStartingLua) {
    Fixture fixture;
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    const engine::AutoloadDefinition missing{"Missing", {engine::NewUuid(), "game/missing.lua"}, true};
    const std::vector<engine::AutoloadDefinition> definitions{missing};
    std::string error;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("Missing"), std::string::npos);
    EXPECT_NE(error.find(missing.script.id.ToString()), std::string::npos);
    ASSERT_EQ(capture.errors.size(), 1u);
    EXPECT_EQ(capture.errors.front(), "[Lua] " + error);
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root / "missing-root", error));
    ASSERT_EQ(capture.errors.size(), 2u);
    EXPECT_EQ(capture.errors.back(), "[Lua] " + error);
}

TEST(ScriptAutoloads, EntityDiagnosticsStillUseTheExistingEntityId) {
    Fixture fixture;
    const auto definition = fixture.Add("EntityBehavior", "return {OnStart = function(self) error('entity-error') end}");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(fixture.root, error)) << error;
    // The existing three-field aggregate remains source-compatible.
    const engine::ScriptInstanceDesc desc{fixture.root / "EntityBehavior.lua", definition.script.id, engine::NewUuid()};
    engine::InstanceId instance;
    ASSERT_TRUE(runtime.CreateInstance(desc, instance, error)) << error;
    LogCapture capture;
    EXPECT_FALSE(runtime.StartInstance(instance, error));
    EXPECT_NE(error.find("entity=" + desc.entityId), std::string::npos);
    EXPECT_EQ(error.find("autoload="), std::string::npos);
    EXPECT_EQ(capture.errors.size(), 1u);
}

TEST(ScriptAutoloads, RollbackReversesAllStartedServicesEvenWhenDestroyFails) {
    Fixture fixture;
    const std::vector<engine::AutoloadDefinition> definitions{
        fixture.Add("First", "return {OnDestroy = function(self) Log.info('rollback-A') end}"),
        fixture.Add("Second", "return {OnDestroy = function(self) Log.info('rollback-B'); error('destroy-failure') end}"),
        fixture.Add("Third", "return {OnStart = function(self) error('start-failure') end}"),
    };
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    std::string error;
    EXPECT_FALSE(fixture.scripts->BeginPlay(fixture.root, definitions, error));
    EXPECT_NE(error.find("autoload=Third"), std::string::npos);  // Cleanup doesn't overwrite the startup error.
    const auto second = std::find(capture.messages.begin(), capture.messages.end(), "[Lua] rollback-B");
    const auto first = std::find(capture.messages.begin(), capture.messages.end(), "[Lua] rollback-A");
    ASSERT_NE(second, capture.messages.end());
    ASSERT_NE(first, capture.messages.end());
    EXPECT_LT(second, first);
    EXPECT_EQ(capture.errors.size(), 2u);  // One startup error and one independent teardown error.
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    fixture.scripts->EndPlay();
    EXPECT_EQ(capture.Count("[Lua] rollback-A"), 1);
    const std::vector<engine::AutoloadDefinition> retry{definitions.front()};
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, retry, error)) << error;
    fixture.scripts->EndPlay();
}

TEST(ScriptAutoloads, UpdateFailureDisablesOnlyItsInstanceAndIsNotReloggedOnStop) {
    Fixture fixture;
    const auto broken = fixture.Add("BrokenUpdate", R"(
        return {
            OnUpdate = function(self, dt) error('update-failure') end,
            OnDestroy = function(self) Log.info('faulted-must-not-destroy') end
        }
    )");
    const auto healthy = fixture.Add("Healthy", R"(
        return {
            OnUpdate = function(self, dt) Log.info('healthy-update') end,
            OnDestroy = function(self) Log.info('healthy-stop') end
        }
    )");
    const std::vector<engine::AutoloadDefinition> definitions{broken, healthy};
    ASSERT_TRUE(fixture.host.Initialize());
    LogCapture capture;
    std::string error;
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    engine::FrameContext frame;
    frame.deltaSeconds = 0.25;
    fixture.scripts->Update(frame);
    fixture.scripts->Update(frame);
    EXPECT_TRUE(fixture.scripts->IsPlaying());
    ASSERT_EQ(capture.errors.size(), 1u);
    EXPECT_NE(capture.errors.front().find("autoload=BrokenUpdate"), std::string::npos);
    EXPECT_NE(capture.errors.front().find(broken.script.id.ToString()), std::string::npos);
    EXPECT_EQ(capture.Count("[Lua] healthy-update"), 2);
    fixture.scripts->EndPlay();
    EXPECT_EQ(capture.Count("[Lua] healthy-stop"), 1);
    EXPECT_EQ(capture.Count("[Lua] faulted-must-not-destroy"), 0);
    EXPECT_EQ(capture.errors.size(), 1u);
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    fixture.scripts->Update(frame);
    EXPECT_EQ(capture.errors.size(), 2u);  // A new Play gets a fresh instance.
    fixture.scripts->EndPlay();
}

TEST(ScriptAutoloads, StopDestroysEntitiesBeforeServicesAndContinuesAfterDestroyError) {
    Fixture fixture;
    const auto service = fixture.Add("Service", "return {OnDestroy = function(self) Log.info('service-stop') end}");
    const auto throwing = fixture.Add("Throwing", "return {OnDestroy = function(self) error('destroy-failure') end}");
    const auto entity = fixture.Add("EntityBehavior", "return {OnDestroy = function(self) Log.info('entity-stop') end}");
    const std::vector<engine::AutoloadDefinition> definitions{service, throwing};
    ASSERT_TRUE(fixture.host.Initialize());
    std::string error;
    ASSERT_TRUE(fixture.scripts->BeginPlay(fixture.root, definitions, error)) << error;
    engine::InstanceId id;
    const engine::ScriptInstanceDesc desc{fixture.root / "EntityBehavior.lua", entity.script.id, engine::NewUuid()};
    ASSERT_TRUE(fixture.scripts->GetRuntime().CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(fixture.scripts->GetRuntime().StartInstance(id, error)) << error;
    LogCapture capture;
    fixture.scripts->EndPlay();
    const auto entityStop = std::find(capture.messages.begin(), capture.messages.end(), "[Lua] entity-stop");
    const auto serviceStop = std::find(capture.messages.begin(), capture.messages.end(), "[Lua] service-stop");
    ASSERT_NE(entityStop, capture.messages.end());
    ASSERT_NE(serviceStop, capture.messages.end());
    EXPECT_LT(entityStop, serviceStop);
    ASSERT_EQ(capture.errors.size(), 1u);
    EXPECT_NE(capture.errors.front().find("autoload=Throwing"), std::string::npos);
    EXPECT_FALSE(fixture.scripts->GetRuntime().IsInitialized());
    EXPECT_FALSE(fixture.scripts->GetRuntime().UpdateInstance(id, 0.25, error));
    fixture.scripts->EndPlay();
    EXPECT_EQ(capture.Count("[Lua] entity-stop"), 1);
    EXPECT_EQ(capture.Count("[Lua] service-stop"), 1);
    EXPECT_EQ(capture.errors.size(), 1u);
}

}  // namespace
