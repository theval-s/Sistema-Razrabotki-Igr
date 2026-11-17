#include <editor/rendering_status.hpp>
#include <editor/log_model.hpp>
#include <editor/log_panel.hpp>
#include <editor/shortcut_registry.hpp>
#include <engine/engine.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <editor/paths.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QFile>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <chrono>
#include <limits>
#include <thread>

namespace {
const engine::RenderingDiagnostics Ready{engine::RenderingLifecycle::Ready, std::nullopt};

TEST(EditorRenderingStatus, FailedStartupKeepsEveryStructuredFieldWithoutATick) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    engine::RenderingFailure failure;
    failure.summary = "Required shader failed";
    failure.operation = "D3DCompile";
    failure.pass = "GBufferPass";
    failure.path = std::filesystem::path(L"C:/shaders/\u6d4b\u8bd5.hlsl");
    failure.detail = "include could not be read\nfull compiler output";
    failure.nativeError = -2147467259;
    failure.deviceRemovalReason = -2005270523;
    failure.shader = engine::ShaderDiagnosticContext{"pixel", "mainPS", "ps_5_0", {{"SKINNED", "1"}}};
    const auto result = status.describe({engine::RenderingLifecycle::Failed, failure}, true, false, 0);
    EXPECT_TRUE(result.warning);
    EXPECT_TRUE(result.summary.contains(QStringLiteral("GBufferPass")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("D3DCompile")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("\u6d4b\u8bd5.hlsl")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("0x80004005")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("Device removal reason:")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("mainPS | target ps_5_0")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("SKINNED=1")));
    EXPECT_TRUE(result.detail.endsWith(QStringLiteral("full compiler output")));
    const auto again = status.describe({engine::RenderingLifecycle::Failed, failure}, true, true, 10);
    EXPECT_EQ(again.detail, result.detail);
}

TEST(EditorRenderingStatus, EmptySceneAndCameraFallbackAreNonfatalAndCountsAreSubmissionOnly) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    const auto empty = status.describe(Ready, false, false, 0);
    EXPECT_FALSE(empty.warning);
    EXPECT_TRUE(empty.summary.contains(QStringLiteral("No drawable scene items submitted")));
    EXPECT_TRUE(empty.detail.contains(QStringLiteral("Edit uses the editor camera")));
    const auto playing = status.describe(Ready, false, true, 3);
    EXPECT_FALSE(playing.warning);
    EXPECT_TRUE(playing.detail.contains(QStringLiteral("keeps the current RenderWorld camera")));
    EXPECT_TRUE(playing.detail.contains(QStringLiteral("3 submitted drawable items")));
    EXPECT_TRUE(playing.detail.contains(QStringLiteral("does not measure on-screen visibility")));
    const auto camera = engine.GetScene().CreateEntity("Camera");
    engine.GetScene().AddComponent<engine::Camera>(camera);
    engine.GetScene().SetActiveCamera(camera);
    EXPECT_TRUE(status.describe(Ready, false, true, 3).summary.isEmpty());
}

TEST(EditorRenderingStatus, OnlyCurrentRendererReferencesRetainEventsAndRemovalClearsDetails) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    auto& scene = engine.GetScene();
    const auto entity = scene.CreateEntity("Broken mesh");
    engine::MeshRenderer renderer;
    renderer.mesh = {3, 1};
    renderer.meshReference = {engine::AssetUuid("11111111-1111-4111-8111-111111111111"), "game/models/broken.obj"};
    renderer.materialReference.path = "game/materials/broken.material-instance.json";
    scene.AddComponent<engine::MeshRenderer>(entity, renderer);
    engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/models/unrelated.obj", engine::AssetKind::Mesh,
        "unrelated failure", &engine.GetSceneAssets(), 4, 1});
    engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/models/broken.obj", engine::AssetKind::Mesh,
        "specific mesh import cause", &engine.GetSceneAssets(), renderer.mesh.slot, renderer.mesh.generation});
    const auto failed = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(failed.warning);
    EXPECT_TRUE(failed.detail.contains(QStringLiteral("specific mesh import cause")));
    EXPECT_FALSE(failed.detail.contains(QStringLiteral("unrelated failure")));
    EXPECT_TRUE(failed.detail.contains(QStringLiteral("UUID is not resolved in the catalog")));

    // Removal and immediate re-add with the very same handle/name must not retain
    // details from the removed component, even without a refresh between them.
    scene.RemoveComponent<engine::MeshRenderer>(entity);
    scene.AddComponent<engine::MeshRenderer>(entity, renderer);
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("specific mesh import cause")));
    scene.Clear();
    EXPECT_FALSE(status.describe(Ready, false, false, 0).warning);
}

TEST(EditorRenderingStatus, RetryGenerationAndSceneReplacementCannotKeepOldFailureText) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    auto& scene = engine.GetScene();
    const auto entity = scene.CreateEntity("Skinned");
    engine::SkinnedMeshRenderer renderer;
    renderer.mesh = {5, 1}; // Retired handles read Failed; no GPU or file fixture needed.
    renderer.meshReference.path = "game/rigs/character.skinnedmesh";
    scene.AddComponent<engine::SkinnedMeshRenderer>(entity, renderer);
    engine.GetEvents().trigger(engine::AssetFailedEvent{0, renderer.meshReference.path, engine::AssetKind::SkinnedMesh,
        "old attempt", &engine.GetSceneAssets(), renderer.mesh.slot, renderer.mesh.generation});
    EXPECT_TRUE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("old attempt")));
    scene.Modify<engine::SkinnedMeshRenderer>(entity, [](auto& component) { ++component.mesh.generation; });
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("old attempt")));
    scene.Clear();
    const auto replacement = scene.CreateEntity("Replacement");
    scene.AddComponent<engine::SkinnedMeshRenderer>(replacement, renderer);
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("old attempt")));
}

TEST(EditorRenderingStatus, ExactAttemptIgnoresGlobalLegacyWrongKindAndWrongGenerationEvents) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    auto& scene = engine.GetScene();
    const auto entity = scene.CreateEntity("Handle only");
    engine::MeshRenderer renderer;
    renderer.mesh = {7, 2};
    scene.AddComponent<engine::MeshRenderer>(entity, renderer);
    const auto publish = [&](const engine::AssetSystem* source, engine::AssetKind kind, std::uint32_t slot,
                          std::uint32_t generation, const char* cause) {
        engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/same-name.obj", kind, cause, source, slot, generation});
    };
    publish(&engine.GetAssets(), engine::AssetKind::Mesh, 7, 2, "global failure must not leak");
    publish(nullptr, engine::AssetKind::Mesh, 7, 2, "legacy failure must not leak");
    publish(&engine.GetSceneAssets(), engine::AssetKind::SkinnedMesh, 7, 2, "wrong kind must not leak");
    publish(&engine.GetSceneAssets(), engine::AssetKind::Mesh, 7, 1, "old generation must not leak");
    publish(&engine.GetSceneAssets(), engine::AssetKind::Mesh, std::numeric_limits<std::uint32_t>::max(), 2,
         "invalid slot must not leak");
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("must not leak")));
    publish(&engine.GetSceneAssets(), engine::AssetKind::Mesh, 7, 2, "original exact cause");
    // Equal-name global success/failure must neither clear nor overwrite the scene failure.
    publish(&engine.GetAssets(), engine::AssetKind::Mesh, 7, 2, "global replacement must not leak");
    engine.GetEvents().trigger(engine::AssetLoadedEvent{0, "game/same-name.obj", engine::AssetKind::Mesh,
        &engine.GetAssets(), 7, 2});
    engine.GetEvents().trigger(engine::AssetLoadedEvent{0, "game/same-name.obj", engine::AssetKind::Mesh,
        &engine.GetSceneAssets(), 7, 3}); // A new attempt is not this component's failed handle.
    const auto result = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(result.detail.contains(QStringLiteral("original exact cause")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("game/same-name.obj")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("slot 7, generation 2")));
    EXPECT_FALSE(result.detail.contains(QStringLiteral("must not leak")));
    EXPECT_EQ(result.logSearch, QStringLiteral("game/same-name.obj"));
}

TEST(EditorRenderingStatus, PreinstallFailuresAreClaimedThenUnreferencedEntriesExpireAtEventLoopBoundary) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/preinstall.obj", engine::AssetKind::Mesh,
        "synchronous original cause", &engine.GetSceneAssets(), 9, 3});
    const auto entity = engine.GetScene().CreateEntity("Installed after completion");
    engine::MeshRenderer renderer;
    renderer.mesh = {9, 3};
    engine.GetScene().AddComponent<engine::MeshRenderer>(entity, renderer);
    QCoreApplication::processEvents();
    EXPECT_TRUE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("synchronous original cause")));

    engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/not-referenced.obj", engine::AssetKind::Mesh,
        "unclaimed cause", &engine.GetSceneAssets(), 10, 3});
    QCoreApplication::processEvents();
    const auto late = engine.GetScene().CreateEntity("Too late to claim an unrelated request");
    renderer.mesh = {10, 3};
    engine.GetScene().AddComponent<engine::MeshRenderer>(late, renderer);
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("unclaimed cause")));
    engine.GetScene().Clear();
    const auto replacement = engine.GetScene().CreateEntity("Replacement");
    renderer.mesh = {9, 3};
    engine.GetScene().AddComponent<engine::MeshRenderer>(replacement, renderer);
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("synchronous original cause")));
}

TEST(EditorRenderingStatus, SharedFailureSurvivesRemovingOneUserButNotTheLastUser) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    auto& scene = engine.GetScene();
    engine::MeshRenderer renderer;
    renderer.mesh = {8, 4};
    const auto first = scene.CreateEntity("First");
    const auto second = scene.CreateEntity("Second");
    scene.AddComponent<engine::MeshRenderer>(first, renderer);
    scene.AddComponent<engine::MeshRenderer>(second, renderer);
    engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/shared.obj", engine::AssetKind::Mesh,
        "shared cause", &engine.GetSceneAssets(), 8, 4});
    scene.RemoveComponent<engine::MeshRenderer>(first);
    EXPECT_TRUE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("shared cause")));
    scene.RemoveComponent<engine::MeshRenderer>(second);
    scene.AddComponent<engine::MeshRenderer>(second, renderer);
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QStringLiteral("shared cause")));
}

TEST(EditorRenderingStatus, PreinstallCacheIsBoundedWithoutEvictingCurrentFailures) {
    engine::Engine engine;
    editor::RenderingStatus status(engine);
    auto& scene = engine.GetScene();
    engine::MeshRenderer renderer;
    renderer.mesh = {1, 1};
    scene.AddComponent<engine::MeshRenderer>(scene.CreateEntity("Current"), renderer);
    engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/current.obj", engine::AssetKind::Mesh,
        "current failure retained", &engine.GetSceneAssets(), 1, 1});
    for (std::uint32_t slot = 100; slot <= 356; ++slot) {
        engine.GetEvents().trigger(engine::AssetFailedEvent{0, "game/not-installed.obj", engine::AssetKind::Mesh,
            "preinstall cause " + std::to_string(slot), &engine.GetSceneAssets(), slot, 1});
    }
    renderer.mesh = {100, 1};
    scene.AddComponent<engine::MeshRenderer>(scene.CreateEntity("Oldest evicted"), renderer);
    renderer.mesh = {356, 1};
    scene.AddComponent<engine::MeshRenderer>(scene.CreateEntity("Newest retained"), renderer);
    const auto result = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(result.detail.contains(QStringLiteral("current failure retained")));
    EXPECT_FALSE(result.detail.contains(QStringLiteral("preinstall cause 100")));
    EXPECT_TRUE(result.detail.contains(QStringLiteral("preinstall cause 356")));
}

struct AssetStatusFixture {
    QTemporaryDir directory;
    std::filesystem::path previousRoot = engine::AssetPathResolver::GetGameRoot();
    AssetStatusFixture() { engine::AssetPathResolver::SetGameRoot(editor::toPath(directory.path())); }
    ~AssetStatusFixture() { engine::AssetPathResolver::SetGameRoot(previousRoot); }
};

struct AssetCompletions {
    std::vector<engine::AssetFailedEvent> failures;
    std::vector<engine::AssetLoadedEvent> loaded;
    void onFailure(const engine::AssetFailedEvent& event) { failures.push_back(event); }
    void onLoaded(const engine::AssetLoadedEvent& event) { loaded.push_back(event); }
};

template<class T>
void settle(engine::AssetSystem& assets, engine::AssetHandle<T> handle) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    engine::FrameContext frame;
    while (assets.GetState(handle) == engine::AssetState::Loading && std::chrono::steady_clock::now() < deadline) {
        assets.Update(frame);
        ++frame.frameIndex;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(assets.GetState(handle), engine::AssetState::Loading) << "Asset request did not settle";
}

TEST(EditorRenderingStatus, RealBlockingFailureBeforeComponentInstallationKeepsRetiredHandleCause) {
    AssetStatusFixture fixture;
    ASSERT_TRUE(fixture.directory.isValid());
    engine::Engine engine({.applicationName = "Blocking status fixture", .taskThreads = 2});
    editor::RenderingStatus status(engine); // Subscribe before LoadSync.
    ASSERT_TRUE(engine.Initialize()); // CPU failure needs no rendering subsystem/GPU.
    auto& assets = engine.GetSceneAssets();
    AssetCompletions events;
    entt::scoped_connection failed = engine.GetEvents().sink<engine::AssetFailedEvent>()
        .connect<&AssetCompletions::onFailure>(events);
    const auto handle = assets.RequestMesh("game/missing-before-install.obj");
    ASSERT_TRUE(handle);
    EXPECT_EQ(assets.GetState(handle), engine::AssetState::Loading);
    EXPECT_FALSE(assets.LoadMesh("game/missing-before-install.obj"));
    ASSERT_EQ(assets.GetState(handle), engine::AssetState::Failed);
    ASSERT_EQ(events.failures.size(), 1u);
    const auto& event = events.failures.front();
    EXPECT_EQ(event.source, &assets);
    EXPECT_EQ(event.slot, handle.slot);
    EXPECT_EQ(event.generation, handle.generation);
    EXPECT_FALSE(event.error.empty());
    EXPECT_FALSE(assets.ReferenceFor(handle));
    const auto entity = engine.GetScene().CreateEntity("Blocking result installed later");
    engine::MeshRenderer renderer;
    renderer.mesh = handle; // No UUID/path metadata survives in this component.
    engine.GetScene().AddComponent<engine::MeshRenderer>(entity, renderer);
    QCoreApplication::processEvents(); // Reconciliation must claim the preinstall event.
    const auto result = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(result.detail.contains(QString::fromUtf8(event.name.c_str())));
    EXPECT_TRUE(result.detail.contains(QString::fromUtf8(event.error.c_str())));
    EXPECT_EQ(result.logSearch, QString::fromUtf8(event.name.c_str()));
    engine.GetScene().Clear();
    EXPECT_FALSE(status.describe(Ready, false, false, 0).detail.contains(QString::fromUtf8(event.error.c_str())));
}

TEST(EditorRenderingStatus, RealSceneAssetPendingFailureRetryAndRepairFollowCurrentHandle) {
    AssetStatusFixture fixture;
    ASSERT_TRUE(fixture.directory.isValid());
    struct NativeWindow {
        HWND value = CreateWindowExW(0, L"STATIC", L"Scene status asset lifecycle", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~NativeWindow() { if (value) DestroyWindow(value); }
    } window;
    ASSERT_NE(window.value, nullptr);
    {
        engine::RenderingContext probe;
        try {
            if (!probe.Initialize({window.value, 64, 64}, false)) GTEST_SKIP() << "Native D3D11 unavailable";
        } catch (const engine::RenderingException& error) {
            GTEST_SKIP() << "Native D3D11 unavailable: " << error.what();
        }
    }
    engine::Engine engine({.applicationName = "Scene status repair fixture", .surface = {window.value, 64, 64},
                           .taskThreads = 2, .graphicsDebugLayer = false});
    ASSERT_NE(engine.AddSubsystem<engine::RenderingSubsystem>(), nullptr);
    editor::RenderingStatus status(engine);
    ASSERT_TRUE(engine.Initialize());
    auto& assets = engine.GetSceneAssets();
    ASSERT_TRUE(assets.LoadMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance));
    AssetCompletions events;
    entt::scoped_connection failed = engine.GetEvents().sink<engine::AssetFailedEvent>()
        .connect<&AssetCompletions::onFailure>(events);
    entt::scoped_connection loaded = engine.GetEvents().sink<engine::AssetLoadedEvent>()
        .connect<&AssetCompletions::onLoaded>(events);
    const auto handle = assets.RequestMesh("game/repair.obj");
    ASSERT_TRUE(handle);
    const auto entity = engine.GetScene().CreateEntity("Repairable renderer");
    engine::MeshRenderer renderer;
    renderer.mesh = handle;
    renderer.materialInstance = assets.RequestMaterialInstance(engine::BuiltinAssets::DefaultMaterialInstance);
    engine.GetScene().AddComponent<engine::MeshRenderer>(entity, renderer);
    ASSERT_EQ(assets.GetState(handle), engine::AssetState::Loading);
    const auto pending = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(pending.summary.contains(QStringLiteral("Preparing")));
    ASSERT_NO_FATAL_FAILURE(settle(assets, handle));
    ASSERT_EQ(assets.GetState(handle), engine::AssetState::Failed);
    ASSERT_EQ(events.failures.size(), 1u);
    const auto original = events.failures.front();
    EXPECT_EQ(original.source, &assets);
    EXPECT_EQ(original.slot, handle.slot);
    EXPECT_EQ(original.generation, handle.generation);
    EXPECT_FALSE(assets.ReferenceFor(handle));
    const auto broken = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(broken.warning);
    EXPECT_TRUE(broken.detail.contains(QStringLiteral("game/repair.obj")));
    EXPECT_TRUE(broken.detail.contains(QString::fromUtf8(original.error.c_str())));

    QFile repaired(fixture.directory.filePath(QStringLiteral("repair.obj")));
    ASSERT_TRUE(repaired.open(QIODevice::WriteOnly));
    repaired.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n");
    repaired.close();
    const auto retry = assets.RequestMesh("game/repair.obj");
    ASSERT_TRUE(retry);
    EXPECT_NE(retry, handle);
    engine.GetScene().Modify<engine::MeshRenderer>(entity, [&](auto& component) { component.mesh = retry; });
    const auto retrying = status.describe(Ready, false, false, 0);
    EXPECT_TRUE(retrying.summary.contains(QStringLiteral("Preparing")));
    EXPECT_FALSE(retrying.detail.contains(QString::fromUtf8(original.error.c_str())));
    // Late replay of an old exact generation cannot attach to this new request.
    engine.GetEvents().trigger(original);
    ASSERT_NO_FATAL_FAILURE(settle(assets, retry));
    ASSERT_EQ(assets.GetState(retry), engine::AssetState::Ready);
    ASSERT_EQ(events.loaded.size(), 1u);
    EXPECT_EQ(events.loaded.front().source, &assets);
    EXPECT_EQ(events.loaded.front().slot, retry.slot);
    EXPECT_EQ(events.loaded.front().generation, retry.generation);
    const auto healthy = status.describe(Ready, false, false, 1);
    EXPECT_FALSE(healthy.warning);
    EXPECT_TRUE(healthy.summary.isEmpty()); // Healthy Edit fallback stays in Details.
    EXPECT_FALSE(healthy.detail.contains(QString::fromUtf8(original.error.c_str())));
    EXPECT_FALSE(healthy.detail.contains(QStringLiteral("Failed / unavailable")));
}

TEST(EditorRenderingStatus, LogNavigationDrainsStartupErrorAndRemovesAllHidingFilters) {
    QSettings().clear();
    editor::ShortcutRegistry shortcuts;
    editor::LogPanel panel(shortcuts);
    auto* filter = panel.findChild<editor::LogFilterModel*>();
    auto* search = panel.findChild<QLineEdit*>();
    ASSERT_NE(filter, nullptr);
    ASSERT_NE(search, nullptr);
    filter->setLevelShown(spdlog::level::critical, false);
    filter->setSourceShown(QStringLiteral("Render"), false);
    search->setText(QStringLiteral("unrelated text"));
    spdlog::logger logger("Render", panel.sink());
    logger.critical("GBufferPass: startup compiler failure\ncompiler details");
    panel.revealDiagnostic(QStringLiteral("GBufferPass"), QStringLiteral("owned full structured detail"));
    EXPECT_TRUE(search->text().isEmpty());
    EXPECT_TRUE(filter->isSourceShown(QStringLiteral("Render")));
    EXPECT_TRUE(filter->isLevelShown(spdlog::level::critical));
    EXPECT_EQ(filter->rowCount(), 1);
    auto* details = panel.findChild<QPlainTextEdit*>();
    ASSERT_NE(details, nullptr);
    EXPECT_FALSE(details->isHidden());
    EXPECT_EQ(details->toPlainText(), QStringLiteral("owned full structured detail"));
    panel.findChild<editor::LogModel*>()->clear();
    panel.revealDiagnostic(QStringLiteral("GBufferPass"), QStringLiteral("still available after Clear"));
    EXPECT_EQ(details->toPlainText(), QStringLiteral("still available after Clear"));
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SRI.Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("editor_rendering_status_tests"));
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
