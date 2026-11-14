#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/core/Log.hpp>
#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>
#include "ui/UiRuntimeAccess.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {
using namespace engine;

struct Roots {
    std::filesystem::path game = AssetPathResolver::GetGameRoot();
    ~Roots() {
        AssetPathResolver::SetGameRoot(game);
    }
};

struct Messages {
    std::vector<std::string> errors;
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink =
        std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg& message) {
            if (message.level == spdlog::level::err)
                errors.emplace_back(message.payload.data(), message.payload.size());
        });
    Messages() { log::Sinks()->add_sink(sink); }
    ~Messages() { log::Sinks()->remove_sink(sink); }
};

// No simultaneous successful runtimes: Rml interfaces and asset roots are global.
class UiTest : public testing::Test {
protected:
    Roots roots;
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-ui-" + NewUuid());
    Engine host{EngineConfig{.taskThreads = 1}};
    InputSubsystem* input = host.AddSubsystem<InputSubsystem>();
    UiSubsystem* ui = host.AddSubsystem<UiSubsystem>(UiSubsystemConfig{.renderer = UiRendererBackend::Null});
    ScriptSubsystem* scripts = host.AddSubsystem<ScriptSubsystem>();
    UiSession session;
    UiOwner owner;
    std::string error;
    unsigned nextScript = 0;

    static constexpr std::string_view markup = R"(<rml><head><title>UI test</title>
        <style>body { width: 640px; height: 480px; } input { width: 180px; height: 30px; }</style>
        </head><body><input id="edit" type="text" value="initial"/>
        <div id="container"><span id="child">child</span></div></body></rml>)";

    AssetReference Asset(std::string_view name, std::string_view contents, CatalogAssetKind kind) {
        const auto file = root / name;
        std::filesystem::create_directories(file.parent_path());
        EXPECT_TRUE(CreateNewFile(file, contents, error)) << error;
        AssetMetadata metadata{NewUuid(), kind};
        EXPECT_TRUE(WriteAssetMetadata(file, metadata, error)) << error;
        return {metadata.id, "game/" + std::string(name)};
    }
    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        Asset("ui/test.rml", markup, CatalogAssetKind::UiDocument);
        ASSERT_TRUE(host.Initialize());
        ASSERT_TRUE(ui->BeginNativeSession(session, error)) << error;
        ASSERT_TRUE(ui->CreateNativeOwner(session, "native test", owner, error)) << error;
    }
    void TearDown() override {
        host.Shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    UiDocument Load(std::string_view path = "game/ui/test.rml", UiDocumentDesc desc = {}) {
        UiDocument document;
        EXPECT_TRUE(ui->LoadDocument(owner, path, desc, document, error)) << error;
        return document;
    }
    UiElement Element(UiDocument document, std::string_view id) {
        UiElement element;
        EXPECT_TRUE(ui->FindElement(owner, document, id, element, error)) << error;
        return element;
    }
    void Tick() { host.Tick(); }
    void Lua(std::string_view source) {
        EXPECT_TRUE(scripts->GetRuntime().ExecuteString(source, "UI integration assertion", error)) << error;
    }
    InstanceId Start(std::string_view source, std::string name = "Panel") {
        ScriptInstanceDesc desc;
        desc.source = root / ("script" + std::to_string(nextScript++) + ".lua");
        desc.assetId = NewUuid();
        desc.autoloadName = std::move(name);
        EXPECT_TRUE(CreateNewFile(desc.source, source, error)) << error;
        InstanceId instance;
        EXPECT_TRUE(scripts->GetRuntime().CreateInstance(desc, instance, error)) << error;
        EXPECT_TRUE(scripts->GetRuntime().StartInstance(instance, error)) << error;
        return instance;
    }
    void KeyEvent(Key key, bool down, bool repeat = false) {
        InputUiEvent event;
        event.payload = InputUiKey{.logical = key, .down = down, .repeat = repeat};
        input->OnUiEvent(std::move(event));
    }
    void Queue(UiSubscription subscription, std::string type = "click") {
        UiEvent event;
        event.type = std::move(type);
        event.targetId = event.currentTargetId = "edit";
        EXPECT_TRUE(UiRuntimeAccess::QueueEventForTesting(*ui, subscription, event, error)) << error;
    }
    void EmptyResources() {
        const auto stats = UiRuntimeAccess::Inspect(*ui);
        EXPECT_EQ(stats.documents, 0u);
        EXPECT_EQ(stats.models, 0u);
        EXPECT_EQ(stats.subscriptions, 0u);
        EXPECT_EQ(stats.queuedEvents, 0u);
    }
};

TEST_F(UiTest, RejectsSecondGlobalOwnerAndSupportsSequentialReinitialization) {
    Engine second{EngineConfig{.taskThreads = 1}};
    auto* other = second.AddSubsystem<UiSubsystem>(UiSubsystemConfig{.renderer = UiRendererBackend::Null});
    EXPECT_FALSE(second.Initialize());
    EXPECT_FALSE(other->IsInitialized());
    second.Shutdown();
    const auto document = Load();
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error)) << error;
    EXPECT_TRUE(ui->GetCaptureState().keyboard);
    const auto lifetime = ui->GetLifetimeToken();
    ui->Shutdown();
    EXPECT_TRUE(lifetime.expired());
    ASSERT_TRUE(ui->Initialize());
    UiSession fresh;
    ASSERT_TRUE(ui->BeginNativeSession(fresh, error)) << error;
    EXPECT_NE(fresh, session);
    EXPECT_FALSE(ui->IsValid(document));
    EXPECT_FALSE(ui->Show(owner, document, UiDocumentMode::Modal, error));
    EmptyResources();
}

TEST_F(UiTest, OwnerIsolationIdempotentCloseSlotReuseAndSessionReplacement) {
    const auto old = Load();
    const auto element = Element(old, "edit");
    UiOwner other;
    ASSERT_TRUE(ui->CreateNativeOwner(session, "other", other, error));
    EXPECT_FALSE(ui->Close(other, old, error));
    EXPECT_FALSE(ui->SetText(other, element, "intrusion", error));
    ASSERT_TRUE(ui->Close(owner, old, error));
    EXPECT_FALSE(ui->IsValid(old));
    EXPECT_FALSE(ui->IsVisible(old));
    EXPECT_FALSE(ui->IsValid(element));
    Tick();
    const auto fresh = Load();
    EXPECT_NE(fresh, old);
    EXPECT_TRUE(ui->Close(owner, old, error)) << error;
    EXPECT_FALSE(ui->Close(other, old, error));
    EXPECT_TRUE(ui->IsValid(fresh));
    ui->ReleaseNativeOwner(owner);
    EXPECT_FALSE(ui->IsValid(fresh));
    EXPECT_TRUE(ui->Close(owner, fresh, error)) << error;
    UiDocument rejected;
    EXPECT_FALSE(ui->LoadDocument(owner, "game/ui/test.rml", {}, rejected, error));
    ui->EndNativeSession(session);
    ASSERT_TRUE(ui->BeginNativeSession(session, error));
    EXPECT_FALSE(ui->Show(owner, fresh, UiDocumentMode::Hud, error));
    Tick();
    EmptyResources();
}

TEST_F(UiTest, DomReplacementInvalidatesDescendantsAndPlainTextDoesNotInsertMarkup) {
    const auto document = Load();
    const auto container = Element(document, "container");
    const auto child = Element(document, "child");
    ASSERT_TRUE(ui->SetText(owner, container, "<input id='injected'/>", error));
    EXPECT_FALSE(ui->IsValid(child));
    UiElement missing;
    EXPECT_FALSE(ui->FindElement(owner, document, "injected", missing, error));
    ASSERT_TRUE(ui->SetInnerRml(owner, container, "<input id='inserted' type='text' value='yes'/>", error));
    const auto inserted = Element(document, "inserted");
    std::string value;
    ASSERT_TRUE(ui->GetFormValue(owner, inserted, value, error));
    EXPECT_EQ(value, "yes");
}

TEST_F(UiTest, ReloadCommitsAtomicallyRetainsSubscriptionAndRollsBackMissingResource) {
    const auto document = Load();
    const auto old = Element(document, "edit");
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error));
    int calls = 0;
    UiSubscription subscription;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "click", [&](const UiEvent&) { ++calls; }, subscription, error));
    ASSERT_TRUE(WriteFileAtomic(root / "ui/test.rml", R"(<rml><head><title>new</title></head><body>
        <input id="edit" type="text" value="replacement"/></body></rml>)", error));
    ASSERT_TRUE(ui->RequestReload(owner, document, error));
    Tick();
    EXPECT_TRUE(ui->IsValid(document));
    EXPECT_TRUE(ui->IsVisible(document));
    EXPECT_FALSE(ui->IsValid(old));
    EXPECT_TRUE(ui->GetCaptureState().keyboard);
    const auto replacement = Element(document, "edit");
    std::string value;
    ASSERT_TRUE(ui->GetFormValue(owner, replacement, value, error));
    EXPECT_EQ(value, "replacement");
    Queue(subscription);
    Tick();
    EXPECT_EQ(calls, 1);
    // A non-null parser result is insufficient: a failed stylesheet must roll back.
    ASSERT_TRUE(WriteFileAtomic(root / "ui/test.rml", R"(<rml><head><title>bad</title>
        <link type="text/rcss" href="missing.rcss"/></head><body><input id="edit"/></body></rml>)", error));
    ASSERT_TRUE(ui->RequestReload(owner, document, error));
    Tick();
    EXPECT_TRUE(ui->IsValid(replacement));
    ASSERT_TRUE(ui->GetFormValue(owner, replacement, value, error));
    EXPECT_EQ(value, "replacement");
    Queue(subscription);
    Tick();
    EXPECT_EQ(calls, 2);
}

TEST_F(UiTest, DeferredSelfDisconnectCloseAndOwnerReleaseCancelAlreadyQueuedDeliveries) {
    for (int action = 0; action != 3; ++action) {
        SCOPED_TRACE(action);
        UiOwner current;
        ASSERT_TRUE(ui->CreateNativeOwner(session, "callback owner", current, error));
        UiDocument document;
        ASSERT_TRUE(ui->LoadDocument(current, "game/ui/test.rml", {}, document, error));
        UiSubscription subscription;
        int calls = 0;
        ASSERT_TRUE(ui->SubscribeNative(current, document, "edit", "click", [&](const UiEvent&) {
            ++calls;
            if (action == 0) EXPECT_TRUE(ui->Disconnect(current, subscription, error));
            if (action == 1) EXPECT_TRUE(ui->Close(current, document, error));
            if (action == 2) ui->ReleaseNativeOwner(current);
        }, subscription, error));
        Queue(subscription);
        Queue(subscription);
        EXPECT_EQ(calls, 0);
        Tick();
        EXPECT_EQ(calls, 1);
        EXPECT_FALSE(ui->IsValid(subscription));
        EXPECT_TRUE(ui->Disconnect(current, subscription, error)) << error;
        ui->ReleaseNativeOwner(current);
    }
    Tick();
    EmptyResources();
}

TEST_F(UiTest, NativeCallbackExceptionRetiresModalAndReleasesCapture) {
    const auto document = Load();
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error));
    UiSubscription subscription;
    int calls = 0;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "click", [&](const UiEvent&) {
        ++calls;
        throw std::runtime_error("native UI failure marker");
    }, subscription, error));
    Queue(subscription);
    Queue(subscription);
    Tick();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(ui->IsValid(document));
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    EmptyResources();
}

UiValueSchema Scalar(UiValueKind kind) { return {kind, {}, {}}; }
UiValueSchema ArrayOf(UiValueSchema element) { return {UiValueKind::Array, {}, {std::move(element)}}; }

TEST_F(UiTest, RealBoundFormsSurviveArrayGrowthZeroAndNestedReplacement) {
    UiModelDesc desc;
    desc.name = "inventory";
    const UiValueSchema item{UiValueKind::Object,
        {{"name", Scalar(UiValueKind::String)}, {"counts", ArrayOf(Scalar(UiValueKind::Number))}}, {}};
    desc.schema.fields = {{"items", ArrayOf(item)}, {"title", Scalar(UiValueKind::String)}};
    auto row = [](std::string name, double count) {
        return StateValue(StateValue::Object{{"name", std::move(name)}, {"counts", StateValue::Array{count}}});
    };
    desc.initialValues = StateValue::Object{{"items", StateValue::Array{row("seed", 1)}}, {"title", "initial"}};
    UiModel model;
    ASSERT_TRUE(ui->CreateModel(owner, desc, model, error)) << error;
    Asset("ui/model.rml", R"(<rml><head><title>binding</title></head><body data-model="inventory">
        <input id="title" type="text" data-value="title"/>
        <div data-for="item : items"><input type="text" data-value="item.name"/>
        <span data-for="count : item.counts">{{count}}</span></div>
        <input id="first" type="text" data-value="items[0].name"/>
        </body></rml>)", CatalogAssetKind::UiDocument);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    const auto document = Load("game/ui/model.rml", {{model}});
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Interactive, error));
    Tick();
    const auto first = Element(document, "first");
    auto expectForm = [&](std::string_view expected) {
        std::string text;
        EXPECT_TRUE(ui->GetFormValue(owner, first, text, error)) << error;
        EXPECT_EQ(text, expected);
    };
    expectForm("seed");
    StateValue::Array grown;
    for (int i = 0; i < 256; ++i) grown.push_back(row("row" + std::to_string(i), i));
    ASSERT_TRUE(ui->SetModelValue(owner, model, "items", grown, error)) << error;
    Tick();
    expectForm("row0");
    const UiModelPath namePath{std::string("items"), std::uint32_t(0), std::string("name")};
    const UiModelPath nested{std::string("items"), std::uint32_t(255), std::string("counts"), std::uint32_t(0)};
    ASSERT_TRUE(UiRuntimeAccess::WriteModelForTesting(*ui, owner, model, nested, 42.5, error)) << error;
    ASSERT_TRUE(UiRuntimeAccess::WriteModelForTesting(*ui, owner, model, namePath, "form write", error)) << error;
    StateValue copy;
    ASSERT_TRUE(ui->GetModelValue(owner, model, "items", copy, error));
    EXPECT_EQ(copy.Items()[255].At("counts").Items()[0].Number(), 42.5);
    EXPECT_EQ(copy.Items()[0].At("name").String(), "form write");
    copy.value = nullptr; // Get must not alias storage.
    Tick();
    expectForm("form write");
    ASSERT_TRUE(ui->SetModelValue(owner, model, "items", StateValue::Array{}, error));
    Tick();
    EXPECT_FALSE(UiRuntimeAccess::WriteModelForTesting(*ui, owner, model, namePath, "removed", error));
    ASSERT_TRUE(ui->SetModelValue(owner, model, "items", StateValue::Array{row("reborn", 7)}, error));
    Tick();
    expectForm("reborn");
    // Actual Rml form event writes through its data-value binding, not the seam.
    ASSERT_TRUE(ui->SetFormValue(owner, first, "typed", error));
    ASSERT_TRUE(ui->GetModelValue(owner, model, "items", copy, error));
    EXPECT_EQ(copy.Items()[0].At("name").String(), "typed");
    EXPECT_FALSE(ui->RemoveModel(owner, model, error));
    ASSERT_TRUE(ui->Close(owner, document, error));
    Tick();
    EXPECT_TRUE(ui->RemoveModel(owner, model, error)) << error;
}

TEST_F(UiTest, DuplicateCrossOwnerAndUndeclaredModelDependenciesAreRejected) {
    UiModelDesc desc;
    desc.name = "shared";
    desc.schema.fields = {{"value", Scalar(UiValueKind::Number)}};
    desc.initialValues = StateValue::Object{{"value", 1}};
    UiModel model, duplicate;
    ASSERT_TRUE(ui->CreateModel(owner, desc, model, error));
    UiOwner other;
    ASSERT_TRUE(ui->CreateNativeOwner(session, "foreign", other, error));
    EXPECT_FALSE(ui->CreateModel(other, desc, duplicate, error));
    Asset("ui/shared.rml", R"(<rml><head><title>shared</title></head>
        <body data-model="shared"><span>{{value}}</span></body></rml>)", CatalogAssetKind::UiDocument);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error));
    UiDocument document;
    EXPECT_FALSE(ui->LoadDocument(owner, "game/ui/shared.rml", {}, document, error));
    EXPECT_FALSE(ui->LoadDocument(other, "game/ui/shared.rml", {{model}}, document, error));
    EXPECT_FALSE(ui->LoadDocument(other, "game/ui/shared.rml", {}, document, error));
    ASSERT_TRUE(ui->LoadDocument(owner, "game/ui/shared.rml", {{model}}, document, error)) << error;
    for (const StateValue invalid : {StateValue(nullptr), StateValue("wrong"),
        StateValue(std::numeric_limits<double>::infinity()), StateValue(std::uint64_t(9007199254740993ULL))}) {
        EXPECT_FALSE(ui->SetModelValue(owner, model, "value", invalid, error));
        StateValue value;
        ASSERT_TRUE(ui->GetModelValue(owner, model, "value", value, error));
        EXPECT_EQ(value.Number(), 1);
    }
    EXPECT_FALSE(ui->SetModelValue(other, model, "value", 2, error));
    ui->ReleaseNativeOwner(owner);
    Tick();
    EmptyResources();
}

TEST_F(UiTest, LuaCallbacksRunPausedRetainDiscardedSubscriptionAndReleaseReferencesOnDestroy) {
    ASSERT_TRUE(scripts->BeginPlay(root, error)) << error;
    Lua("stats={events=0, updates=0}; refs=setmetatable({}, {__mode='v'})");
    const auto instance = Start(R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('modal'))
            local callback = function(event)
                assert(Time.IsPaused())
                assert(event.type == 'keydown' and event.targetId == 'edit')
                stats.events = stats.events + 1
            end
            refs[1] = callback
            assert(doc:On('edit', 'keydown', callback)) -- deliberately discard subscription
            assert(doc:GetElement('edit'):Focus())
        end
        function OnUpdate(dt) stats.updates = stats.updates + 1 end
        function OnDestroy()
            assert(doc:Close())
            assert(not UI.LoadDocument('game/ui/test.rml'))
        end
    )");
    host.SetPaused(true);
    Lua("collectgarbage('collect'); assert(refs[1] ~= nil)");
    KeyEvent(Key::A, true);
    Tick();
    Lua("assert(stats.events == 1 and stats.updates == 0)");
    ASSERT_TRUE(scripts->GetRuntime().DestroyInstance(instance, error)) << error;
    Lua("collectgarbage('collect'); assert(refs[1] == nil)");
    Tick();
    EmptyResources();
}

TEST_F(UiTest, LuaSelfDisconnectCloseFaultAndShutdownNeverDeliverStaleCallbacks) {
    ASSERT_TRUE(scripts->BeginPlay(root, error));
    Lua("stats={events=0}");
    Start(R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('modal'))
            subscription = assert(doc:On('edit', 'keydown', function(event)
                stats.events = stats.events + 1
                assert(subscription:Disconnect())
                assert(doc:Close())
            end))
            assert(doc:GetElement('edit'):Focus())
        end
    )");
    KeyEvent(Key::A, true);
    KeyEvent(Key::A, true, true);
    Tick();
    Lua("assert(stats.events == 1)");
    EmptyResources();
    Messages messages;
    const auto faulted = Start(R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('modal'))
            assert(doc:On('edit', 'keydown', function(event) error('Lua UI failure marker') end))
            assert(doc:GetElement('edit'):Focus())
        end
    )", "FaultingPanel");
    KeyEvent(Key::B, true);
    Tick();
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    EXPECT_FALSE(scripts->GetRuntime().UpdateInstance(faulted, 0.1, error));
    const auto diagnostic = std::find_if(messages.errors.begin(), messages.errors.end(), [](const auto& text) {
        return text.find("Lua UI failure marker") != std::string::npos;
    });
    ASSERT_NE(diagnostic, messages.errors.end());
    EXPECT_NE(diagnostic->find("FaultingPanel"), std::string::npos);
    EXPECT_NE(diagnostic->find("edit"), std::string::npos);
    EXPECT_NE(diagnostic->find("keydown"), std::string::npos);
    EXPECT_NE(diagnostic->find("stack traceback"), std::string::npos);
    EmptyResources();
    Start(R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('modal'))
            assert(doc:On('edit', 'keydown', function() error('callback after shutdown') end))
            assert(doc:GetElement('edit'):Focus())
        end
    )", "ShutdownPanel");
    KeyEvent(Key::C, true);
    scripts->EndPlay();
    Tick();
    EmptyResources();
    EXPECT_TRUE(std::none_of(messages.errors.begin(), messages.errors.end(), [](const auto& text) {
        return text.find("callback after shutdown") != std::string::npos;
    }));
    ASSERT_TRUE(scripts->BeginPlay(root, error));
    Lua("assert(Autoloads.ShutdownPanel == nil)");
}

TEST_F(UiTest, FailedLuaStartupAutomaticallyRetiresModalAndCallbacks) {
    const auto asset = Asset("Broken.lua", R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('modal'))
            assert(doc:On('edit', 'keydown', function() error('retired callback') end))
            error('startup failure marker')
        end
    )", CatalogAssetKind::Script);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error));
    const std::vector<AutoloadDefinition> definitions{{"Broken", asset, true}};
    EXPECT_FALSE(scripts->BeginPlay(root, definitions, error));
    EXPECT_NE(error.find("startup failure marker"), std::string::npos);
    Tick();
    EXPECT_FALSE(scripts->GetRuntime().IsInitialized());
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    EmptyResources();
    ASSERT_TRUE(scripts->BeginPlay(root, error)) << error;
}

TEST_F(UiTest, OpenLevelRemovesEntityUiButRetainsAutoloadDocumentModelAndCallback) {
    const auto persistent = Asset("Persistent.lua", R"(
        function OnStart()
            starts = (starts or 0) + 1
            events = 0
            model = assert(UI.CreateModel('persistent', {title='retained'}))
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('modal'))
            assert(doc:On('edit', 'keydown', function() events = events + 1 end))
        end
        function Focus() assert(doc:GetElement('edit'):Focus()) end
        function Title() return assert(model:Get('title')) end
    )", CatalogAssetKind::Script);
    const auto entityScript = Asset("Entity.lua", R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/ui/test.rml'))
            assert(doc:Show('interactive'))
            assert(doc:On('edit', 'keydown', function() error('dead entity callback') end))
        end
        function OnDestroy() assert(doc:IsValid()); assert(doc:Close()) end
    )", CatalogAssetKind::Script);
    Asset("scenes/Next.scene.json", R"({"version":1,"entities":[]})", CatalogAssetKind::Scene);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error));
    host.SetEditorMode();
    const auto entity = host.GetScene().CreateEntity("Scene UI owner");
    host.GetScene().AddComponent<ScriptComponent>(entity, ScriptComponent{entityScript, true});
    const std::vector<AutoloadDefinition> definitions{{"Persistent", persistent, true}};
    ASSERT_TRUE(host.PlayCurrentScene(root, definitions, error)) << error;
    EXPECT_EQ(UiRuntimeAccess::Inspect(*ui).documents, 2u);
    Lua("retained=Autoloads.Persistent.doc; assert(Scene.OpenLevel('Next'))");
    Tick();
    EXPECT_FALSE(host.GetScene().IsValid(entity));
    const auto stats = UiRuntimeAccess::Inspect(*ui);
    EXPECT_EQ(stats.documents, 1u);
    EXPECT_EQ(stats.models, 1u);
    EXPECT_EQ(stats.subscriptions, 1u);
    // Scene replacement resets Input and queues its focus-loss notification for
    // the next latch. Complete that transient reset before refocusing the retained
    // DOM, then model the native host reacquiring the Play surface explicitly.
    Tick();
    InputUiEvent regainedFocus;
    regainedFocus.payload = InputUiFocus{true};
    input->OnUiEvent(std::move(regainedFocus));
    Tick();
    Lua(R"(
        assert(Autoloads.Persistent.starts == 1)
        assert(Autoloads.Persistent.Title() == 'retained')
        assert(retained:IsValid() and retained:IsVisible())
        Autoloads.Persistent.Focus()
    )");
    KeyEvent(Key::A, true);
    Tick();
    Lua("assert(Autoloads.Persistent.events == 1, 'persistent events=' .. tostring(Autoloads.Persistent.events))");
    host.StopPlay();
    Tick();
    EmptyResources();
}

TEST_F(UiTest, CallbackGeneratedEventsWaitForNextUpdateAndLateUpdateNeverDispatches) {
    const auto document = Load();
    UiSubscription subscription;
    int calls = 0;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "click", [&](const UiEvent&) {
        ++calls;
        if (calls == 1) Queue(subscription);
    }, subscription, error));
    Queue(subscription);
    FrameContext frame;
    ui->LateUpdate(frame);
    ui->Render(frame);
    EXPECT_EQ(calls, 0);
    ui->Update(frame);
    EXPECT_EQ(calls, 1);
    ui->LateUpdate(frame);
    EXPECT_EQ(calls, 1);
    ui->Update(frame);
    EXPECT_EQ(calls, 2);
}

TEST_F(UiTest, InteractiveDragRetainsPointerOutsideHitRegionAndConsumesRelease) {
    const auto document = Load();
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Interactive, error)) << error;
    auto pointer = [&](float x, float y, bool button, bool down) {
        InputUiEvent event;
        event.pointerX = x;
        event.pointerY = y;
        event.hasPointerPosition = true;
        if (button) {
            event.payload = InputUiPointerButton{MouseButton::Left, down};
            input->OnMouseButton(MouseButton::Left, down);
        } else event.payload = InputUiPointerMove{};
        input->OnUiEvent(std::move(event));
    };
    pointer(20, 15, true, true);
    Tick();
    ASSERT_TRUE(ui->GetCaptureState().pointer);
    EXPECT_FALSE(input->WasPressed(MouseButton::Left));
    // Continue selection/dragging well outside the document's 640x480 body.
    pointer(1000, 650, false, false);
    Tick();
    EXPECT_TRUE(ui->GetCaptureState().pointer);
    pointer(1000, 650, true, false);
    Tick();
    EXPECT_FALSE(ui->GetCaptureState().pointer);
    EXPECT_FALSE(input->WasReleased(MouseButton::Left));
    // A fresh outside click is gameplay input, even with a visible interactive doc.
    pointer(1000, 650, true, true);
    Tick();
    EXPECT_TRUE(input->WasPressed(MouseButton::Left));
    EXPECT_FALSE(ui->GetCaptureState().pointer);
}

TEST_F(UiTest, AssetUuidIsAuthoritativeAndFailedLoadPreservesOutput) {
    const auto document = Load();
    UiDocument output = document;
    const AssetReference unknown{NewUuid(), "game/ui/test.rml"};
    EXPECT_FALSE(ui->LoadDocument(owner, unknown, {}, output, error));
    EXPECT_EQ(output, document);
    const auto* entry = host.GetAssets().GetCatalog().FindPath("game/ui/test.rml");
    ASSERT_NE(entry, nullptr);
    const AssetReference renamedHint{entry->metadata.id, "game/does-not-exist.rml"};
    ASSERT_TRUE(ui->LoadDocument(owner, renamedHint, {}, output, error)) << error;
    EXPECT_NE(output, document);
    EXPECT_TRUE(ui->IsValid(output));
}

TEST_F(UiTest, OrderedKeyRepeatsAndSameFrameModalCloseKeepGameplayEdgesConsumed) {
    const auto document = Load();
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error));
    ASSERT_TRUE(ui->Focus(owner, Element(document, "edit"), error));
    std::vector<std::string> order;
    std::vector<std::uint64_t> sequences;
    UiSubscription down, up;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "keydown", [&](const UiEvent& event) {
        order.push_back("down");
        sequences.push_back(event.inputSequence);
    }, down, error));
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "keyup", [&](const UiEvent& event) {
        order.push_back("up");
        sequences.push_back(event.inputSequence);
        EXPECT_TRUE(ui->Hide(owner, document, error));
    }, up, error));
    input->OnKey(Key::A, true);
    KeyEvent(Key::A, true);
    KeyEvent(Key::A, true, true);
    input->OnKey(Key::A, false);
    KeyEvent(Key::A, false);
    Tick();
    EXPECT_EQ(order, (std::vector<std::string>{"down", "down", "up"}));
    ASSERT_EQ(sequences.size(), 3u);
    EXPECT_GT(sequences[0], 0u);
    EXPECT_LT(sequences[0], sequences[1]);
    EXPECT_LT(sequences[1], sequences[2]);
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_FALSE(input->WasPressed(Key::A));
    EXPECT_FALSE(input->WasReleased(Key::A));
}

TEST_F(UiTest, ButtonNavigationDefaultsRespectAuthoredAndDynamicTabOptOut) {
    Asset("ui/navigation.rml", R"(<rml><head><title>Navigation defaults</title>
        <style>.skip { tab-index: none; }</style></head><body>
        <button id="first">First</button><button id="optional" class="skip">Optional</button>
        <button id="last">Last</button></body></rml>)", CatalogAssetKind::UiDocument);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    const auto document = Load("game/ui/navigation.rml");
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Interactive, error)) << error;
    const auto first = Element(document, "first");
    const auto optional = Element(document, "optional");
    unsigned lastClicks = 0, optionalClicks = 0;
    UiSubscription lastSubscription, optionalSubscription;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "last", "click", [&](const UiEvent&) {
        ++lastClicks;
    }, lastSubscription, error)) << error;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "optional", "click", [&](const UiEvent&) {
        ++optionalClicks;
    }, optionalSubscription, error)) << error;
    auto tabAndActivate = [&] {
        KeyEvent(Key::Tab, true);
        KeyEvent(Key::Tab, false);
        KeyEvent(Key::Space, true);
        KeyEvent(Key::Space, false);
        Tick();
    };
    ASSERT_TRUE(ui->Focus(owner, first, error)) << error;
    tabAndActivate();
    EXPECT_EQ(lastClicks, 1u);
    EXPECT_EQ(optionalClicks, 0u);
    ASSERT_TRUE(ui->SetClass(owner, optional, "skip", false, error)) << error;
    Tick();
    ASSERT_TRUE(ui->Focus(owner, first, error)) << error;
    tabAndActivate();
    EXPECT_EQ(lastClicks, 1u);
    EXPECT_EQ(optionalClicks, 1u);
}
} // namespace
