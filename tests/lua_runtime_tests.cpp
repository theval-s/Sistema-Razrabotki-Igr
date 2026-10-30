#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>

#include <gtest/gtest.h>

#include <string>

namespace {
struct ScriptFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-scripts-" + engine::NewUuid());
    ScriptFixture() { std::filesystem::create_directories(root); }
    ~ScriptFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }

    engine::ScriptInstanceDesc Write(std::string_view source, const char* name = "behavior.lua") const {
        engine::ScriptInstanceDesc desc{root / name, engine::NewUuid(), engine::NewUuid()};
        std::string error;
        EXPECT_TRUE(engine::CreateNewFile(desc.source, source, error)) << error;
        return desc;
    }
};
}

TEST(LuaRuntime, InitializesAndShutsDownWithoutRendering) {
    engine::LuaRuntime runtime;
    std::string error;

    EXPECT_FALSE(runtime.IsInitialized());
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(runtime.IsInitialized());

    runtime.Shutdown();
    EXPECT_FALSE(runtime.IsInitialized());
}

TEST(ScriptSubsystem, ShutdownAndReinitializeStartAFreshSession) {
    ScriptFixture fixture;
    engine::ScriptSubsystem subsystem;
    std::string error;

    EXPECT_FALSE(subsystem.GetRuntime().IsInitialized());
    ASSERT_TRUE(subsystem.Initialize());
    EXPECT_FALSE(subsystem.GetRuntime().IsInitialized());
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    ASSERT_TRUE(subsystem.GetRuntime().ExecuteString("sessionValue = 42", "session", error)) << error;

    // Engine initialization and duplicate Play must not replace an active session.
    ASSERT_TRUE(subsystem.Initialize());
    EXPECT_FALSE(subsystem.BeginPlay(fixture.root, error));
    ASSERT_TRUE(subsystem.GetRuntime().ExecuteString("assert(sessionValue == 42)", "session", error)) << error;

    subsystem.EndPlay();
    subsystem.EndPlay();
    EXPECT_FALSE(subsystem.IsPlaying());
    EXPECT_FALSE(subsystem.GetRuntime().IsInitialized());
    EXPECT_FALSE(subsystem.GetRuntime().ExecuteString("sessionValue = 1", "closed", error));
    EXPECT_FALSE(error.empty());

    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    EXPECT_TRUE(subsystem.GetRuntime().ExecuteString("assert(sessionValue == nil)", "fresh", error)) << error;
    EXPECT_TRUE(error.empty());
    subsystem.Shutdown();
    EXPECT_FALSE(subsystem.IsPlaying());
    EXPECT_FALSE(subsystem.GetRuntime().IsInitialized());
    EXPECT_FALSE(subsystem.BeginPlay(fixture.root, error));
}

TEST(ScriptSubsystem, SessionsHaveIndependentLuaStates) {
    ScriptFixture fixture;
    engine::ScriptSubsystem first;
    engine::ScriptSubsystem second;
    std::string error;

    ASSERT_TRUE(first.Initialize());
    ASSERT_TRUE(second.Initialize());
    ASSERT_TRUE(first.BeginPlay(fixture.root, error)) << error;
    ASSERT_TRUE(second.BeginPlay(fixture.root, error)) << error;
    ASSERT_TRUE(first.GetRuntime().ExecuteString("sessionValue = 42", "first", error)) << error;
    EXPECT_TRUE(second.GetRuntime().ExecuteString("assert(sessionValue == nil)", "second", error)) << error;

    first.Shutdown();
    EXPECT_TRUE(second.GetRuntime().ExecuteString("assert(sessionValue == nil)", "still-open", error)) << error;
}

TEST(ScriptSubsystem, PausePreservesInstancesAndOnlyPlayUpdatesThem) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        return {
            Start = function(self) self.ticks = 0 end,
            Update = function(self, dt)
                assert(dt == 0.25)
                self.ticks = self.ticks + 1
                assert(self.ticks == 1)
            end
        }
    )");
    engine::ScriptSubsystem subsystem;
    engine::FrameContext frame;
    std::string error;
    EXPECT_FALSE(subsystem.BeginPlay(fixture.root, error));
    ASSERT_TRUE(subsystem.Initialize());
    subsystem.Update(frame);
    EXPECT_FALSE(subsystem.IsPlaying());
    EXPECT_FALSE(subsystem.BeginPlay(fixture.root / "missing", error));
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(subsystem.GetRuntime().CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(subsystem.GetRuntime().StartInstance(id, error)) << error;
    frame.paused = true;
    frame.deltaSeconds = 0.25;
    subsystem.Update(frame);
    EXPECT_TRUE(subsystem.IsPlaying());
    frame.paused = false;
    frame.deltaSeconds = 0.0;
    subsystem.Update(frame);
    // If either skipped frame called Update, this assertion would fail.
    EXPECT_TRUE(subsystem.GetRuntime().UpdateInstance(id, 0.25, error)) << error;
    subsystem.EndPlay();
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    engine::InstanceId fresh;
    ASSERT_TRUE(subsystem.GetRuntime().CreateInstance(desc, fresh, error)) << error;
    EXPECT_NE(id, fresh);
    EXPECT_FALSE(subsystem.GetRuntime().StartInstance(id, error));
}

TEST(LuaInstances, SameFileCreatesIndependentTablesAndEnvironments) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        privateValue = (privateValue or 0) + 1
        assert(privateValue == 1)
        return {
            Start = function(self) self.ticks = 0; stats.starts = stats.starts + 1 end,
            Update = function(self, dt)
                self.ticks = self.ticks + 1
                assert(self.ticks == 1 and dt == 0.5)
            end,
            Destroy = function(self) stats.destroys = stats.destroys + 1 end
        }
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {starts = 0, destroys = 0}", "setup", error)) << error;
    engine::InstanceId first, second;
    ASSERT_TRUE(runtime.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(desc, second, error)) << error;
    EXPECT_NE(first, second);
    EXPECT_FALSE(runtime.UpdateInstance(first, 0.5, error));
    ASSERT_TRUE(runtime.StartInstance(first, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(first, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(second, error)) << error;
    EXPECT_TRUE(runtime.UpdateInstance(first, 0.5, error)) << error;
    EXPECT_TRUE(runtime.UpdateInstance(second, 0.5, error)) << error;
    EXPECT_TRUE(runtime.DestroyInstance(first, error)) << error;
    EXPECT_TRUE(runtime.DestroyInstance(second, error)) << error;
    EXPECT_FALSE(runtime.DestroyInstance(first, error));
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.starts == 2 and stats.destroys == 2)", "check", error)) << error;
}

TEST(LuaInstances, RejectsInvalidScriptsAndPreservesOutputOnFailure) {
    ScriptFixture fixture;
    engine::LuaRuntime runtime;
    std::string error;
    engine::InstanceId id;
    auto desc = fixture.Write("return {}", "valid.lua");
    EXPECT_FALSE(runtime.CreateInstance(desc, id, error));
    EXPECT_FALSE(id.IsValid());
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    const auto original = id;
    for (const auto& bad : {
            fixture.Write("return 42", "number.lua"),
            fixture.Write("this is not lua!", "syntax.lua"),
            fixture.Write("return { Update = 42 }", "callback.lua"),
            fixture.Write("error('load failed')", "load-error.lua")}) {
        EXPECT_FALSE(runtime.CreateInstance(bad, id, error));
        EXPECT_EQ(id, original);
        EXPECT_NE(error.find(bad.assetId.ToString()), std::string::npos);
        EXPECT_NE(error.find(bad.entityId), std::string::npos);
    }
    desc.source = fixture.root / "missing.lua";
    EXPECT_FALSE(runtime.CreateInstance(desc, id, error));
    engine::LuaRuntime other;
    ASSERT_TRUE(other.Initialize(error)) << error;
    EXPECT_FALSE(other.StartInstance(original, error));
}

TEST(LuaInstances, FaultedInstancesStopUpdatingButCanBeDestroyed) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        return {
            Update = function(self, dt) error('update failed') end,
            Destroy = function(self) stats.destroyed = true end
        }
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {}", "setup", error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_FALSE(runtime.UpdateInstance(id, 0.1, error));
    EXPECT_NE(error.find("update failed"), std::string::npos);
    EXPECT_NE(error.find(desc.entityId), std::string::npos);
    EXPECT_FALSE(runtime.StartInstance(id, error));
    EXPECT_FALSE(runtime.UpdateInstance(id, 0.1, error));
    EXPECT_TRUE(runtime.DestroyInstance(id, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.destroyed)", "check", error)) << error;
}

TEST(LuaInstances, DestroyFailureStillReleasesInstanceAndShutdownIsSafe) {
    ScriptFixture fixture;
    const auto desc = fixture.Write("return { Destroy = function(self) error('destroy failed') end }");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_FALSE(runtime.DestroyInstance(id, error));
    EXPECT_NE(error.find("destroy failed"), std::string::npos);
    EXPECT_FALSE(runtime.StartInstance(id, error));
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    runtime.Shutdown();
    runtime.Shutdown();
    EXPECT_FALSE(runtime.IsInitialized());
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    EXPECT_FALSE(runtime.StartInstance(id, error));
}

TEST(ScriptSubsystem, UpdatesInstancesAndSkipsFaultedOnesWithoutCallingGlobalUpdate) {
    ScriptFixture fixture;
    const auto failing = fixture.Write(R"(
        return { Update = function(self, dt)
            stats.bad = stats.bad + 1
            error('one instance failed')
        end }
    )", "failing.lua");
    const auto healthy = fixture.Write(R"(
        return { Update = function(self, dt)
            assert(dt == 0.25)
            stats.good = stats.good + 1
        end }
    )", "healthy.lua");
    engine::ScriptSubsystem subsystem;
    std::string error;
    ASSERT_TRUE(subsystem.Initialize());
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    auto& runtime = subsystem.GetRuntime();
    ASSERT_TRUE(runtime.ExecuteString(R"(
        stats = {bad = 0, good = 0}
        function Update(dt) error('legacy Update must not run') end
    )", "setup", error)) << error;
    engine::InstanceId bad, good;
    ASSERT_TRUE(runtime.CreateInstance(failing, bad, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(healthy, good, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(bad, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(good, error)) << error;
    engine::FrameContext frame;
    frame.deltaSeconds = 0.25;
    subsystem.Update(frame);
    subsystem.Update(frame);
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.bad == 1 and stats.good == 2)", "check", error)) << error;
    frame.paused = true;
    subsystem.Update(frame);
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.good == 2)", "paused", error)) << error;
}

TEST(LuaInstances, FailedStartFaultsInstanceAndUnstartedDestructionDoesNotCallCallback) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        return {
            Start = function(self) error('start failed') end,
            Destroy = function(self) stats.destroys = stats.destroys + 1 end
        }
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {destroys = 0}", "setup", error)) << error;
    engine::InstanceId unstarted, failed;
    ASSERT_TRUE(runtime.CreateInstance(desc, unstarted, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(unstarted, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(desc, failed, error)) << error;
    EXPECT_FALSE(runtime.StartInstance(failed, error));
    EXPECT_NE(error.find("start failed"), std::string::npos);
    EXPECT_FALSE(runtime.UpdateInstance(failed, 0.1, error));
    EXPECT_TRUE(runtime.DestroyInstance(failed, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.destroys == 1)", "check", error)) << error;
}
