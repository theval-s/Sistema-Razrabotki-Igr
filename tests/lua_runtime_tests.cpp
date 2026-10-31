#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/assets/AssetPathResolver.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>

#include <gtest/gtest.h>

#include <string>
#include <algorithm>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#endif

namespace {
struct LogCapture {
    std::vector<std::string> errors;
    std::vector<std::string> messages;
    std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink =
        std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& message) {
            messages.emplace_back(message.payload.data(), message.payload.size());
            if (message.level == spdlog::level::err) errors.push_back(messages.back());
        });
    LogCapture() { logger->sinks().push_back(sink); }
    ~LogCapture() { std::erase(logger->sinks(), sink); }
};

struct ScriptFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-scripts-" + engine::NewUuid());
    ScriptFixture() { std::filesystem::create_directories(root); }
    ~ScriptFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }

    std::filesystem::path File(const std::filesystem::path& relative, std::string_view source) const {
        const auto file = root / relative;
        std::filesystem::create_directories(file.parent_path());
        std::string error;
        EXPECT_TRUE(engine::CreateNewFile(file, source, error)) << error;
        return file;
    }

    engine::ScriptInstanceDesc Write(std::string_view source, const char* name = "behavior.lua") const {
        engine::ScriptInstanceDesc desc{root / name, engine::NewUuid(), engine::NewUuid()};
        std::string error;
        EXPECT_TRUE(engine::CreateNewFile(desc.source, source, error)) << error;
        return desc;
    }
};

struct CurrentDirectoryScope {
    std::filesystem::path previous = std::filesystem::current_path();
    explicit CurrentDirectoryScope(const std::filesystem::path& directory) { std::filesystem::current_path(directory); }
    ~CurrentDirectoryScope() { std::error_code ignored; std::filesystem::current_path(previous, ignored); }
};

struct GameRootScope {
    std::filesystem::path previous = engine::AssetPathResolver::GetGameRoot();
    explicit GameRootScope(const std::filesystem::path& root) { engine::AssetPathResolver::SetGameRoot(root); }
    ~GameRootScope() { engine::AssetPathResolver::SetGameRoot(previous); }
};

#ifdef _WIN32
// Junction creation does not require the Windows symbolic-link privilege.
std::error_code CreateTestJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::filesystem::create_directory(link);
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {static_cast<int>(GetLastError()), std::system_category()};
    const std::wstring printName = std::filesystem::canonical(target).native();
    const std::wstring substitute = L"\\??\\" + printName;
    struct Header {
        DWORD tag;
        WORD dataLength, reserved;
        WORD substituteOffset, substituteLength, printOffset, printLength;
    };
    static_assert(sizeof(Header) == 16);
    const auto substituteBytes = substitute.size() * sizeof(wchar_t);
    const auto printBytes = printName.size() * sizeof(wchar_t);
    const auto total = sizeof(Header) + substituteBytes + printBytes + 2 * sizeof(wchar_t);
    std::vector<std::byte> buffer(total);
    auto* header = reinterpret_cast<Header*>(buffer.data());
    *header = {IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(total - 8), 0,
               0, static_cast<WORD>(substituteBytes),
               static_cast<WORD>(substituteBytes + sizeof(wchar_t)), static_cast<WORD>(printBytes)};
    std::memcpy(buffer.data() + sizeof(Header), substitute.c_str(), substituteBytes + sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(Header) + header->printOffset, printName.c_str(), printBytes + sizeof(wchar_t));
    DWORD returned = 0;
    const bool success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != 0;
    const DWORD failure = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    return {static_cast<int>(failure), std::system_category()};
}
#endif
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

TEST(ScriptProperties, ReadsLiteralDefaultsWithoutExecutingGameplayAndRejectsComputedValues) {
    engine::ScriptPropertyMap values;
    std::string error;
    ASSERT_TRUE(engine::ReadScriptProperties(R"(
        error('must never execute in the inspector')
        -- Properties = { fake = 1 }
        local ignored = [==[ Properties = { fake = 2 } ]==]
        Properties = { horseName = "Star\nRunner", speed = -2.5e1, racing = true, note = [[hello]], }
        function OnStart() Properties.speed = 7 end
    )", values, error)) << error;
    EXPECT_EQ(values.size(), 4u);
    EXPECT_EQ(values.at("horseName").String(), "Star\nRunner");
    EXPECT_EQ(values.at("speed").Number(), -25);
    EXPECT_TRUE(values.at("racing").Boolean());
    for (const auto* source : {"Properties = { speed = getSpeed() }", "Properties = { speed = 1 + 2 }",
        "Properties = { data = {} }", "Properties = { x = 1, x = 2 }", "local Properties = {}",
        "Properties = { x = 1e999 }"}) EXPECT_FALSE(engine::ReadScriptProperties(source, values, error)) << source;
    ASSERT_TRUE(engine::ReadScriptProperties("function OnStart() local Properties = {} end", values, error));
    EXPECT_TRUE(values.empty());
}

TEST(ScriptProperties, AppliesIndependentOverridesBeforeStartAndPreservesPrivateRuntimeChanges) {
    ScriptFixture fixture;
    auto first = fixture.Write(R"(
        Properties = { horseName = "Default", speed = 2, racing = true }
        function OnStart()
            Log.info(Properties.horseName)
            assert(Properties.speed == 2 and Properties.racing == true)
        end
        function OnUpdate(dt) Properties.horseName = 'changed' end
    )");
    first.properties = {{"horseName", "First"}, {"speed", "wrong type"}, {"removed", true}};
    auto second = first;
    second.properties = {{"horseName", "Second"}};
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(fixture.root, error));
    LogCapture logs;
    engine::InstanceId a, b;
    ASSERT_TRUE(runtime.CreateInstance(first, a, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(second, b, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(a, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(a, 0.1, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(b, error)) << error;
    EXPECT_NE(std::find(logs.messages.begin(), logs.messages.end(), "[Lua] First"), logs.messages.end());
    EXPECT_NE(std::find(logs.messages.begin(), logs.messages.end(), "[Lua] Second"), logs.messages.end());
    EXPECT_EQ(first.properties.at("horseName").String(), "First");
    first.properties.erase("horseName"); // The Inspector's Reset operation.
    engine::InstanceId reset;
    ASSERT_TRUE(runtime.CreateInstance(first, reset, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(reset, error)) << error;
    EXPECT_NE(std::find(logs.messages.begin(), logs.messages.end(), "[Lua] Default"), logs.messages.end());
}

TEST(LuaTopLevel, InstancesHavePrivateFieldsClosuresAndExactCallbackArguments) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        elapsed = 0
        nested = {count = 0}
        Properties = {speed = 5}
        local privateCount = 0
        function OnStart(...)
            assert(select('#', ...) == 0)
            assert(elapsed == 0 and nested.count == 0 and privateCount == 0)
            stats.starts = stats.starts + 1
        end
        function OnUpdate(dt, ...)
            assert(select('#', ...) == 0 and dt == 0.25)
            elapsed = elapsed + dt
            privateCount = privateCount + 1
            nested.count = nested.count + 1
            assert(elapsed == 0.25 and privateCount == 1 and nested.count == 1)
            stats.updates = stats.updates + 1
        end
        function OnDestroy(...)
            assert(select('#', ...) == 0)
            stats.destroys = stats.destroys + 1
        end
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error));
    ASSERT_TRUE(runtime.ExecuteString("stats = {starts=0, updates=0, destroys=0}", "setup", error));
    engine::InstanceId first, second;
    ASSERT_TRUE(runtime.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(desc, second, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(first, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(first, 0.25, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(second, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(second, 0.25, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(first, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(second, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString(
        "assert(stats.starts == 2 and stats.updates == 2 and stats.destroys == 2); assert(elapsed == nil and nested == nil and Properties == nil)",
        "check", error)) << error;
}

TEST(LuaTopLevel, MissingCallbacksDoNotInheritRuntimeGlobalCallbacks) {
    ScriptFixture fixture;
    const auto desc = fixture.Write("local temporary = 42");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error));
    ASSERT_TRUE(runtime.ExecuteString(R"(
        function OnStart() error('inherited callback') end
        function OnUpdate() error('inherited callback') end
        function OnDestroy() error('inherited callback') end
    )", "setup", error));
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    EXPECT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_TRUE(runtime.UpdateInstance(id, 0.25, error)) << error;
    EXPECT_TRUE(runtime.DestroyInstance(id, error)) << error;
}

TEST(LuaTopLevel, RejectsInvalidCallbackAndReturnFormats) {
    ScriptFixture fixture;
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error));
    const std::vector<std::string> sources{"OnStart = 42", "OnUpdate = false", "OnDestroy = 'bad'", "return nil", "return {}, {}"};
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto desc = fixture.Write(sources[i], ("bad" + std::to_string(i) + ".lua").c_str());
        engine::InstanceId id;
        EXPECT_FALSE(runtime.CreateInstance(desc, id, error));
        EXPECT_FALSE(id.IsValid());
    }
}

TEST(LuaTopLevel, RequireKeepsSharedModuleSemanticsAndErrorsFaultOnlyOneInstance) {
    ScriptFixture fixture;
    fixture.File("shared.lua", "return {count=0}");
    const auto desc = fixture.Write(R"(
        local shared = require('shared')
        function OnStart() shared.count = shared.count + 1 end
        function OnUpdate(dt) error('top-level update failure') end
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(fixture.root, error));
    engine::InstanceId first, second;
    ASSERT_TRUE(runtime.CreateInstance(desc, first, error));
    ASSERT_TRUE(runtime.CreateInstance(desc, second, error));
    ASSERT_TRUE(runtime.StartInstance(first, error));
    EXPECT_FALSE(runtime.UpdateInstance(first, 0.25, error));
    ASSERT_TRUE(runtime.StartInstance(second, error));
    EXPECT_TRUE(runtime.ExecuteString("assert(require('shared').count == 2)", "shared-check", error)) << error;
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
            OnStart = function(self) self.ticks = 0 end,
            OnUpdate = function(self, dt)
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
            OnStart = function(self) self.ticks = 0; stats.starts = stats.starts + 1 end,
            OnUpdate = function(self, dt)
                self.ticks = self.ticks + 1
                assert(self.ticks == 1 and dt == 0.5)
            end,
            OnDestroy = function(self) stats.destroys = stats.destroys + 1 end
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
    EXPECT_TRUE(runtime.DestroyInstance(first, error));
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
            fixture.Write("return { OnUpdate = 42 }", "callback.lua"),
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
            OnUpdate = function(self, dt) error('update failed') end,
            OnDestroy = function(self) stats.destroyed = true end
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
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.destroyed == nil)", "check", error)) << error;
}

TEST(LuaInstances, DestroyFailureStillReleasesInstanceAndShutdownIsSafe) {
    ScriptFixture fixture;
    LogCapture logs;
    const auto desc = fixture.Write("return { OnDestroy = function(self) error('destroy failed') end }");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_FALSE(runtime.DestroyInstance(id, error));
    EXPECT_NE(error.find("destroy failed"), std::string::npos);
    EXPECT_TRUE(runtime.DestroyInstance(id, error));
    EXPECT_EQ(logs.errors.size(), 1u);
    EXPECT_FALSE(runtime.StartInstance(id, error));
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    runtime.Shutdown();
    runtime.Shutdown();
    EXPECT_FALSE(runtime.IsInitialized());
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    EXPECT_FALSE(runtime.StartInstance(id, error));
    EXPECT_EQ(logs.errors.size(), 2u);
}

TEST(ScriptSubsystem, UpdatesInstancesAndSkipsFaultedOnesWithoutCallingGlobalUpdate) {
    ScriptFixture fixture;
    const auto failing = fixture.Write(R"(
        return { OnUpdate = function(self, dt)
            stats.bad = stats.bad + 1
            error('one instance failed')
        end }
    )", "failing.lua");
    const auto healthy = fixture.Write(R"(
        return { OnUpdate = function(self, dt)
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
    LogCapture logs;
    const auto desc = fixture.Write(R"(
        return {
            OnStart = function(self)
                stats.starts = stats.starts + 1
                error('start failed')
            end,
            OnDestroy = function(self) stats.destroys = stats.destroys + 1 end
        }
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {starts = 0, destroys = 0}", "setup", error)) << error;
    engine::InstanceId unstarted, failed;
    ASSERT_TRUE(runtime.CreateInstance(desc, unstarted, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(unstarted, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(desc, failed, error)) << error;
    EXPECT_FALSE(runtime.StartInstance(failed, error));
    EXPECT_NE(error.find("start failed"), std::string::npos);
    EXPECT_FALSE(runtime.StartInstance(failed, error));
    EXPECT_FALSE(runtime.UpdateInstance(failed, 0.1, error));
    EXPECT_TRUE(runtime.DestroyInstance(failed, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.starts == 1 and stats.destroys == 0)", "check", error)) << error;
    EXPECT_EQ(logs.errors.size(), 1u);
}

TEST(LuaInstances, ReexecutesChunkAndKeepsNestedDefaultsIndependentForEachObject) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        assert(type(Log.info) == 'function') -- Shared engine API remains available.
        privateLoads = (privateLoads or 0) + 1
        assert(privateLoads == 1)
        stats.loads = stats.loads + 1
        local index = stats.loads
        return {
            defaults = {nested = {ticks = 0}, items = {'seed'}},
            OnStart = function(self, ...)
                assert(select('#', ...) == 0)
                assert(self.defaults.nested.ticks == 0 and #self.defaults.items == 1)
                stats.tables[index] = self
            end,
            OnUpdate = function(self, dt, ...)
                assert(select('#', ...) == 0)
                assert(self == stats.tables[index])
                assert(dt == 0.25 or dt == 0.5)
                self.defaults.nested.ticks = self.defaults.nested.ticks + 1
                table.insert(self.defaults.items, dt)
            end,
            OnDestroy = function(self, ...)
                assert(select('#', ...) == 0)
                assert(self == stats.tables[index])
                stats.destroyed[index] = true
            end
        }
    )");
    auto secondObject = desc;
    secondObject.entityId = engine::NewUuid();
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {loads = 0, tables = {}, destroyed = {}}", "setup", error)) << error;
    engine::InstanceId first, second;
    ASSERT_TRUE(runtime.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(secondObject, second, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(first, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(first, 0.25, error)) << error;
    // The second object's initial nested fields must survive mutations of the first.
    ASSERT_TRUE(runtime.StartInstance(second, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(second, 0.25, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(first, 0.5, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString(R"(
        assert(stats.loads == 2)
        local a, b = stats.tables[1], stats.tables[2]
        assert(not rawequal(a, b))
        assert(not rawequal(a.defaults, b.defaults))
        assert(not rawequal(a.defaults.nested, b.defaults.nested))
        assert(not rawequal(a.defaults.items, b.defaults.items))
        assert(a.defaults.nested.ticks == 2 and b.defaults.nested.ticks == 1)
        assert(#a.defaults.items == 3 and #b.defaults.items == 2)
        assert(privateLoads == nil)
    )", "check", error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(first, error)) << error;
    ASSERT_TRUE(runtime.UpdateInstance(second, 0.5, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(second, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.destroyed[1] and stats.destroyed[2])", "destroyed", error)) << error;
}

TEST(LuaInstances, RejectsWrongTypeForEveryOnCallbackWithUsefulDiagnostics) {
    ScriptFixture fixture;
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    for (const std::string name : {"OnStart", "OnUpdate", "OnDestroy"}) {
        const std::string filename = name + ".lua";
        const auto desc = fixture.Write("return { " + name + " = false }", filename.c_str());
        engine::InstanceId id;
        EXPECT_FALSE(runtime.CreateInstance(desc, id, error));
        EXPECT_FALSE(id.IsValid());
        EXPECT_NE(error.find(name + " must be a function or nil"), std::string::npos);
        EXPECT_NE(error.find(desc.assetId.ToString()), std::string::npos);
        EXPECT_NE(error.find(desc.entityId), std::string::npos);
    }
}

TEST(LuaInstances, MissingOnCallbacksAreAllowedAndDoNotUseGlobalsOrOldFieldNames) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        local function unexpected() error('not an instance callback') end
        return { Start = unexpected, Update = unexpected, Destroy = unexpected }
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString(R"(
        function OnStart() error('global OnStart') end
        function OnUpdate() error('global OnUpdate') end
        function OnDestroy() error('global OnDestroy') end
    )", "globals", error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_TRUE(runtime.UpdateInstance(id, 0.25, error)) << error;
    EXPECT_TRUE(runtime.DestroyInstance(id, error)) << error;
    EXPECT_TRUE(error.empty());
}

TEST(LuaInstances, CapturesProtectedCallbacksAtCreation) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        return {
            OnStart = function(self)
                stats.starts = stats.starts + 1
                self.OnUpdate = function() error('replacement update') end
                self.OnDestroy = function() error('replacement destroy') end
            end,
            OnUpdate = function(self, dt) stats.updates = stats.updates + 1 end,
            OnDestroy = function(self) stats.destroys = stats.destroys + 1 end
        }
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {starts = 0, updates = 0, destroys = 0}", "setup", error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    EXPECT_TRUE(runtime.UpdateInstance(id, 0.25, error)) << error;
    EXPECT_TRUE(runtime.DestroyInstance(id, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.starts == 1 and stats.updates == 1 and stats.destroys == 1)",
                                     "check", error)) << error;
}

TEST(LuaErrors, UnicodeSyntaxErrorPreservesFileLineAndIdentityAndLogsOnce) {
    ScriptFixture fixture;
    LogCapture logs;
    engine::ScriptInstanceDesc desc{
        fixture.root / u8"\u0441\u0438\u043d\u0442\u0430\u043a\u0441\u0438\u0441.lua",
        engine::NewUuid(), engine::NewUuid()};
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(desc.source, "-- header\nlocal = 1\n", error)) << error;
    engine::LuaRuntime runtime;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    engine::InstanceId id;
    EXPECT_FALSE(runtime.CreateInstance(desc, id, error));
    EXPECT_FALSE(id.IsValid());
    EXPECT_NE(error.find(engine::ToUtf8(desc.source)), std::string::npos);
    EXPECT_NE(error.find(engine::ToUtf8(desc.source.filename()) + ":2:"), std::string::npos);
    EXPECT_NE(error.find(desc.assetId.ToString()), std::string::npos);
    EXPECT_NE(error.find(desc.entityId), std::string::npos);
    EXPECT_NE(error.find("CreateInstance"), std::string::npos);
    ASSERT_EQ(logs.errors.size(), 1u);
    EXPECT_EQ(logs.errors[0], "[Lua] " + error);
    EXPECT_TRUE(runtime.ExecuteString("assert(1 + 1 == 2)", "still-running", error)) << error;
}

TEST(LuaErrors, UnicodeUpdateFailureIsLoggedOnceAndOnlyDisablesItsInstance) {
    ScriptFixture fixture;
    LogCapture logs;
    engine::ScriptInstanceDesc desc{
        fixture.root / u8"\u043e\u0431\u044a\u0435\u043a\u0442.lua",
        engine::NewUuid(), engine::NewUuid()};
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(desc.source,
        "return {\nOnUpdate = function(self, dt)\nerror('update failure marker')\nend,\n"
        "OnDestroy = function(self) stats.badDestroyed = true end\n}\n", error)) << error;
    const auto healthy = fixture.Write("return {OnUpdate = function(self, dt) stats.good = stats.good + 1 end}");
    engine::LuaRuntime runtime;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {good = 0}", "setup", error)) << error;
    engine::InstanceId bad, good;
    ASSERT_TRUE(runtime.CreateInstance(desc, bad, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(healthy, good, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(bad, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(good, error)) << error;
    EXPECT_FALSE(runtime.UpdateInstance(bad, 0.1, error));
    const std::string original = error;
    EXPECT_NE(error.find("OnUpdate"), std::string::npos);
    EXPECT_NE(error.find(engine::ToUtf8(desc.source)), std::string::npos);
    EXPECT_NE(error.find(engine::ToUtf8(desc.source.filename()) + ":3:"), std::string::npos);
    EXPECT_NE(error.find(desc.assetId.ToString()), std::string::npos);
    EXPECT_NE(error.find(desc.entityId), std::string::npos);
    EXPECT_NE(error.find("stack traceback:"), std::string::npos);
    for (int attempt = 0; attempt < 3; ++attempt) {
        EXPECT_FALSE(runtime.StartInstance(bad, error));
        EXPECT_EQ(error, original);
        EXPECT_FALSE(runtime.UpdateInstance(bad, 0.1, error));
        EXPECT_EQ(error, original);
        EXPECT_TRUE(runtime.UpdateInstance(good, 0.1, error)) << error;
    }
    EXPECT_TRUE(runtime.DestroyInstance(bad, error)) << error;
    EXPECT_TRUE(runtime.DestroyInstance(bad, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(stats.good == 3 and stats.badDestroyed == nil)", "check", error)) << error;
    runtime.Shutdown();
    ASSERT_EQ(logs.errors.size(), 1u);
    EXPECT_EQ(logs.errors[0], "[Lua] " + original);
}

TEST(LuaErrors, CompatibilityFileAndCallbacksUseProtectedCallsAndUtf8ChunkNames) {
    ScriptFixture fixture;
    LogCapture logs;
    const auto file = fixture.root / u8"\u0444\u0430\u0439\u043b.lua";
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(file,
        "function Legacy() error('legacy failure') end\nfunction Update(dt) error('update failure') end\n", error)) << error;
    engine::LuaRuntime runtime;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteFile(file, error)) << error;
    EXPECT_FALSE(runtime.CallFunction("Legacy", error));
    EXPECT_NE(error.find(engine::ToUtf8(file.filename()) + ":1:"), std::string::npos);
    EXPECT_FALSE(runtime.CallUpdate(0.1, error));
    EXPECT_NE(error.find(engine::ToUtf8(file.filename()) + ":2:"), std::string::npos);
    EXPECT_EQ(logs.errors.size(), 2u);
    const auto syntaxFile = fixture.root / u8"\u043e\u0448\u0438\u0431\u043a\u0430.lua";
    ASSERT_TRUE(engine::CreateNewFile(syntaxFile, "-- header\nlocal = 1\n", error)) << error;
    EXPECT_FALSE(runtime.ExecuteFile(syntaxFile, error));
    EXPECT_NE(error.find(engine::ToUtf8(syntaxFile)), std::string::npos);
    EXPECT_NE(error.find(engine::ToUtf8(syntaxFile.filename()) + ":2:"), std::string::npos);
    EXPECT_EQ(logs.errors.size(), 3u);
}

TEST(LuaInstances, DestroyReleasesTableEnvironmentAndCallbackReferencesBeforeState) {
    ScriptFixture fixture;
    const auto desc = fixture.Write(R"(
        local behavior = {}
        behavior.OnStart = function(self)
            selfReference = self -- Environment/table cycle should be collectable after destroy.
            refs[1], refs[2] = self, _ENV
        end
        behavior.OnDestroy = function(self) stats.destroys = stats.destroys + 1 end
        return behavior
    )");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("refs = setmetatable({}, {__mode = 'v'}); stats = {destroys = 0}",
                                     "setup", error)) << error;
    engine::InstanceId id;
    ASSERT_TRUE(runtime.CreateInstance(desc, id, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(id, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(id, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(id, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString(R"(
        collectgarbage('collect')
        assert(refs[1] == nil and refs[2] == nil)
        assert(stats.destroys == 1)
    )", "released", error)) << error;
}

TEST(ScriptSubsystem, EndPlayDestroysHealthyInstancesOnceAndRejectsReusedSlotsFromOldSessions) {
    ScriptFixture fixture;
    LogCapture logs;
    const auto desc = fixture.Write("return { OnDestroy = function(self) Log.info('session cleanup marker') end }");
    engine::ScriptSubsystem subsystem;
    std::string error;
    ASSERT_TRUE(subsystem.Initialize());
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    engine::InstanceId old;
    ASSERT_TRUE(subsystem.GetRuntime().CreateInstance(desc, old, error)) << error;
    ASSERT_TRUE(subsystem.GetRuntime().StartInstance(old, error)) << error;
    subsystem.EndPlay();
    subsystem.EndPlay();
    EXPECT_FALSE(subsystem.GetRuntime().DestroyInstance(old, error));
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    engine::InstanceId fresh;
    // The first local slot is reused, but the session token must differ.
    ASSERT_TRUE(subsystem.GetRuntime().CreateInstance(desc, fresh, error)) << error;
    EXPECT_NE(fresh, old);
    EXPECT_FALSE(subsystem.GetRuntime().StartInstance(old, error));
    EXPECT_FALSE(subsystem.GetRuntime().UpdateInstance(old, 0.1, error));
    EXPECT_FALSE(subsystem.GetRuntime().DestroyInstance(old, error));
    ASSERT_TRUE(subsystem.GetRuntime().StartInstance(fresh, error)) << error;
    subsystem.Shutdown();
    subsystem.Shutdown();
    EXPECT_EQ(std::count(logs.messages.begin(), logs.messages.end(), "[Lua] session cleanup marker"), 2);
    EXPECT_TRUE(logs.errors.empty());
}

TEST(LuaModules, ResolvesProjectRootWithUnicodePathsAndDifferentWorkingDirectory) {
    ScriptFixture fixture;
    const auto project = fixture.root / u8"\u043f\u0440\u043e\u0435\u043a\u0442";
    fixture.File(project / "shared/settings.lua", "return {value = 42}");
    fixture.File(project / "shared/settings/init.lua", "return {value = -1}");
    fixture.File(project / "shared/folder/init.lua", "return {value = 7}");
    const auto elsewhere = fixture.root / "elsewhere";
    fixture.File(elsewhere / "shared/settings.lua", "return {value = -2}");
    GameRootScope gameRoot(project);
    CurrentDirectoryScope workingDirectory(elsewhere);
    engine::ScriptSubsystem subsystem;
    std::string error;
    ASSERT_TRUE(subsystem.Initialize());
    ASSERT_TRUE(subsystem.BeginPlay(engine::AssetPathResolver::GetGameRoot(), error)) << error;
    EXPECT_TRUE(subsystem.GetRuntime().ExecuteString(R"(
        assert(require('shared.settings').value == 42)
        assert(require('shared.folder').value == 7)
        assert(package == nil and dofile == nil and loadfile == nil)
    )", "project-root", error)) << error;
}

TEST(LuaModules, InstancesShareModulesButNotSelfAndStopClearsModuleCache) {
    ScriptFixture fixture;
    fixture.File("shared/settings.lua", "stats.loads = stats.loads + 1; return {value = 0}");
    const auto desc = fixture.Write(R"(
        local settings = require('shared.settings')
        return {
            nested = {value = 0},
            OnStart = function(self)
                self.nested.value = self.nested.value + 1
                settings.value = settings.value + 1
                table.insert(stats.selves, self)
                table.insert(stats.modules, settings)
            end,
            OnDestroy = function(self)
                assert(require('shared.settings') == settings)
            end
        }
    )");
    engine::ScriptSubsystem subsystem;
    std::string error;
    ASSERT_TRUE(subsystem.Initialize());
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    auto& runtime = subsystem.GetRuntime();
    ASSERT_TRUE(runtime.ExecuteString("stats = {loads = 0, selves = {}, modules = {}}", "setup", error)) << error;
    engine::InstanceId first, second;
    ASSERT_TRUE(runtime.CreateInstance(desc, first, error)) << error;
    ASSERT_TRUE(runtime.CreateInstance(desc, second, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(first, error)) << error;
    ASSERT_TRUE(runtime.StartInstance(second, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString(R"(
        assert(stats.loads == 1)
        assert(stats.modules[1] == stats.modules[2] and stats.modules[1].value == 2)
        assert(stats.selves[1] ~= stats.selves[2])
        assert(stats.selves[1].nested ~= stats.selves[2].nested)
        assert(stats.selves[1].nested.value == 1 and stats.selves[2].nested.value == 1)
    )", "shared-module", error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(first, error)) << error;
    ASSERT_TRUE(runtime.DestroyInstance(second, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(require('shared.settings') == stats.modules[1])", "cache-lifetime", error)) << error;
    subsystem.EndPlay();
    ASSERT_TRUE(subsystem.BeginPlay(fixture.root, error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("stats = {loads = 0}", "new-session", error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(require('shared.settings').value == 0 and stats.loads == 1)",
                                     "fresh-module", error)) << error;
}

TEST(LuaModules, RejectsInvalidNamesAndDoesNotExposeNativeOrUnrestrictedFileLoading) {
    ScriptFixture fixture;
    fixture.File("native_only.dll", "not a Lua source file");
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(fixture.root, error)) << error;
    EXPECT_TRUE(runtime.ExecuteString(R"(
        for _, name in ipairs({
            '', '../outside', '/absolute', 'C:relative', 'C:/absolute',
            'a/b', 'a' .. string.char(92) .. 'b', string.rep(string.char(92), 2) .. 'server',
            '.a', 'a.', 'a..b', '..', 'a-b', 'a' .. string.char(0) .. 'b'
        }) do
            local ok, message = pcall(require, name)
            assert(not ok and string.find(message, 'invalid module name', 1, true))
        end
        assert(not pcall(require, {}))
        assert(not pcall(require, 123))
        assert(package == nil and loadfile == nil and dofile == nil)
        assert(not pcall(require, 'native_only'))
        assert(not pcall(require, 'os'))
        -- Defining a fake package table cannot reconfigure the C++ loader.
        package = {path = '../?.lua', cpath = '../?.dll'}
        assert(not pcall(require, 'missing'))
    )", "restricted-loader", error)) << error;
}

TEST(LuaModules, DetectsCyclesAndClearsLoadingMarkersAfterFailures) {
    ScriptFixture fixture;
    fixture.File("a.lua", "if not allowCycleRecovery then require('b') end; return {recovered = true}");
    fixture.File("b.lua", "return require('a')");
    engine::LuaRuntime runtime;
    LogCapture logs;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(fixture.root, error)) << error;
    EXPECT_FALSE(runtime.ExecuteString("require('a')", "cycle", error));
    EXPECT_NE(error.find("cyclic require: a -> b -> a"), std::string::npos);
    EXPECT_EQ(logs.errors.size(), 1u);
    ASSERT_TRUE(runtime.ExecuteString("allowCycleRecovery = true", "repair", error)) << error;
    EXPECT_TRUE(runtime.ExecuteString("assert(require('a').recovered)", "retry", error)) << error;
}

TEST(LuaModules, ModuleLoadErrorsAreOrdinaryLuaErrorsAndPreserveSourceLocations) {
    ScriptFixture fixture;
    fixture.File("broken.lua", "-- header\nlocal = 1\n");
    fixture.File("failing.lua", "-- header\nerror('module execution failed')\n");
    engine::LuaRuntime runtime;
    LogCapture logs;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(fixture.root, error)) << error;
    EXPECT_FALSE(runtime.ExecuteString("require('broken')", "syntax-module", error));
    EXPECT_NE(error.find("broken.lua:2:"), std::string::npos);
    EXPECT_FALSE(runtime.ExecuteString("require('failing')", "failed-module", error));
    EXPECT_NE(error.find("failing.lua:2:"), std::string::npos);
    EXPECT_NE(error.find("module execution failed"), std::string::npos);
    EXPECT_TRUE(runtime.ExecuteString("assert(not pcall(require, 'missing')); assert(1 + 1 == 2)",
                                     "recoverable", error)) << error;
    EXPECT_EQ(logs.errors.size(), 2u);
}

TEST(LuaModules, CachesFalseAndNilResultsAndKeepsProjectsSeparate) {
    ScriptFixture firstProject;
    ScriptFixture secondProject;
    firstProject.File("settings.lua", "return {project = 1}");
    secondProject.File("settings.lua", "return {project = 2}");
    firstProject.File("empty.lua", "stats.empty = stats.empty + 1");
    firstProject.File("false_module.lua", "stats.falsy = stats.falsy + 1; return false");
    engine::LuaRuntime first, second;
    std::string error;
    ASSERT_TRUE(first.Initialize(firstProject.root, error)) << error;
    ASSERT_TRUE(second.Initialize(secondProject.root, error)) << error;
    EXPECT_TRUE(first.ExecuteString(R"(
        stats = {empty = 0, falsy = 0}
        assert(require('settings').project == 1)
        assert(require('empty') == true and require('empty') == true)
        assert(require('false_module') == false and require('false_module') == false)
        assert(stats.empty == 1 and stats.falsy == 1)
    )", "first-project", error)) << error;
    EXPECT_TRUE(second.ExecuteString("assert(require('settings').project == 2)", "second-project", error)) << error;
    EXPECT_FALSE(first.Initialize(secondProject.root, error));
    EXPECT_TRUE(first.ExecuteString("assert(require('settings').project == 1)", "pinned-root", error)) << error;
}

TEST(LuaModules, RejectsCanonicalSymlinkEscapeButAllowsLinksWithinRoot) {
    ScriptFixture fixture;
    const auto root = fixture.root / "content";
    const auto neighbor = fixture.root / "content-neighbor";
    fixture.File(neighbor / "secret.lua", "return {outside = true}");
    fixture.File(root / "inside/settings.lua", "return {value = 42}");
    std::error_code ec;
    std::filesystem::create_directory_symlink(neighbor, root / "outside", ec);
    if (ec) GTEST_SKIP() << "Directory symlinks are unavailable: " << ec.message();
    std::filesystem::create_directory_symlink(root / "inside", root / "alias", ec);
    ASSERT_FALSE(ec) << ec.message();
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(root, error)) << error;
    EXPECT_FALSE(runtime.ExecuteString("require('outside.secret')", "escape", error));
    EXPECT_NE(error.find("escapes project content root"), std::string::npos);
    EXPECT_TRUE(runtime.ExecuteString("assert(require('alias.settings').value == 42)", "internal-link", error)) << error;
}

#ifdef _WIN32
TEST(LuaModules, RejectsJunctionEscapeButAllowsJunctionsWithinRoot) {
    ScriptFixture fixture;
    const auto root = fixture.root / "content";
    const auto neighbor = fixture.root / "content-neighbor";
    fixture.File(neighbor / "secret.lua", "return {outside = true}");
    fixture.File(root / "inside/settings.lua", "return {value = 42}");
    const auto outsideError = CreateTestJunction(root / "outside", neighbor);
    if (outsideError.value() == ERROR_ACCESS_DENIED || outsideError.value() == ERROR_PRIVILEGE_NOT_HELD) {
        GTEST_SKIP() << "Junction creation is restricted: " << outsideError.message();
    }
    ASSERT_FALSE(outsideError) << outsideError.message();
    const auto insideError = CreateTestJunction(root / "alias", root / "inside");
    ASSERT_FALSE(insideError) << insideError.message();
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(root, error)) << error;
    EXPECT_FALSE(runtime.ExecuteString("require('outside.secret')", "junction-escape", error));
    EXPECT_NE(error.find("escapes project content root"), std::string::npos);
    EXPECT_TRUE(runtime.ExecuteString("assert(require('alias.settings').value == 42)", "internal-junction", error)) << error;
}
#endif
