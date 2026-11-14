#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/ai/BehaviorTree.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/core/FrameContext.hpp>
#include <gtest/gtest.h>

namespace {
constexpr entt::entity NullEntity = entt::null;
struct PlayFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-play-" + engine::NewUuid());
    std::filesystem::path previous = engine::AssetPathResolver::GetGameRoot();
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::ScriptSubsystem* scripting = host.AddSubsystem<engine::ScriptSubsystem>();
    PlayFixture() {
        std::filesystem::create_directories(root / "scenes");
        engine::AssetPathResolver::SetGameRoot(root);
        host.SetEditorMode();
    }
    ~PlayFixture() {
        host.Shutdown();
        engine::AssetPathResolver::SetGameRoot(previous);
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
    void Level(const char* name) {
        engine::SceneDocument document;
        engine::SceneEntityDefinition entity;
        entity.id = engine::NewUuid(); entity.name = name;
        document.entities.push_back(entity);
        std::string text, error;
        ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error));
        const auto file = root / "scenes" / (std::string(name) + ".scene.json");
        ASSERT_TRUE(engine::CreateNewFile(file, text, error));
        ASSERT_TRUE(engine::WriteAssetMetadata(file, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
    }
};

TEST(BehaviorTree, ResumesWithinBudgetAndCancelsOnlyActiveAction) {
    using namespace engine::ai;
    struct Leaves final : LeafExecutor {
        int checks = 0;
        int enters = 0;
        int ticks = 0;
        std::vector<ExitReason> exits;
        bool Check(std::size_t) override { ++checks; return true; }
        void Enter(std::size_t) override { ++enters; }
        Status Tick(std::size_t, double) override { return ++ticks % 2 ? Status::Running : Status::Success; }
        void Exit(std::size_t, ExitReason reason) override { exits.push_back(reason); }
    };
    std::string error;
    auto definition = TreeDefinition::Create({
        {"root", NodeKind::Sequence, {1, 2}, {}, {}},
        {"ready", NodeKind::Condition, {}, {}, {}},
        {"work", NodeKind::Action, {}, {}, {}},
    }, 0, error);
    ASSERT_NE(definition, nullptr) << error;
    auto executor = std::make_unique<Leaves>();
    const auto* leaves = executor.get();
    TreeInstance tree(definition, std::move(executor));
    EXPECT_EQ(tree.Tick(0.016, 1).steps, 1u);
    EXPECT_EQ(leaves->checks, 0);
    EXPECT_EQ(tree.Tick(0.016, 1).status, Status::Running);
    EXPECT_EQ(tree.Tick(0.016, 1).status, Status::Running);
    EXPECT_EQ(tree.Tick(0.016, 1).status, Status::Running);
    ASSERT_NE(tree.ActiveAction(), nullptr);
    EXPECT_EQ(tree.ActiveAction()->id, "work");
    EXPECT_EQ(tree.Tick(0.016, 1).status, Status::Success);
    EXPECT_EQ(leaves->checks, 1);
    EXPECT_EQ(leaves->enters, 1);
    EXPECT_EQ(tree.Tick(0.016, 4).status, Status::Running);
    tree.Stop();
    tree.Stop();
    EXPECT_EQ(leaves->exits, (std::vector{ExitReason::Success, ExitReason::Cancelled}));

    EXPECT_FALSE(TreeDefinition::Create({
        {"root", NodeKind::Action, {}, {}, {}},
        {"orphan", NodeKind::Sequence, {1}, {}, {}},
    }, 0, error));
    EXPECT_FALSE(error.empty());
}

TEST(PlayMode, BehaviorTreesShareDefinitionsAndKeepIndependentState) {
    PlayFixture f;
    std::string error;
    const auto asset = [&](const char* name, const char* source, engine::CatalogAssetKind kind) {
        const auto file = f.root / name;
        ASSERT_TRUE(engine::CreateNewFile(file, source, error)) << error;
        ASSERT_TRUE(engine::WriteAssetMetadata(file, {engine::NewUuid(), kind}, error)) << error;
    };
    asset("agent.bt.lua", R"(
        return { root = "choose", nodes = {
            { id = "choose", kind = "Selector", children = {"work", "fallback"} },
            { id = "work", kind = "Sequence", children = {"ready", "count"} },
            { id = "ready", kind = "Condition", script = "game/ready.condition.lua" },
            { id = "count", kind = "Action", script = "game/count.action.lua", parameters = {limit = 4} },
            { id = "fallback", kind = "Action", script = "game/fallback.action.lua" },
        }}
    )", engine::CatalogAssetKind::BehaviorTree);
    asset("ready.condition.lua", R"(
        return { Check = function(ctx) return ctx.parameters.ready end }
    )", engine::CatalogAssetKind::Script);
    asset("count.action.lua", R"(
        local count
        return {
            OnEnter = function(ctx) count = 0 end,
            Tick = function(ctx)
                count = count + 1
                local p = ctx.entity:GetPosition()
                p.x = count
                ctx.entity:SetPosition(p)
                return count >= ctx.parameters.limit and BT.Success or BT.Running
            end,
            OnExit = function(ctx, reason)
                if reason == "Cancelled" then ctx.entity:SetName("cancelled") end
            end,
        }
    )", engine::CatalogAssetKind::Script);
    asset("fallback.action.lua", "return {Tick = function() return BT.Failure end}", engine::CatalogAssetKind::Script);
    asset("runaway.bt.lua", R"(
        return { root = "loop", nodes = {
            { id = "loop", kind = "Action", script = "game/loop.action.lua" },
        }}
    )", engine::CatalogAssetKind::BehaviorTree);
    asset("loop.action.lua", "return {Tick = function() while true do end end}", engine::CatalogAssetKind::Script);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    scene.CreateEntity("first");
    scene.CreateEntity("second");
    scene.CreateEntity("third");
    scene.CreateEntity("runaway");
    scene.AddComponent<engine::ai::BehaviorTreeComponent>(scene.CreateEntity("unassigned"));
    const auto* catalogEntry = f.host.GetAssets().GetCatalog().FindPath("game/agent.bt.lua");
    ASSERT_NE(catalogEntry, nullptr);
    const auto authored = engine::ai::BehaviorTreeComponent{
        {catalogEntry->metadata.id, catalogEntry->path}, true, {{"ready", false}}};
    scene.AddComponent<engine::ai::BehaviorTreeComponent>(scene.FindFirstByName("third"), authored);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    auto& runtime = f.scripting->GetRuntime();
    ASSERT_TRUE(runtime.ExecuteString(R"(
        first = Scene.FindByName("first")
        second = Scene.FindByName("second")
        third = Scene.FindByName("third")
        runaway = Scene.FindByName("runaway")
        BT.Attach(runaway, "game/runaway.bt.lua")
        BT.Attach(first, "game/agent.bt.lua", {ready = true})
        BT.Attach(second, "game/agent.bt.lua", {ready = true})
    )", "attach agents", error)) << error;
    engine::FrameContext frame{};
    frame.deltaSeconds = 0.016;
    f.scripting->Update(frame);
    ASSERT_TRUE(runtime.ExecuteString(R"(
        local status, action = BT.GetStatus(first)
        assert(status == BT.Running and action == "count")
        assert(first:GetPosition().x == 1 and second:GetPosition().x == 1)
        assert(BT.GetStatus(third) == BT.Failure)
        assert(BT.GetStatus(runaway) == "Faulted")
        assert(BT.GetStatus(Scene.FindByName("unassigned")) == "Stopped")
        BT.SetParameters(first, {ready = true, limit = 2})
        BT.Detach(second)
    )", "edit agents", error)) << error;
    frame.paused = true;
    f.scripting->Update(frame);
    ASSERT_TRUE(runtime.ExecuteString(R"(
        assert(first:GetPosition().x == 1 and BT.GetStatus(first) == BT.Running)
        assert(second:GetName() == "cancelled")
    )", "check pause", error)) << error;
    frame.paused = false;
    f.scripting->Update(frame);
    const auto timing = runtime.GetBehaviorTiming();
    EXPECT_EQ(timing.updates, 2u);
    EXPECT_GE(timing.maxMilliseconds, timing.meanMilliseconds);
    EXPECT_TRUE(runtime.ExecuteString(R"(
        assert(first:GetPosition().x == 2 and BT.GetStatus(first) == BT.Success)
        assert(second:GetName() == "cancelled" and BT.GetStatus(second) == "Stopped")
        local updates, mean, maximum = BT.GetTiming()
        assert(updates == 2 and maximum >= mean)
    )", "check agents", error)) << error;
    f.host.StopPlay();
    EXPECT_EQ(scene.FindFirstByName("cancelled"), NullEntity);
    const auto* restored = scene.TryGetComponent<engine::ai::BehaviorTreeComponent>(scene.FindFirstByName("third"));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(*restored, authored);
    const auto* empty = scene.TryGetComponent<engine::ai::BehaviorTreeComponent>(scene.FindFirstByName("unassigned"));
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->tree.id.IsValid());
}

TEST(PlayMode, StopRestoresUnsavedEntitiesAndRemovesRuntimeChanges) {
    PlayFixture f;
    ASSERT_TRUE(f.host.Initialize());
    auto entity = f.host.GetScene().CreateEntity("Unsaved");
    const std::string id = f.host.GetScene().GetId(entity).ToString();
    f.host.GetScene().GetTransform(entity).position.x = 12;
    std::string error;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
        entity = f.host.GetScene().FindById(id);
        f.host.GetScene().DestroyEntity(entity);
        f.host.GetScene().CreateEntity("Spawned");
        EXPECT_FALSE(f.host.PlayCurrentScene(f.root, {}, error));
        f.host.StopPlay(); f.host.StopPlay();
        EXPECT_FALSE(f.host.IsPlaying());
        EXPECT_FALSE(f.scripting->GetRuntime().IsInitialized());
        entity = f.host.GetScene().FindById(id);
        ASSERT_TRUE(f.host.GetScene().IsValid(entity));
        EXPECT_EQ(f.host.GetScene().GetTransform(entity).position.x, 12);
        EXPECT_EQ(f.host.GetScene().FindFirstByName("Spawned"), NullEntity);
    }
}

TEST(PlayMode, LuaOpenLevelIsDeferredAndAutoloadStateSurvivesAcrossScenes) {
    PlayFixture f;
    f.Level("Next"); f.Level("Last");
    std::string error;
    const auto source = f.root / "Game.lua";
    ASSERT_TRUE(engine::CreateNewFile(source, R"(
        local shared = require('shared')
        function OnStart()
            shared.starts = shared.starts + 1
            starts = (starts or 0) + 1
            assert(Scene.OpenLevel('Next'))
        end
    )", error));
    ASSERT_TRUE(engine::CreateNewFile(f.root / "shared.lua", "return {starts = 0}", error));
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(f.root / "shared.lua", {engine::NewUuid(), engine::CatalogAssetKind::Script}, error));
    const auto behavior = f.root / "Player.lua";
    const engine::AssetMetadata behaviorMeta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(behavior, R"(
        function OnStart()
            Autoloads.Game.player_score = 42
        end
        function OnDestroy()
            assert(Autoloads.Game.player_score == 42)
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(behavior, behaviorMeta, error));
    ASSERT_TRUE(f.host.Initialize());
    f.host.GetScene().AddComponent<engine::ScriptComponent>(f.host.GetScene().CreateEntity("Editor original"),
        engine::ScriptComponent{{behaviorMeta.id, "game/Player.lua"}, true});
    const std::vector<engine::AutoloadDefinition> autoloads{{"Game", {meta.id, "game/Game.lua"}, true}};
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, autoloads, error)) << error;
    EXPECT_NE(f.host.GetScene().FindFirstByName("Editor original"), NullEntity);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Next"), NullEntity);
    ASSERT_TRUE(f.scripting->GetRuntime().ExecuteString("assert(Scene.OpenLevel('Last'))", "switch", error));
    EXPECT_NE(f.host.GetScene().FindFirstByName("Next"), NullEntity);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Last"), NullEntity);
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(
        "assert(require('shared').starts == 1 and Autoloads.Game.starts == 1 and Autoloads.Game.player_score == 42)",
        "check", error));
    EXPECT_FALSE(f.host.RequestOpenLevel("../escape", error));
    EXPECT_FALSE(f.host.RequestOpenLevel("Missing", error));
    EXPECT_NE(f.host.GetScene().FindFirstByName("Last"), NullEntity);
    f.host.StopPlay();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Editor original"), NullEntity);
    EXPECT_EQ(f.host.GetScene().FindFirstByName("Last"), NullEntity);
}

TEST(PlayMode, CursorModeFromOnStartLastsUntilStop) {
    PlayFixture f;
    f.Level("Next");
    auto* input = f.host.AddSubsystem<engine::InputSubsystem>();
    const auto file = f.root / "Mouse.lua";
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(file, "return {OnStart = function(self) Input.SetCursorMode(CursorMode.Locked) end}", error));
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    const std::vector<engine::AutoloadDefinition> autoloads{{"Mouse", {meta.id, "game/Mouse.lua"}, true}};
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, autoloads, error)) << error;
    f.host.Tick();
    EXPECT_EQ(input->GetCursorMode(), engine::CursorMode::Locked);
    ASSERT_TRUE(f.host.RequestOpenLevel("Next", error));
    f.host.Tick();
    EXPECT_EQ(input->GetCursorMode(), engine::CursorMode::Locked);
    f.host.StopPlay();
    EXPECT_EQ(input->GetCursorMode(), engine::CursorMode::Normal);
}

TEST(PlayMode, StartupFailureRestoresSceneAndCancelsQueuedLevel) {
    PlayFixture f; f.Level("Next");
    auto* input = f.host.AddSubsystem<engine::InputSubsystem>();
    const auto file = f.root / "Bad.lua";
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(file, "return {OnStart = function(self) Input.SetCursorMode(CursorMode.Hidden); assert(Scene.OpenLevel('Next')); error('fail') end}", error));
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    f.host.GetScene().CreateEntity("Original");
    const std::vector<engine::AutoloadDefinition> definitions{{"Bad", {meta.id, "game/Bad.lua"}, true}};
    EXPECT_FALSE(f.host.PlayCurrentScene(f.root, definitions, error));
    EXPECT_FALSE(f.host.IsPlaying());
    EXPECT_EQ(input->GetCursorMode(), engine::CursorMode::Normal);
    f.host.Tick();
    EXPECT_NE(f.host.GetScene().FindFirstByName("Original"), NullEntity);
    EXPECT_EQ(f.host.GetScene().FindFirstByName("Next"), NullEntity);
}

TEST(PlayMode, NativeControllersStayDormantInEditAndRestartOnPlay) {
    PlayFixture f;
    int created = 0, destroyed = 0;
    struct Counter : engine::Controller {
        int& created; int& destroyed;
        Counter(int& c, int& d) : created(c), destroyed(d) {}
        void OnCreate() override { ++created; }
        void OnDestroy() override { ++destroyed; }
    };
    ASSERT_TRUE(f.host.GetScene().RegisterController("counter", {1, {}, [&] { return std::make_unique<Counter>(created, destroyed); }}));
    const auto entity = f.host.GetScene().CreateEntity("Controlled");
    f.host.GetScene().AttachController(entity, {"counter", 1, true, {}});
    EXPECT_EQ(created, 0);
    ASSERT_TRUE(f.host.Initialize());
    std::string error;
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    EXPECT_EQ(created, 1);
    f.host.SetPaused(true);
    f.host.Tick();
    EXPECT_EQ(f.host.GetFrame().deltaSeconds, 0);
    f.host.StopPlay();
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(created, 1);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    EXPECT_EQ(created, 2);
    f.host.StopPlay();
}

TEST(PlayMode, NativeTeardownCannotLeaveCursorRequestInEditor) {
    PlayFixture f;
    auto* input = f.host.AddSubsystem<engine::InputSubsystem>();
    struct CursorController : engine::Controller {
        engine::InputSubsystem& input;
        explicit CursorController(engine::InputSubsystem& in) : input(in) {}
        void OnDestroy() override { input.SetCursorMode(engine::CursorMode::Hidden); }
    };
    ASSERT_TRUE(f.host.GetScene().RegisterController("cursor", {1, {}, [input] {
        return std::make_unique<CursorController>(*input);
    }}));
    const auto entity = f.host.GetScene().CreateEntity("Controlled");
    f.host.GetScene().AttachController(entity, {"cursor", 1, true, {}});
    ASSERT_TRUE(f.host.Initialize());
    std::string error;
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    input->SetCursorMode(engine::CursorMode::Locked);
    f.host.StopPlay();
    EXPECT_EQ(input->GetCursorMode(), engine::CursorMode::Normal);
}
TEST(ScriptComponents, IndependentInstancesMoveOwnersAndStopRestoresAuthoringData) {
    PlayFixture f;
    std::string error;
    const auto source = f.root / "Move.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(source, R"(
        elapsed = 0
        function OnStart() assert(entity:IsValid()) end
        function OnUpdate(dt)
            elapsed = elapsed + dt
            local p = entity:GetPosition()
            p.x = p.x + elapsed
            entity:SetPosition(p)
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    auto first = scene.CreateEntity("First");
    auto second = scene.CreateEntity("Second");
    scene.GetTransform(second).position.x = 10;
    for (const auto entity : {first, second})
        scene.AddComponent<engine::ScriptComponent>(entity, engine::ScriptComponent{{meta.id, "game/Move.lua"}, true});
    EXPECT_EQ(scene.AddComponent<engine::ScriptComponent>(first), nullptr);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    first = scene.FindFirstByName("First"); second = scene.FindFirstByName("Second");
    engine::FrameContext frame; frame.deltaSeconds = 0.5;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 0.5f);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 10.5f);
    scene.TryGetComponent<engine::ScriptComponent>(first)->enabled = false;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 0.5f);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 11.5f);
    scene.TryGetComponent<engine::ScriptComponent>(first)->enabled = true;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 1.0f); // elapsed restarted at zero.
    scene.RemoveComponent<engine::ScriptComponent>(second);
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 13.0f);
    f.host.StopPlay();
    first = scene.FindFirstByName("First"); second = scene.FindFirstByName("Second");
    EXPECT_FLOAT_EQ(scene.GetTransform(first).position.x, 0);
    EXPECT_FLOAT_EQ(scene.GetTransform(second).position.x, 10);
    ASSERT_TRUE(scene.HasComponent<engine::ScriptComponent>(second));
    EXPECT_TRUE(scene.TryGetComponent<engine::ScriptComponent>(first)->enabled);
}

TEST(ScriptComponents, SceneSwitchStartsNewScriptsAndInvalidatesRetainedOwner) {
    PlayFixture f;
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(f.root / "shared.lua", "return {owners={}, destroys=0}", error));
    const auto source = f.root / "Owner.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(source, R"(
        local shared = require('shared')
        function OnStart() table.insert(shared.owners, entity) end
        function OnDestroy()
            assert(entity:IsValid())
            shared.destroys = shared.destroys + 1
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(source, meta, error));
    engine::SceneDocument document;
    engine::SceneEntityDefinition next; next.id = engine::NewUuid(); next.name = "Next";
    next.script = engine::ScriptComponent{{meta.id, "game/Owner.lua"}, true};
    document.entities.push_back(next);
    std::string text;
    ASSERT_TRUE(engine::SerializeSceneDocument(document, text, error));
    const auto level = f.root / "scenes/Next.scene.json";
    ASSERT_TRUE(engine::CreateNewFile(level, text, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(level, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Old"), *next.script);
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    ASSERT_TRUE(f.host.RequestOpenLevel("Next", error));
    f.host.Tick();
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(R"(
        local s = require('shared')
        assert(#s.owners == 2 and s.destroys == 1)
        assert(not s.owners[1]:IsValid() and s.owners[2]:IsValid())
        assert(not pcall(function() s.owners[1]:GetPosition() end))
    )", "owners-after-scene-change", error)) << error;
    scene.DestroyEntity(scene.FindFirstByName("Next"));
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(
        "assert(require('shared').destroys == 2); assert(not require('shared').owners[2]:IsValid())",
        "owners-after-delete", error)) << error;
    f.host.StopPlay();
}

TEST(ScriptComponents, BrokenAndMissingAttachmentsDoNotStopHealthyObjects) {
    PlayFixture f;
    std::string error;
    const auto file = f.root / "Check.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(file, R"(
        function OnUpdate(dt)
            local p = entity:GetPosition()
            if p.x < 0 then error('broken object') end
            p.x = p.x + 1
            entity:SetPosition(p)
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    const auto bad = scene.CreateEntity("Broken");
    scene.GetTransform(bad).position.x = -1;
    const auto good = scene.CreateEntity("Healthy");
    for (auto entity : {bad, good}) scene.AddComponent<engine::ScriptComponent>(entity,
        engine::ScriptComponent{{meta.id, "game/Check.lua"}, true});
    scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Missing"),
        engine::ScriptComponent{{engine::NewUuid(), "game/missing.lua"}, true});
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    engine::FrameContext frame; frame.deltaSeconds = 0.1;
    f.scripting->Update(frame); f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("Healthy")).position.x, 2);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("Broken")).position.x, -1);
    f.host.StopPlay();
}

TEST(ScriptComponents, PropertyOverridesSurvivePlayStopAndSceneSwitch) {
    PlayFixture f;
    std::string error;
    const auto file = f.root / "Horse.lua";
    const engine::AssetMetadata meta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(file, R"(
        Properties = { horseName = "Horse", speed = 2 }
        function OnStart() Log.info(Properties.horseName) end
        function OnUpdate(dt)
            local p = entity:GetPosition()
            p.x = p.x + Properties.speed * dt
            entity:SetPosition(p)
            Properties.horseName = 'runtime only'
        end
    )", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(file, meta, error));
    engine::SceneDocument next;
    engine::SceneEntityDefinition horse; horse.id = engine::NewUuid(); horse.name = "NextHorse";
    horse.script = engine::ScriptComponent{{meta.id, "game/Horse.lua"}, true, {{"speed", 7}, {"horseName", "Next"}}};
    next.entities.push_back(horse);
    std::string text;
    ASSERT_TRUE(engine::SerializeSceneDocument(next, text, error));
    const auto level = f.root / "scenes/Next.scene.json";
    ASSERT_TRUE(engine::CreateNewFile(level, text, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(level, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Original"),
        engine::ScriptComponent{{meta.id, "game/Horse.lua"}, true, {{"speed", 3}, {"horseName", "Original name"}}});
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    engine::FrameContext frame; frame.deltaSeconds = 1;
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("Original")).position.x, 3);
    ASSERT_TRUE(f.host.RequestOpenLevel("Next", error));
    f.host.Tick();
    f.scripting->Update(frame);
    EXPECT_FLOAT_EQ(scene.GetTransform(scene.FindFirstByName("NextHorse")).position.x, 7);
    f.host.StopPlay();
    const auto* restored = scene.TryGetComponent<engine::ScriptComponent>(scene.FindFirstByName("Original"));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->properties.at("horseName").String(), "Original name");
    EXPECT_EQ(restored->properties.at("speed").Number(), 3);
}
} // namespace

TEST(ScriptComponents, ChaserDestroysScriptedTargetAndItself) {
    PlayFixture f;
    std::string error;
    const auto chase = f.root / "Chase.lua";
    const auto idle = f.root / "Idle.lua";
    const engine::AssetMetadata chaseMeta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    const engine::AssetMetadata idleMeta{engine::NewUuid(), engine::CatalogAssetKind::Script};
    ASSERT_TRUE(engine::CreateNewFile(chase, R"(
        local target
        function OnStart() target = Scene.FindByName('Cube') end
        function OnUpdate(dt)
            if target and target:IsValid() then
                local p = entity:GetPosition()
                local t = target:GetPosition()
                if (t - p):Length() < 1 then Scene.Destroy(target) else entity:SetPosition(p + (t - p) * 0.5) end
            else
                Scene.Destroy(entity)
            end
        end
    )", error));
    ASSERT_TRUE(engine::CreateNewFile(idle, "function OnUpdate(dt) entity:GetPosition() end", error));
    ASSERT_TRUE(engine::WriteAssetMetadata(chase, chaseMeta, error));
    ASSERT_TRUE(engine::WriteAssetMetadata(idle, idleMeta, error));
    ASSERT_TRUE(f.host.Initialize());
    auto& scene = f.host.GetScene();
    const auto cube = scene.CreateEntity("Cube");
    scene.GetTransform(cube).position.x = 5;
    scene.AddComponent<engine::ScriptComponent>(cube, engine::ScriptComponent{{idleMeta.id, "game/Idle.lua"}, true});
    for (int i = 0; i < 3; ++i)
        scene.AddComponent<engine::ScriptComponent>(scene.CreateEntity("Roach"), engine::ScriptComponent{{chaseMeta.id, "game/Chase.lua"}, true});
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
        engine::FrameContext frame; frame.deltaSeconds = 0.1;
        for (int i = 0; i < 20; ++i) f.scripting->Update(frame);
        EXPECT_EQ(scene.FindFirstByName("Cube"), NullEntity);
        EXPECT_EQ(scene.FindFirstByName("Roach"), NullEntity);
        f.host.StopPlay();
        EXPECT_NE(scene.FindFirstByName("Cube"), NullEntity);
    }
}

TEST(ScriptComponents, WrongArgumentTypesAreLuaErrorsInEveryConfig) {
    PlayFixture f;
    std::string error;
    ASSERT_TRUE(f.host.Initialize());
    f.host.GetScene().CreateEntity("Roach");
    ASSERT_TRUE(f.host.PlayCurrentScene(f.root, {}, error)) << error;
    EXPECT_TRUE(f.scripting->GetRuntime().ExecuteString(R"(
        local e = Scene.FindByName('Roach')
        assert(not pcall(function() e:SetRotation(0, 0, 0) end))
        assert(not pcall(function() e:SetRotation(Vector3.new(0, 0, 1)) end))
        assert(not pcall(function() e:SetPosition(1, 2, 3) end))
        assert(not pcall(function() e:SetScale(nil) end))
        e:SetRotation(Quaternion.Euler(0, 0, math.rad(15)))
    )", "wrong-arguments", error)) << error;
    f.host.StopPlay();
}
