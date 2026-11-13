#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/Log.hpp>
#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>
#include "ui/UiRuntimeAccess.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using namespace engine;

class UiExampleTest : public testing::Test {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path gameRoot;
    std::vector<std::string> diagnostics;
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink =
        std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& message) {
            if (message.level >= spdlog::level::warn)
                diagnostics.emplace_back(message.payload.data(), message.payload.size());
        });
    Engine host{EngineConfig{.taskThreads = 1}};
    InputSubsystem* input = host.AddSubsystem<InputSubsystem>();
    UiSubsystem* ui = host.AddSubsystem<UiSubsystem>(UiSubsystemConfig{.renderer = UiRendererBackend::Null});
    ScriptSubsystem* scripts = host.AddSubsystem<ScriptSubsystem>();
    std::string error;

    void SetUp() override {
#ifdef SRI_SOURCE_ROOT
        gameRoot = std::filesystem::path(SRI_SOURCE_ROOT) / "example_game";
#else
        gameRoot = AssetPathResolver::GetRoot() / "game";
#endif
        ASSERT_TRUE(std::filesystem::is_regular_file(gameRoot / "scripts/ui/example_ui.lua")) << gameRoot;
        AssetPathResolver::SetGameRoot(gameRoot);
        log::Sinks()->add_sink(sink);
        ASSERT_TRUE(host.Initialize());
    }

    void TearDown() override {
        host.Shutdown();
        log::Sinks()->remove_sink(sink);
        AssetPathResolver::SetGameRoot(oldRoot);
        for (const auto& message : diagnostics) ADD_FAILURE() << message;
    }

    void Lua(std::string_view source) {
        ASSERT_TRUE(scripts->GetRuntime().ExecuteString(source, "actual example content validation", error)) << error;
    }

    void StartExample() {
        const auto* entry = host.GetAssets().GetCatalog().FindPath("game/scripts/ui/example_ui.lua");
        ASSERT_NE(entry, nullptr);
        const std::vector<AutoloadDefinition> definitions{
            {"ExampleUI", AssetReference{entry->metadata.id, entry->path}, true}};
        ASSERT_TRUE(scripts->BeginPlay(gameRoot, definitions, error)) << error;
        host.Tick(); // Force real layout and all initial data-for/data-value updates.
    }

    void MovePointer(float x, float y) {
        InputUiEvent move;
        move.pointerX = x;
        move.pointerY = y;
        move.hasPointerPosition = true;
        move.payload = InputUiPointerMove{};
        input->OnUiEvent(std::move(move));
        host.Tick();
    }

    void PressKey(Key logical, PhysicalKey physical) {
        for (const bool down : {true, false}) {
            input->OnKey(logical, down);
            input->OnPhysicalKey(physical, down);
            InputUiEvent key;
            key.payload = InputUiKey{.logical = logical, .physical = physical, .down = down};
            input->OnUiEvent(std::move(key));
        }
    }
};

TEST_F(UiExampleTest, ActualAutoloadLoadsFontsStylesDocumentsAndTypedArrayBindingsWithoutDiagnostics) {
    StartExample();
    ASSERT_FALSE(HasFatalFailure());
    auto stats = UiRuntimeAccess::Inspect(*ui);
    EXPECT_EQ(stats.documents, 3u);
    EXPECT_EQ(stats.models, 3u);
    EXPECT_EQ(stats.subscriptions, 4u);
    Lua("assert(Autoloads.ExampleUI ~= nil); assert(not Time.IsPaused())");
    // Feed the actual physical action bound by the shipped autoload.
    input->OnPhysicalKey(PhysicalKey::P, true);
    host.Tick();
    Lua("assert(Time.IsPaused())");
    EXPECT_TRUE(ui->GetCaptureState().keyboard);
    EXPECT_TRUE(ui->GetCaptureState().pointer);
    input->OnPhysicalKey(PhysicalKey::P, false);
    host.Tick(); // Layout the now-visible settings form while simulation is paused.
    scripts->EndPlay();
    host.Tick();
    stats = UiRuntimeAccess::Inspect(*ui);
    EXPECT_EQ(stats.documents, 0u);
    EXPECT_EQ(stats.models, 0u);
    EXPECT_EQ(stats.subscriptions, 0u);
    EXPECT_EQ(stats.queuedEvents, 0u);
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_FALSE(ui->GetCaptureState().pointer);
}

TEST_F(UiExampleTest, ActualInventoryActionLaysOutPopulatedArrayAndClosesOnSecondPress) {
    StartExample();
    ASSERT_FALSE(HasFatalFailure());
    MovePointer(60, 60); // Inside the shipped HUD panel: passive content must pass through.
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    input->OnPhysicalKey(PhysicalKey::I, true);
    host.Tick();
    MovePointer(640, 200); // Within the centered dialog in the 1280x720 null surface.
    EXPECT_TRUE(ui->GetCaptureState().pointer);
    Lua("assert(not Time.IsPaused())");
    MovePointer(8, 8); // The fullscreen document body is not an interactive hit region.
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    MovePointer(640, 200);
    EXPECT_TRUE(ui->GetCaptureState().pointer);
    input->OnPhysicalKey(PhysicalKey::I, false);
    host.Tick();
    input->OnPhysicalKey(PhysicalKey::I, true);
    host.Tick();
    EXPECT_FALSE(ui->GetCaptureState().pointer);
}

TEST_F(UiExampleTest, NativeLoadedActualPauseDocumentDeliversOrderedKeyboardClickWhilePaused) {
    UiSession session;
    UiOwner owner;
    ASSERT_TRUE(ui->BeginNativeSession(session, error)) << error;
    ASSERT_TRUE(ui->CreateNativeOwner(session, "actual pause content", owner, error)) << error;
    UiModelDesc settings;
    settings.name = "example_settings";
    settings.schema.fields = {{"player", {UiValueKind::String, {}, {}}},
                              {"volume", {UiValueKind::Number, {}, {}}}};
    settings.initialValues = StateValue::Object{{"player", "Native player"}, {"volume", 75}};
    UiModel model;
    ASSERT_TRUE(ui->CreateModel(owner, settings, model, error)) << error;
    UiDocument document;
    ASSERT_TRUE(ui->LoadDocument(owner, "game/ui/pause.rml", {{model}}, document, error)) << error;
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error)) << error;
    host.SetPaused(true);
    UiElement resume;
    ASSERT_TRUE(ui->FindElement(owner, document, "resume", resume, error)) << error;
    ASSERT_TRUE(ui->Focus(owner, resume, error)) << error;
    unsigned clicks = 0;
    UiSubscription subscription;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "resume", "click", [&](const UiEvent& event) {
        EXPECT_EQ(event.currentTargetId, "resume");
        EXPECT_TRUE(host.IsPaused());
        ++clicks;
        EXPECT_TRUE(ui->Hide(owner, document, error)) << error;
        host.SetPaused(false);
    }, subscription, error)) << error;
    host.Tick();
    // Traverse the actual authored form, starting at Resume and wrapping through
    // the range control, editable player name, then back to the generic button.
    PressKey(Key::Tab, PhysicalKey::Tab);
    host.Tick();
    EXPECT_FALSE(ui->GetCaptureState().textInput);
    PressKey(Key::Tab, PhysicalKey::Tab);
    host.Tick();
    EXPECT_TRUE(ui->GetCaptureState().textInput);
    PressKey(Key::Tab, PhysicalKey::Tab);
    host.Tick();
    EXPECT_FALSE(ui->GetCaptureState().textInput);
    PressKey(Key::Space, PhysicalKey::Space);
    EXPECT_EQ(clicks, 0u);
    host.Tick();
    EXPECT_EQ(clicks, 1u);
    EXPECT_FALSE(host.IsPaused());
    EXPECT_FALSE(ui->IsVisible(document));
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    EXPECT_FALSE(input->WasPressed(Key::Space));
    EXPECT_FALSE(input->WasReleased(Key::Space));
    ui->EndNativeSession(session);
    host.Tick();
    EXPECT_EQ(UiRuntimeAccess::Inspect(*ui).documents, 0u);
}
} // namespace
