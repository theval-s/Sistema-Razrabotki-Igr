#include <engine/assets/AssetEvents.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <TaskScheduler.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

namespace {
using namespace engine;
constexpr auto ValidMaterial = R"({"version":1,"hooks":[]})";
constexpr auto BadName = "game/materials/bad.material.json";
constexpr auto GoodName = "game/materials/good.material.json";
constexpr auto ReloadName = "game/materials/reload.material.json";

// Exercise the actual two Engine-owned AssetSystems and dispatcher, with just
// their kernel dependencies initialized. An optional uninitialized rendering
// subsystem supplies its real resource manager for hook-free materials: these
// have no GPU allocation and need neither a window/device nor deployed shaders.
struct AssetRuntime {
    Engine engine;
    explicit AssetRuntime(bool materials = false) {
        if (materials) engine.AddSubsystem<RenderingSubsystem>();
        enki::TaskSchedulerConfig config;
        config.numTaskThreadsToCreate = 1;
        engine.GetTaskScheduler().Initialize(config);
        engine.GetAssets().Initialize();
        engine.GetSceneAssets().Initialize();
    }
    ~AssetRuntime() {
        engine.GetAssets().Shutdown();
        engine.GetSceneAssets().Shutdown();
        engine.GetSceneAssets().UnloadAll();
        engine.GetAssets().UnloadAll();
        engine.GetTaskScheduler().WaitforAllAndShutdown();
    }
};

struct Capture {
    std::vector<AssetLoadedEvent> loaded;
    std::vector<AssetFailedEvent> failed;
    std::function<void(const AssetFailedEvent&)> onFailure;
    std::function<void(const AssetLoadedEvent&)> onSuccess;
    entt::scoped_connection loadedConnection;
    entt::scoped_connection failedConnection;

    explicit Capture(Engine& engine) {
        loadedConnection = engine.GetEvents().sink<AssetLoadedEvent>().connect<&Capture::Loaded>(*this);
        failedConnection = engine.GetEvents().sink<AssetFailedEvent>().connect<&Capture::Failed>(*this);
    }
    void Loaded(const AssetLoadedEvent& event) {
        loaded.push_back(event);
        if (onSuccess) onSuccess(event);
    }
    void Failed(const AssetFailedEvent& event) {
        failed.push_back(event);
        if (onFailure) onFailure(event);
    }
};

template<class Event, class T>
void ExpectIdentity(const Event& event, const AssetSystem& source, AssetHandle<T> handle,
                    std::string_view name) {
    EXPECT_EQ(event.source, &source);
    EXPECT_EQ(event.kind, AssetValueKind<T>::value);
    EXPECT_EQ(event.slot, handle.slot);
    EXPECT_EQ(event.generation, handle.generation);
    EXPECT_EQ(event.name, name);
    EXPECT_EQ(event.id, ToResourceID(name));
}

class AssetEvents : public ::testing::Test {
protected:
    std::filesystem::path previousGameRoot;
    std::filesystem::path root;

    void Write(std::string_view file, std::string_view contents) {
        std::ofstream output(root / "materials" / file, std::ios::binary | std::ios::trunc);
        output << contents;
        output.close();
        ASSERT_TRUE(output) << "Cannot write temporary asset fixture";
    }
    void SetUp() override {
        previousGameRoot = AssetPathResolver::GetGameRoot();
        root = std::filesystem::temp_directory_path() / ("sri-asset-events-" + NewUuid()) / "game";
        ASSERT_TRUE(std::filesystem::create_directories(root / "materials"));
        ASSERT_NO_FATAL_FAILURE(Write("bad.material.json", "{}"));
        ASSERT_NO_FATAL_FAILURE(Write("good.material.json", ValidMaterial));
        ASSERT_NO_FATAL_FAILURE(Write("reload.material.json", ValidMaterial));
        ASSERT_NO_FATAL_FAILURE(Write("example.material-instance.json",
            R"({"version":1,"material":"game/materials/good.material.json","parameters":{}})"));
        std::size_t generated = 0;
        std::string error;
        ASSERT_TRUE(GenerateMissingAssetMetadata(root, generated, error)) << error;
        ASSERT_EQ(generated, 4u);
        AssetPathResolver::SetGameRoot(root);
    }
    void TearDown() override {
        AssetPathResolver::SetGameRoot(previousGameRoot);
        std::error_code ignored;
        if (!root.empty()) std::filesystem::remove_all(root.parent_path(), ignored);
    }
    void Settle(AssetSystem& assets, AssetHandle<MaterialAsset> handle) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        FrameContext frame;
        while (assets.GetState(handle) == AssetState::Loading && std::chrono::steady_clock::now() < until) {
            assets.Update(frame);
            ++frame.frameIndex;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_NE(assets.GetState(handle), AssetState::Loading) << "Asset worker did not settle";
    }
};

TEST_F(AssetEvents, AsyncFailureDistinguishesEqualNamesAndHandlesInBothSystems) {
    AssetRuntime runtime;
    auto& global = runtime.engine.GetAssets();
    auto& scene = runtime.engine.GetSceneAssets();
    Capture events(runtime.engine);
    const auto globalHandle = global.RequestMaterial(BadName);
    const auto sceneHandle = scene.RequestMaterial(BadName);
    ASSERT_TRUE(globalHandle && sceneHandle);
    ASSERT_EQ(globalHandle, sceneHandle); // Only source can distinguish these attempts.
    events.onFailure = [&](const AssetFailedEvent& event) {
        const auto handle = AssetHandle<MaterialAsset>{event.slot, event.generation};
        auto& source = event.source == &global ? global : scene;
        EXPECT_EQ(source.GetState(handle), AssetState::Failed);
        EXPECT_FALSE(source.Peek(handle));
    };
    ASSERT_NO_FATAL_FAILURE(Settle(global, globalHandle));
    ASSERT_NO_FATAL_FAILURE(Settle(scene, sceneHandle));
    ASSERT_EQ(events.failed.size(), 2u);
    ExpectIdentity(events.failed[0], global, globalHandle, BadName);
    ExpectIdentity(events.failed[1], scene, sceneHandle, BadName);
    EXPECT_FALSE(events.failed[0].error.empty());
    EXPECT_FALSE(events.failed[1].error.empty());
    EXPECT_TRUE(events.loaded.empty());
    EXPECT_FALSE(global.Peek(globalHandle));
    EXPECT_FALSE(scene.Peek(sceneHandle));
}

TEST_F(AssetEvents, BlockingFailurePublishesOriginalRetiredHandleInBothSystems) {
    AssetRuntime runtime;
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        const auto handle = assets->RequestMaterial(BadName);
        ASSERT_TRUE(handle);
        EXPECT_FALSE(assets->LoadMaterial(BadName));
        ASSERT_FALSE(events.failed.empty());
        ExpectIdentity(events.failed.back(), *assets, handle, BadName);
        EXPECT_EQ(assets->GetState(handle), AssetState::Failed);
        EXPECT_FALSE(assets->Peek(handle));
        const auto retry = assets->RequestMaterial(BadName);
        EXPECT_EQ(retry.slot, handle.slot);
        EXPECT_NE(retry.generation, handle.generation);
        // The copied event still identifies the old attempt after slot reuse.
        ExpectIdentity(events.failed.back(), *assets, handle, BadName);
    }
    EXPECT_EQ(events.failed.size(), 2u);
    EXPECT_TRUE(events.loaded.empty());
}

TEST_F(AssetEvents, GpuFailureAlsoPublishesTheOriginalHandle) {
    AssetRuntime runtime; // Valid CPU material, but no RenderingSubsystem to upload it.
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        const auto handle = assets->RequestMaterial(GoodName);
        EXPECT_FALSE(assets->LoadMaterial(GoodName));
        ASSERT_FALSE(events.failed.empty());
        ExpectIdentity(events.failed.back(), *assets, handle, GoodName);
        EXPECT_NE(events.failed.back().error.find("no RenderingSubsystem"), std::string::npos);
        EXPECT_EQ(assets->GetState(handle), AssetState::Failed);
    }
    EXPECT_EQ(events.failed.size(), 2u);
}

TEST_F(AssetEvents, MissingJobFailureIsCorrelatedAndDeliveredAfterRetirement) {
    AssetRuntime runtime;
    Capture events(runtime.engine);
    events.onFailure = [&](const AssetFailedEvent& event) {
        auto& assets = event.source == &runtime.engine.GetAssets()
            ? runtime.engine.GetAssets() : runtime.engine.GetSceneAssets();
        EXPECT_EQ(assets.GetState(AssetHandle<MaterialAsset>{event.slot, event.generation}), AssetState::Failed);
    };
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        const auto handle = assets->RequestMaterial(BadName);
        // Public cancellation drains the job but leaves its Loading record. This
        // deterministically exercises LoadSync's missing-job branch.
        assets->Shutdown();
        EXPECT_FALSE(assets->LoadMaterial(BadName));
        ASSERT_FALSE(events.failed.empty());
        ExpectIdentity(events.failed.back(), *assets, handle, BadName);
        EXPECT_EQ(events.failed.back().error, "load job disappeared from the scheduler");
        EXPECT_EQ(assets->GetState(handle), AssetState::Failed);
        assets->Update(FrameContext{});
    }
    EXPECT_EQ(events.failed.size(), 2u);
    EXPECT_TRUE(events.loaded.empty());
}

TEST_F(AssetEvents, SynchronousRetryListenerCanRecycleSlotWithoutChangingOuterEvent) {
    AssetRuntime runtime;
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        ASSERT_NO_FATAL_FAILURE(Write("bad.material.json", "{}"));
        events.failed.clear();
        const auto original = assets->RequestMaterial(BadName);
        AssetHandle<MaterialAsset> retry;
        bool retried = false;
        events.onFailure = [&](const AssetFailedEvent& event) {
            ASSERT_EQ(event.source, assets);
            if (retried) return;
            retried = true;
            const auto snapshot = event;
            ExpectIdentity(event, *assets, original, BadName);
            EXPECT_EQ(assets->GetState(original), AssetState::Failed);
            ASSERT_NO_FATAL_FAILURE(Write("bad.material.json", ValidMaterial));
            retry = assets->RequestMaterial(BadName);
            EXPECT_EQ(retry.slot, original.slot);
            EXPECT_NE(retry.generation, original.generation);
            // Nested synchronous completion retires the recycled record too.
            EXPECT_FALSE(assets->LoadMaterial(BadName));
            ExpectIdentity(event, *assets, original, BadName);
            EXPECT_EQ(event.error, snapshot.error);
            EXPECT_EQ(event.name, snapshot.name);
        };
        EXPECT_FALSE(assets->LoadMaterial(BadName));
        events.onFailure = {};
        ASSERT_TRUE(retried);
        ASSERT_EQ(events.failed.size(), 2u);
        ExpectIdentity(events.failed[0], *assets, original, BadName);
        ExpectIdentity(events.failed[1], *assets, retry, BadName);
        EXPECT_NE(events.failed[1].error.find("no RenderingSubsystem"), std::string::npos);
        EXPECT_NE(events.failed[0].error, events.failed[1].error);
        EXPECT_EQ(assets->GetState(original), AssetState::Failed);
        EXPECT_EQ(assets->GetState(retry), AssetState::Failed);
    }
}

TEST_F(AssetEvents, SuccessPublishesReadyBeforeDispatchAndDistinguishesBothSystems) {
    AssetRuntime runtime(true);
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        const auto handle = assets->RequestMaterial(GoodName);
        events.onSuccess = [&](const AssetLoadedEvent& event) {
            ExpectIdentity(event, *assets, handle, GoodName);
            EXPECT_TRUE(assets->IsReady(handle));
            EXPECT_TRUE(assets->Peek(handle));
        };
        ASSERT_NO_FATAL_FAILURE(Settle(*assets, handle));
        events.onSuccess = {};
        EXPECT_TRUE(assets->IsReady(handle));
        ASSERT_FALSE(events.loaded.empty());
        ExpectIdentity(events.loaded.back(), *assets, handle, GoodName);
    }
    ASSERT_EQ(events.loaded.size(), 2u);
    EXPECT_NE(events.loaded[0].source, events.loaded[1].source);
    EXPECT_EQ(events.loaded[0].slot, events.loaded[1].slot);
    EXPECT_EQ(events.loaded[0].generation, events.loaded[1].generation);
    EXPECT_TRUE(events.failed.empty());
}

TEST_F(AssetEvents, ReloadUsesExistingNonzeroSlotAndGenerationAndFailureRetainsReadyValue) {
    AssetRuntime runtime(true);
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        ASSERT_NO_FATAL_FAILURE(Write("reload.material.json", ValidMaterial));
        ASSERT_TRUE(assets->LoadMaterial(GoodName)); // Retain slot zero.
        const auto failed = assets->RequestMaterial(BadName); // Retire slot one.
        ASSERT_FALSE(assets->LoadMaterial(BadName));
        const auto handle = assets->RequestMaterial(ReloadName);
        ASSERT_NE(handle.slot, 0u);
        ASSERT_NE(handle.generation, 0u);
        ASSERT_EQ(handle.slot, failed.slot);
        ASSERT_TRUE(assets->LoadMaterial(ReloadName));
        const auto value = assets->Peek(handle);
        ASSERT_TRUE(value);
        const auto oldMaterial = value->material;
        auto successes = events.loaded.size();
        auto failures = events.failed.size();
        std::string error;
        ASSERT_TRUE(assets->Reload(ReloadName, AssetKind::Material, error)) << error;
        ASSERT_EQ(events.loaded.size(), successes + 1);
        ExpectIdentity(events.loaded.back(), *assets, handle, ReloadName);
        EXPECT_EQ(events.failed.size(), failures);
        EXPECT_TRUE(assets->IsReady(handle));
        EXPECT_EQ(assets->Peek(handle)->material, oldMaterial);

        ASSERT_NO_FATAL_FAILURE(Write("reload.material.json", "{}"));
        successes = events.loaded.size();
        EXPECT_FALSE(assets->Reload(ReloadName, AssetKind::Material, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(events.loaded.size(), successes);
        EXPECT_EQ(events.failed.size(), failures);
        EXPECT_TRUE(assets->IsReady(handle));
        EXPECT_EQ(assets->Peek(handle)->material, oldMaterial);
    }
}

TEST_F(AssetEvents, RejectedOrNoOpReloadDoesNotInventCompletionEvents) {
    AssetRuntime runtime;
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        std::string error;
        EXPECT_TRUE(assets->Reload(GoodName, AssetKind::Material, error));
        EXPECT_FALSE(assets->Reload("game/missing.obj", AssetKind::Mesh, error));
    }
    EXPECT_TRUE(events.loaded.empty());
    EXPECT_TRUE(events.failed.empty());
}

TEST_F(AssetEvents, MaterialInstanceReloadUsesItsOwnHandleRatherThanItsParent) {
    AssetRuntime runtime(true);
    Capture events(runtime.engine);
    constexpr auto name = "game/materials/example.material-instance.json";
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        const auto handle = assets->RequestMaterialInstance(name);
        ASSERT_TRUE(assets->LoadMaterialInstance(name));
        ASSERT_FALSE(events.loaded.empty());
        ExpectIdentity(events.loaded.back(), *assets, handle, name);
        const auto successes = events.loaded.size();
        const auto failures = events.failed.size();
        std::string error;
        ASSERT_TRUE(assets->Reload(name, AssetKind::MaterialInstance, error)) << error;
        ASSERT_EQ(events.loaded.size(), successes + 1);
        ExpectIdentity(events.loaded.back(), *assets, handle, name);
        EXPECT_EQ(events.failed.size(), failures);
        EXPECT_TRUE(assets->IsReady(handle));
    }
}

TEST_F(AssetEvents, ReloadSettlesInitialLoadingAttemptThenPublishesMatchingReloadSuccess) {
    AssetRuntime runtime(true);
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        ASSERT_TRUE(assets->LoadMaterial(GoodName));
        const auto handle = assets->RequestMaterial(ReloadName);
        ASSERT_EQ(assets->GetState(handle), AssetState::Loading);
        const auto successes = events.loaded.size();
        std::string error;
        ASSERT_TRUE(assets->Reload(ReloadName, AssetKind::Material, error)) << error;
        ASSERT_EQ(events.loaded.size(), successes + 2);
        ExpectIdentity(events.loaded[successes], *assets, handle, ReloadName);
        ExpectIdentity(events.loaded[successes + 1], *assets, handle, ReloadName);
        EXPECT_TRUE(assets->IsReady(handle));
    }
    EXPECT_TRUE(events.failed.empty());
}

TEST_F(AssetEvents, FailedInitialReloadLoadPublishesOneFailureRatherThanDuplicatingIt) {
    AssetRuntime runtime;
    Capture events(runtime.engine);
    for (auto* assets : {&runtime.engine.GetAssets(), &runtime.engine.GetSceneAssets()}) {
        const auto handle = assets->RequestMaterial(BadName);
        const auto failures = events.failed.size();
        std::string error;
        EXPECT_FALSE(assets->Reload(BadName, AssetKind::Material, error));
        ASSERT_EQ(events.failed.size(), failures + 1);
        ExpectIdentity(events.failed.back(), *assets, handle, BadName);
        EXPECT_EQ(assets->GetState(handle), AssetState::Failed);
    }
    EXPECT_TRUE(events.loaded.empty());
}
} // namespace
