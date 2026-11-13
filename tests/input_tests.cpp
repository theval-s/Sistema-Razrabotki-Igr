#include <gtest/gtest.h>

#include <memory>

#include <engine/core/FrameContext.hpp>
#include <engine/input/CursorPolicy.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32PhysicalKey.hpp>

using namespace engine;

TEST(Input, PhysicalScanCodesAndAxis) {
    EXPECT_EQ(PhysicalKeyFromWin32Scan(0x11, false), PhysicalKey::W);
    EXPECT_EQ(PhysicalKeyFromWin32Scan(0x1d, true), PhysicalKey::RightControl);
    EXPECT_EQ(PhysicalKeyFromWin32Scan(0x1d, false), PhysicalKey::LeftControl);
    EXPECT_EQ(PhysicalKeyFromWin32Scan(0x1c, true), PhysicalKey::NumpadEnter);

    InputSubsystem input;
    auto& bindings = input.DefineContext("Game");
    bindings.BindAxis2D("Move", PhysicalKey::W, {0, 1});
    bindings.BindAxis2D("Move", PhysicalKey::S, {0, -1});
    bindings.BindAxis2D("Move", PhysicalKey::D, {1, 0});
    bindings.BindAction("Jump", InputSubsystem::Binding::Keyboard(PhysicalKey::Space));
    auto game = input.Activate("Game");
    input.OnPhysicalKey(PhysicalKeyFromWin32Scan(0x11, false), true);
    input.OnPhysicalKey(PhysicalKey::D, true);
    input.OnPhysicalKey(PhysicalKey::Space, true);
    input.OnPhysicalKey(PhysicalKey::Space, false); // tap entirely between frames
    input.Update(FrameContext{});
    EXPECT_NEAR(game.Axis2D("Move").Length(), 1.0f, 0.0001f);
    EXPECT_TRUE(game.Pressed("Jump"));
    EXPECT_TRUE(game.Released("Jump"));
    EXPECT_FALSE(game.Down("Jump"));
    input.OnPhysicalKey(PhysicalKey::S, true);
    input.Update(FrameContext{});
    EXPECT_FLOAT_EQ(game.Axis2D("Move").y, 0);
}

TEST(Input, HigherContextBlocksAndUnblockingRequiresFreshPress) {
    InputSubsystem input;
    auto& gameBindings = input.DefineContext("Game");
    gameBindings.BindAxis1D("Move", PhysicalKey::W, 1);
    auto game = input.Activate("Game");
    input.OnPhysicalKey(PhysicalKey::W, true);
    input.Update(FrameContext{});
    EXPECT_FLOAT_EQ(game.Axis1D("Move"), 1);
    input.DefineContext("Dialog");
    auto dialog = input.Activate("Dialog", 100, true);
    EXPECT_FLOAT_EQ(game.Axis1D("Move"), 0);
    dialog.Reset();
    EXPECT_FLOAT_EQ(game.Axis1D("Move"), 0);
    input.OnPhysicalKey(PhysicalKey::W, false);
    input.Update(FrameContext{});
    input.OnPhysicalKey(PhysicalKey::W, true);
    input.Update(FrameContext{});
    EXPECT_FLOAT_EQ(game.Axis1D("Move"), 1);
    input.OnFocusLost();
    input.Update(FrameContext{});
    EXPECT_FLOAT_EQ(game.Axis1D("Move"), 0);
}

TEST(Input, MouseBlockingAndContextLifetime) {
    InputSubsystem input;
    auto& bindings = input.DefineContext("Viewport");
    bindings.BindAction("Look", InputSubsystem::Binding::Mouse(MouseButton::Right));
    auto viewport = input.Activate("Viewport");
    input.OnMouseButton(MouseButton::Right, true);
    input.OnMouseMove(4, -2);
    input.Update(FrameContext{});
    EXPECT_TRUE(viewport.Down("Look"));
    EXPECT_FLOAT_EQ(viewport.MouseDelta().x, 4);
    input.DefineContext("Popup");
    auto popup = input.Activate("Popup", 10, false, true);
    EXPECT_FALSE(viewport.Down("Look"));
    EXPECT_FLOAT_EQ(viewport.MouseDelta().x, 0);
    popup.Reset();
    EXPECT_FALSE(viewport.Down("Look"));
    input.OnMouseButton(MouseButton::Right, false);
    input.Update(FrameContext{});
    input.OnMouseButton(MouseButton::Right, true);
    input.Update(FrameContext{});
    EXPECT_TRUE(viewport.Pressed("Look"));
    viewport.Reset();
    EXPECT_FALSE(viewport.Down("Look"));
}

TEST(Input, ContextCanOutliveItsSubsystem) {
    InputSubsystem::Context controls;
    {
        auto input = std::make_unique<InputSubsystem>();
        input->DefineContext("Game").BindAction("Jump",
            InputSubsystem::Binding::Keyboard(PhysicalKey::Space));
        controls = input->Activate("Game");
        EXPECT_TRUE(static_cast<bool>(controls));
    }
    EXPECT_FALSE(static_cast<bool>(controls));
    EXPECT_FALSE(controls.Down("Jump"));
    EXPECT_FLOAT_EQ(controls.Axis1D("Move"), 0);
    controls.Reset();
}

namespace {
void UiKey(InputSubsystem& input, bool down, bool repeat = false) {
    input.OnKey(Key::W, down);
    input.OnPhysicalKey(PhysicalKey::W, down);
    InputUiEvent event;
    event.payload = InputUiKey{Key::W, PhysicalKey::W, InputUiNavigationKey::None, down, repeat};
    input.OnUiEvent(std::move(event));
}
InputSubsystem::Context GameControls(InputSubsystem& input, int priority = 0) {
    auto& bindings = input.DefineContext("Game");
    bindings.BindAction("Move", InputSubsystem::Binding::Keyboard(PhysicalKey::W));
    bindings.BindAxis1D("Axis", PhysicalKey::W, 1);
    bindings.BindAxis2D("Axis2", PhysicalKey::W, {0, 1});
    bindings.BindAction("Fire", InputSubsystem::Binding::Mouse(MouseButton::Left));
    return input.Activate("Game", priority);
}
}

TEST(Input, UiEventsOwnOrderedRepeatsAndDoNotFeedGameplayTwice) {
    InputSubsystem input;
    auto game = GameControls(input);
    UiKey(input, true);
    UiKey(input, true, true);
    InputUiEvent text;
    text.payload = InputUiText{"\xd0\x96\xf0\x9f\x98\x80"};
    input.OnUiEvent(text);
    std::get<InputUiText>(text.payload).utf8.clear();
    UiKey(input, false);
    input.Update({});
    const auto events = input.GetUiEvents();
    ASSERT_EQ(events.size(), 4u);
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_NE(events[i].sequence, 0u);
        if (i) EXPECT_GT(events[i].sequence, events[i - 1].sequence);
    }
    EXPECT_TRUE(std::get<InputUiKey>(events[1].payload).repeat);
    EXPECT_EQ(std::get<InputUiText>(events[2].payload).utf8, "\xd0\x96\xf0\x9f\x98\x80");
    EXPECT_TRUE(game.Pressed("Move"));
    EXPECT_TRUE(game.Released("Move"));
    EXPECT_FALSE(game.Down("Move"));
    const auto last = events.back().sequence;
    input.Update({});
    EXPECT_TRUE(input.GetUiEvents().empty());
    EXPECT_FALSE(game.Pressed("Move"));
    InputUiEvent onlyUi;
    onlyUi.payload = InputUiKey{Key::W, PhysicalKey::W, InputUiNavigationKey::None, true, false};
    input.OnUiEvent(onlyUi);
    input.Update({});
    ASSERT_EQ(input.GetUiEvents().size(), 1u);
    EXPECT_GT(input.GetUiEvents()[0].sequence, last);
    EXPECT_FALSE(game.Down("Move"));
}

TEST(Input, UiConsumedTapMasksRawAndActionsForWholeFrame) {
    InputSubsystem input;
    auto game = GameControls(input);
    UiKey(input, true);
    UiKey(input, false);
    input.Update({});
    input.ConsumeUiEvent(input.GetUiEvents().front().sequence);
    EXPECT_FALSE(input.IsDown(Key::W));
    EXPECT_FALSE(input.WasPressed(Key::W));
    EXPECT_FALSE(input.WasReleased(Key::W));
    EXPECT_FALSE(game.Pressed("Move"));
    EXPECT_FALSE(game.Released("Move"));
    input.SetUiCapture(true, false);
    input.SetUiCapture(false, false);
    EXPECT_FALSE(game.Pressed("Move"));
    input.Update({});
    UiKey(input, true);
    input.Update({});
    EXPECT_TRUE(input.WasPressed(Key::W));
    EXPECT_TRUE(game.Pressed("Move"));
}

TEST(Input, UiCaptureIsReservedAndStickyAcrossSameFrameOpenClose) {
    InputSubsystem input;
    auto game = GameControls(input, 2147483647);
    UiKey(input, true);
    input.OnMouseButton(MouseButton::Left, true);
    input.OnMouseButton(MouseButton::Left, false);
    input.OnMouseMove(7, 9);
    input.OnMousePosition(12, 34);
    input.OnMouseWheel(2);
    input.Update({});
    input.SetUiCapture(true, true);
    input.SetUiCapture(false, false);
    EXPECT_FALSE(input.IsDown(Key::W));
    EXPECT_FALSE(input.WasPressed(Key::W));
    EXPECT_FALSE(input.WasPressed(MouseButton::Left));
    EXPECT_FALSE(input.WasReleased(MouseButton::Left));
    EXPECT_FALSE(game.Down("Move"));
    EXPECT_FALSE(game.Pressed("Fire"));
    EXPECT_FALSE(game.Released("Fire"));
    EXPECT_FLOAT_EQ(game.Axis1D("Axis"), 0);
    EXPECT_FLOAT_EQ(game.Axis2D("Axis2").y, 0);
    EXPECT_FLOAT_EQ(input.GetMousePosition().x, 0);
    EXPECT_FLOAT_EQ(input.GetMouseDelta().x, 0);
    EXPECT_FLOAT_EQ(input.GetWheelDelta(), 0);
    EXPECT_FLOAT_EQ(game.MouseDelta().y, 0);
    input.Update({});
    EXPECT_FALSE(game.Down("Move")); // held key still suppressed, raw truth retained
    UiKey(input, false);
    input.Update({});
    EXPECT_FALSE(input.WasReleased(Key::W));
    EXPECT_FALSE(game.Released("Move"));
    UiKey(input, true);
    input.Update({});
    EXPECT_TRUE(game.Pressed("Move"));
    EXPECT_FLOAT_EQ(game.Axis1D("Axis"), 1);
}

TEST(Input, ConsumedPointerAndHeldButtonCannotLeakThroughRawReaders) {
    InputSubsystem input;
    auto game = GameControls(input);
    input.OnMouseButton(MouseButton::Left, true);
    InputUiEvent event;
    event.pointerX = 50; event.pointerY = 60; event.hasPointerPosition = true;
    event.payload = InputUiPointerButton{MouseButton::Left, true};
    input.OnUiEvent(event);
    input.OnMouseMove(5, 6); input.OnMousePosition(50, 60);
    event.payload = InputUiPointerMove{}; input.OnUiEvent(event);
    input.OnMouseWheel(3);
    event.payload = InputUiWheel{0, 3}; input.OnUiEvent(event);
    input.Update({});
    ASSERT_EQ(input.GetUiEvents().size(), 3u);
    for (const auto& queued : input.GetUiEvents()) input.ConsumeUiEvent(queued.sequence);
    EXPECT_FALSE(input.IsDown(MouseButton::Left));
    EXPECT_FALSE(input.WasPressed(MouseButton::Left));
    EXPECT_FALSE(game.Down("Fire"));
    EXPECT_FLOAT_EQ(input.GetMousePosition().x, 0);
    EXPECT_FLOAT_EQ(input.GetMouseDelta().x, 0);
    EXPECT_FLOAT_EQ(game.WheelDelta(), 0);
    input.Update({});
    EXPECT_FALSE(input.IsDown(MouseButton::Left));
    input.OnMouseButton(MouseButton::Left, false);
    input.Update({});
    EXPECT_FALSE(input.WasReleased(MouseButton::Left));
    input.OnMouseButton(MouseButton::Left, true);
    input.Update({});
    EXPECT_TRUE(game.Pressed("Fire"));
    EXPECT_TRUE(input.WasPressed(MouseButton::Left));
}

TEST(Input, CapturePersistsAcrossLatchAndFocusResetCancelsQueuedText) {
    InputSubsystem input;
    auto game = GameControls(input);
    input.SetUiCapture(true, true);
    UiKey(input, true);
    input.Update({});
    input.SetUiCapture(false, false);
    EXPECT_FALSE(game.Down("Move"));
    InputUiEvent event;
    event.payload = InputUiText{"stale"}; input.OnUiEvent(event);
    const auto sequence = input.GetUiEvents().back().sequence;
    input.OnFocusLost();
    // Published spans remain valid until latch/reset, even for sent focus messages.
    EXPECT_EQ(input.GetUiEvents().back().sequence, sequence);
    input.Update({});
    ASSERT_EQ(input.GetUiEvents().size(), 1u);
    EXPECT_FALSE(std::get<InputUiFocus>(input.GetUiEvents()[0].payload).focused);
    EXPECT_FALSE(game.Down("Move"));
    UiKey(input, true);
    input.Update({});
    EXPECT_TRUE(game.Pressed("Move"));
    input.Reset();
    EXPECT_FALSE(input.IsDown(Key::W));
    EXPECT_TRUE(input.GetUiEvents().empty());
    input.Update({});
    ASSERT_EQ(input.GetUiEvents().size(), 1u);
    EXPECT_GT(input.GetUiEvents()[0].sequence, sequence);
    EXPECT_FALSE(std::get<InputUiFocus>(input.GetUiEvents()[0].payload).focused);
}

TEST(Input, CursorModeRequestOutlivesFocusLossAndReset) {
    InputSubsystem input;
    EXPECT_EQ(input.GetCursorMode(), CursorMode::Normal);
    input.SetCursorMode(CursorMode::Locked);
    // Hosts drop the cursor on focus loss, but put the request back when the focus returns.
    input.OnFocusLost();
    input.Reset();
    input.Update(FrameContext{});
    EXPECT_EQ(input.GetCursorMode(), CursorMode::Locked);
}

TEST(Input, CursorPolicySuspendsAndRestoresEveryRequestWithoutLosingUiDrag) {
    for (const auto request : {CursorMode::Normal, CursorMode::Hidden, CursorMode::Locked}) {
        auto p = ResolveCursorPolicy(request, true, false, false, false);
        EXPECT_EQ(p.mode, request);
        EXPECT_EQ(p.grab, request == CursorMode::Locked);
        for (int requirement = 0; requirement < 3; ++requirement) {
            p = ResolveCursorPolicy(request, true, requirement == 0, requirement == 1, requirement == 2);
            EXPECT_EQ(p.mode, CursorMode::Normal);
            EXPECT_TRUE(p.interactiveUi);
            EXPECT_EQ(p.grab, requirement == 0);
        }
        p = ResolveCursorPolicy(request, false, true, true, true);
        EXPECT_EQ(p.mode, CursorMode::Normal);
        EXPECT_FALSE(p.grab);
        p = ResolveCursorPolicy(request, true, false, false, false);
        EXPECT_EQ(p.mode, request);
    }
}
