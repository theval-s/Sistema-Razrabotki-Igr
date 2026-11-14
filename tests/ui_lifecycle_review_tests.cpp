// Targeted regressions for the UI lifecycle/owner-dispatch review.
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>
#include "ui/UiRuntimeAccess.hpp"
#include <gtest/gtest.h>

namespace {
using namespace engine;

class UiLifecycleReview : public testing::Test {
protected:
    std::filesystem::path previousRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-ui-review-" + NewUuid());
    Engine engine{EngineConfig{.taskThreads = 1}};
    InputSubsystem* input = engine.AddSubsystem<InputSubsystem>();
    UiSubsystem* ui = engine.AddSubsystem<UiSubsystem>(UiSubsystemConfig{.renderer = UiRendererBackend::Null});
    ScriptSubsystem* scripts = engine.AddSubsystem<ScriptSubsystem>();
    UiSession session;
    UiOwner owner;
    UiDocument document;
    UiElement edit;
    std::string error;

    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        const auto file = root / "panel.rml";
        ASSERT_TRUE(CreateNewFile(file,
            "<rml><head><title>Review</title></head><body><input id='edit' type='text'/></body></rml>", error)) << error;
        ASSERT_TRUE(WriteAssetMetadata(file, {NewUuid(), CatalogAssetKind::UiDocument}, error)) << error;
        ASSERT_TRUE(engine.Initialize());
        ASSERT_TRUE(ui->BeginNativeSession(session, error));
        ASSERT_TRUE(ui->CreateNativeOwner(session, "review", owner, error));
        ASSERT_TRUE(ui->LoadDocument(owner, "game/panel.rml", {}, document, error)) << error;
        ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error));
        ASSERT_TRUE(ui->FindElement(owner, document, "edit", edit, error));
        ASSERT_TRUE(ui->Focus(owner, edit, error));
    }
    void TearDown() override {
        engine.Shutdown();
        AssetPathResolver::SetGameRoot(previousRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void KeyDown(Key key) {
        InputUiEvent event;
        event.payload = InputUiKey{.logical = key, .down = true};
        input->OnUiEvent(std::move(event));
    }
    InstanceId Start(std::string_view name, std::string_view source) {
        ScriptInstanceDesc desc;
        desc.source = root / (std::string(name) + ".lua");
        desc.assetId = NewUuid();
        desc.autoloadName = name;
        EXPECT_TRUE(CreateNewFile(desc.source, source, error)) << error;
        InstanceId id;
        EXPECT_TRUE(scripts->GetRuntime().CreateInstance(desc, id, error)) << error;
        EXPECT_TRUE(scripts->GetRuntime().StartInstance(id, error)) << error;
        return id;
    }
    void Lua(std::string_view source) {
        EXPECT_TRUE(scripts->GetRuntime().ExecuteString(source, "autoload UI dispatch regression", error)) << error;
    }
};

TEST_F(UiLifecycleReview, CallbackInputResetCancelsRemainingLatchedEvents) {
    UiSubscription subscription;
    int calls = 0;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "keydown", [&](const UiEvent&) {
        ++calls;
        input->Reset(); // Invalidates the span that Update originally latched.
    }, subscription, error));
    KeyDown(Key::A);
    KeyDown(Key::B);
    engine.Tick();
    EXPECT_EQ(calls, 1);
}

TEST_F(UiLifecycleReview, CallbackCannotReinitializeUntilDeferredShutdownCompletes) {
    UiSubscription subscription;
    int calls = 0;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "keydown", [&](const UiEvent&) {
        ++calls;
        ui->Shutdown();
        EXPECT_FALSE(ui->Initialize());
    }, subscription, error));
    KeyDown(Key::A);
    engine.Tick();
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(ui->IsInitialized());
    ASSERT_TRUE(ui->Initialize());
    EXPECT_FALSE(ui->IsValid(document));
    UiSession fresh;
    ASSERT_TRUE(ui->BeginNativeSession(fresh, error));
    EXPECT_NE(fresh, session);
}

TEST_F(UiLifecycleReview, ElementLookupsReuseIdentityButDomReplacementDoesNot) {
    UiElement again;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(ui->FindElement(owner, document, "edit", again, error));
        EXPECT_EQ(again, edit);
    }
    ASSERT_TRUE(ui->Close(owner, document, error));
    EXPECT_FALSE(ui->IsValid(edit));
    ASSERT_TRUE(ui->LoadDocument(owner, "game/panel.rml", {}, document, error));
    ASSERT_TRUE(ui->FindElement(owner, document, "edit", again, error));
    EXPECT_NE(again, edit);
    ui->EndNativeSession(session);
    EXPECT_FALSE(ui->Close(owner, document, error)); // Expired session, not a live tombstone.
}

TEST_F(UiLifecycleReview, AutoloadMethodsSwitchOwnersPreserveIdentityAndGuardRawAndDynamicAccess) {
    ASSERT_TRUE(scripts->BeginPlay(root, error)) << error;
    const auto service = Start("Panel", R"(
        return {
            OnStart = function(self)
                assert(self == Autoloads.Panel)
                self.model = assert(UI.CreateModel('panel', {value = 1}))
                self.doc = assert(UI.LoadDocument('game/panel.rml'))
            end,
            Set = function(self, value)
                assert(self == Autoloads.Panel)
                assert(self.model:Set('value', value))
                assert(self.doc:Show('modal'))
                return value, nil, 'tail'
            end,
            Fail = function(self) error('autoload-method-failure') end
        }
    )");
    Start("Caller", R"(
        function OnStart()
            local a, b, c = Autoloads.Panel:Set(2)
            assert(a == 2 and b == nil and c == 'tail')
            model = assert(UI.CreateModel('caller', {value = 3}))
            assert(model:Set('value', 4)) -- receiver scope restored after Panel:Set
            assert(not Autoloads.Panel.model:Set('value', 999)) -- leaked handle grants no ownership
            -- Raw C-API StateValue iteration must see managed fields, including
            -- unsupported functions/userdata, rather than accepting an empty object.
            assert(not UI.CreateModel('badManaged', {nested = Autoloads.Panel}))
        end
    )");
    Lua(R"(
        local panel = Autoloads.Panel
        local saved = rawget(panel, 'Set')
        assert(saved == panel.Set)
        local a, b, c = saved(panel, 5)
        assert(a == 5 and b == nil and c == 'tail')
        local seen = false
        for key, value in pairs(panel) do
            if key == 'Set' then assert(value == saved); seen = true end
        end
        assert(seen)
        local key, value = next(panel)
        while key ~= nil do
            if key == 'Set' then assert(value == saved) end
            key, value = next(panel, key)
        end
        panel.Dynamic = function(self)
            assert(self == Autoloads.Panel)
            assert(self.model:Set('value', 6))
        end
        panel:Dynamic()
        rawset(panel, 'Dynamic', function(self) assert(self.model:Set('value', 7)) end)
        rawget(panel, 'Dynamic')(panel)
        panel.Set = 42
        panel.Set = function(self) assert(self.model:Set('value', 8)) end
        panel:Set()
        saved(panel, 9) -- cached method retains its original function, not a name lookup
        retainedMethod, retainedTable = saved, panel
    )");
    ASSERT_TRUE(scripts->GetRuntime().DestroyInstance(service, error)) << error;
    Lua("assert(not pcall(retainedMethod, retainedTable, 10))");
}

TEST_F(UiLifecycleReview, CaughtAutoloadMethodFailureRetiresReceiverUiAndRejectsCachedCalls) {
    ASSERT_TRUE(ui->Close(owner, document, error));
    ASSERT_TRUE(scripts->BeginPlay(root, error));
    Start("Faulting", R"(
        function OnStart()
            doc = assert(UI.LoadDocument('game/panel.rml'))
            assert(doc:Show('modal'))
        end
        function Fail() error('guarded autoload failure') end
    )");
    Lua(R"(
        cached = Autoloads.Faulting.Fail
        local ok, why = pcall(cached)
        assert(not ok and string.find(why, 'autoload=Faulting', 1, true))
        assert(string.find(why, 'guarded autoload failure', 1, true))
        assert(not Autoloads.Faulting.doc:IsValid())
        assert(not pcall(cached))
    )");
    EXPECT_FALSE(ui->GetCaptureState().keyboard);
    EXPECT_EQ(UiRuntimeAccess::Inspect(*ui).documents, 0u);
}
} // namespace
