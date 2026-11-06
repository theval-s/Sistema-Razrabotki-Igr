#include <gtest/gtest.h>

#include <memory>

#include <engine/core/FrameContext.hpp>
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
