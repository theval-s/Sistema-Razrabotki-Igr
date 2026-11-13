#include <gtest/gtest.h>

#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/scripting/LuaBindings.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <engine/scripting/LuaGameplayBindings.hpp>

#include <memory>
#include <limits>

namespace {
using namespace engine;
using DirectX::SimpleMath::Vector3;

struct GameplayBindingTests : testing::Test {
    Engine host{EngineConfig{.taskThreads = 1}};
    InputSubsystem* input = host.AddSubsystem<InputSubsystem>();
    LuaBindingsContext context{.engine = &host};
    sol::state lua;

    void SetUp() override {
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string);
        lua.new_usertype<Vector3>("Vector3", sol::constructors<Vector3(), Vector3(float, float, float)>(),
            "x", &Vector3::x, "y", &Vector3::y, "z", &Vector3::z);
        lua.create_named_table("Scene");
        RegisterLuaBindings(lua, context);
        RegisterGameplayBindings(lua, context);
    }
    void Lua(const char* source) {
        const auto result = lua.safe_script(source, sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            ADD_FAILURE() << error.what();
        }
    }
    void Latch() { input->Update(FrameContext{}); }
};

TEST_F(GameplayBindingTests, LuaOnlyBindingsAndDefaultContextShareInjectedFrameState) {
    Lua(R"(
        game = Input.GetContext('Gameplay')
        assert(game:IsValid())
        game:BindAction('Jump', PhysicalKey.Space)
        game:BindMouseAction('Fire', MouseButton.Left)
        game:BindAxis1D('Forward', PhysicalKey.W, 1)
        game:BindAxis2D('Move', PhysicalKey.W, Vector2.new(0, 1))
        game:BindAxis2D('Move', PhysicalKey.D, Vector2.new(1, 0))
        assert(not Input.Down('Jump'))
        assert(not pcall(function() game:BindAction('Bad', 999) end))
        assert(not pcall(function() game:BindMouseAction('Bad', -1) end))
        assert(not pcall(function() game:BindAxis1D('Bad', PhysicalKey.W, math.huge) end))
    )");
    input->OnPhysicalKey(PhysicalKey::Space, true);
    input->OnPhysicalKey(PhysicalKey::W, true);
    input->OnPhysicalKey(PhysicalKey::D, true);
    input->OnMouseButton(MouseButton::Left, true);
    input->OnMouseMove(4, -3);
    input->OnMouseWheel(2);
    Latch();
    Lua(R"(
        assert(Input.Pressed('Jump') and game:Pressed('Jump') and Input.Down('Jump'))
        assert(game:Down('Fire') and Input.Pressed('Fire'))
        assert(Input.Axis1D('Forward') == 1)
        assert(math.abs(Input.Axis2D('Move'):Length() - 1) < 0.00001)
        assert(Input.MouseDelta().x == 4 and game:MouseDelta().y == -3)
        assert(Input.MouseWheel() == 2 and game:MouseWheel() == 2)
    )");
    Latch();
    Lua("assert(Input.Down('Jump') and not Input.Pressed('Jump')); assert(Input.MouseWheel() == 0)");
    input->OnPhysicalKey(PhysicalKey::Space, false);
    input->OnMouseButton(MouseButton::Left, false);
    Latch();
    Lua("assert(Input.Released('Jump') and game:Released('Fire') and not Input.Down('Fire'))");
}

TEST_F(GameplayBindingTests, RegistrationActivatesDefaultBeforeFirstInputQuery) {
    input->DefineContext("Gameplay").BindAction("Jump", InputSubsystem::Binding::Keyboard(PhysicalKey::Space));
    input->OnPhysicalKey(PhysicalKey::Space, true);
    Latch();
    Lua("assert(Input.Pressed('Jump') and Input.Down('Jump'))");
}

TEST_F(GameplayBindingTests, CursorModeIsTheGamesRequest) {
    Lua(R"(
        assert(Input.CursorMode == CursorMode)
        assert(Input.GetCursorMode() == CursorMode.Normal)
        Input.SetCursorMode(CursorMode.Locked)
        assert(Input.GetCursorMode() == CursorMode.Locked)
        assert(not pcall(Input.SetCursorMode, 7))
        assert(not pcall(Input.SetCursorMode, -1))
        assert(Input.GetCursorMode() == CursorMode.Locked)
    )");
    EXPECT_EQ(input->GetCursorMode(), CursorMode::Locked);
    input->SetCursorMode(CursorMode::Hidden);
    Lua("assert(Input.GetCursorMode() == CursorMode.Hidden)");
}

TEST_F(GameplayBindingTests, MousePositionIsLatchedAbsoluteAndIndependentOfMotion) {
    Lua(R"(
        game = Input.GetContext('Gameplay')
        assert(Input.MousePosition() == Vector2.new())
    )");
    input->OnMousePosition(80, 60);
    input->OnMousePosition(120.5f, 75.25f);
    Lua("assert(Input.MousePosition() == Vector2.new())");
    Latch();
    EXPECT_FLOAT_EQ(input->GetMousePosition().x, 120.5f);
    Lua(R"(
        assert(Input.MousePosition() == Vector2.new(120.5, 75.25))
        assert(game:MousePosition() == Input.MousePosition())
        assert(Input.MouseDelta() == Vector2.new())
    )");
    input->OnMouseMove(9, -4);
    input->OnMouseWheel(1);
    Latch();
    Lua(R"(
        assert(Input.MousePosition() == Vector2.new(120.5, 75.25))
        assert(Input.MouseDelta() == Vector2.new(9, -4))
    )");
    input->OnMousePosition(std::numeric_limits<float>::infinity(), 5);
    input->OnMousePosition(5, std::numeric_limits<float>::quiet_NaN());
    Latch();
    Lua("assert(Input.MousePosition() == Vector2.new(120.5, 75.25))");
    // Captured pointers may leave the viewport; preserve signed coordinates.
    input->OnMousePosition(-12, 500);
    Latch();
    Lua("assert(Input.MousePosition() == Vector2.new(-12, 500))");
    input->Reset();
    Lua("assert(Input.MousePosition() == Vector2.new() and game:MousePosition() == Vector2.new())");
    Latch();
    Lua("assert(Input.MousePosition() == Vector2.new())");
}

TEST_F(GameplayBindingTests, MousePositionRespectsDeviceBlockingReleaseAndFocusLoss) {
    input->DefineContext("HostPosition");
    auto hostControls = input->Activate("HostPosition", -10);
    input->OnMousePosition(140, 90);
    Latch();
    Lua(R"(
        game = Input.GetContext('Gameplay')
        modal = Input.GetContext('PositionDialog')
        assert(modal:Capture(100, true, false))
        assert(Input.MousePosition() == Vector2.new(140, 90))
    )");
    EXPECT_FLOAT_EQ(hostControls.MousePosition().x, 140);
    Lua(R"(
        assert(modal:Capture(100, false, true))
        assert(Input.MousePosition() == Vector2.new())
        assert(game:MousePosition() == Vector2.new())
        assert(modal:MousePosition() == Vector2.new(140, 90))
    )");
    EXPECT_FLOAT_EQ(hostControls.MousePosition().Length(), 0);
    input->OnMousePosition(180, 100);
    Latch();
    Lua(R"(
        assert(Input.MousePosition() == Vector2.new())
        assert(modal:MousePosition() == Vector2.new(180, 100))
        modal:Release()
        assert(modal:MousePosition() == Vector2.new())
        assert(Input.MousePosition() == Vector2.new(180, 100))
    )");
    input->OnFocusLost();
    Latch();
    Lua("assert(Input.MousePosition() == Vector2.new() and game:MousePosition() == Vector2.new())");
    EXPECT_FLOAT_EQ(hostControls.MousePosition().Length(), 0);
    Latch();
    Lua("assert(Input.MousePosition() == Vector2.new())");
    input->OnMousePosition(40, 50); // host resumes feeding after focus is restored
    Latch();
    Lua("assert(Input.MousePosition() == Vector2.new(40, 50))");
}

TEST_F(GameplayBindingTests, NonblockingReadersAndExplicitModalCaptureRespectHostContexts) {
    input->DefineContext("Host").BindAction("Jump", InputSubsystem::Binding::Keyboard(PhysicalKey::Space));
    auto hostControls = input->Activate("Host", -10);
    Lua(R"(
        game = Input.GetContext('Gameplay')
        game:BindAction('Jump', PhysicalKey.Space)
        game:BindMouseAction('Fire', MouseButton.Left)
        reader = Input.GetContext('Reader')
    )");
    input->OnPhysicalKey(PhysicalKey::Space, true);
    input->OnMouseButton(MouseButton::Left, true);
    input->OnMouseMove(7, 2);
    Latch();
    EXPECT_TRUE(hostControls.Down("Jump"));
    Lua(R"(
        assert(Input.Down('Jump') and Input.Down('Fire'))
        modal = Input.DefineContext('Dialog')
        assert(modal:Capture(100, true, false))
        assert(not Input.Down('Jump'))
        assert(Input.Down('Fire') and Input.MouseDelta().x == 7)
    )");
    EXPECT_FALSE(hostControls.Down("Jump"));
    Lua(R"(
        assert(modal:Capture(100, true, true))
        assert(not Input.Down('Fire') and Input.MouseDelta():Length() == 0)
        assert(Input.MouseWheel() == 0)
        modal:Release()
        assert(not modal:IsValid())
        assert(not Input.Down('Jump') and not Input.Down('Fire'))
    )");
    EXPECT_FALSE(hostControls.Down("Jump"));
    input->OnPhysicalKey(PhysicalKey::Space, false);
    input->OnMouseButton(MouseButton::Left, false);
    Latch();
    input->OnPhysicalKey(PhysicalKey::Space, true);
    input->OnMouseButton(MouseButton::Left, true);
    Latch();
    EXPECT_TRUE(hostControls.Down("Jump"));
    Lua("assert(Input.Pressed('Jump') and Input.Pressed('Fire'))");
}

TEST_F(GameplayBindingTests, GarbageCollectionReleasesModalLease) {
    Lua(R"(
        Input.GetContext('Gameplay'):BindAction('Jump', PhysicalKey.Space)
        do
            local modal = Input.GetContext('Temporary')
            assert(modal:Capture(100, true, true))
        end
        collectgarbage('collect')
        collectgarbage('collect')
    )");
    input->OnPhysicalKey(PhysicalKey::Space, true);
    Latch();
    Lua("assert(Input.Down('Jump'))");
}

TEST_F(GameplayBindingTests, ReleasingNonblockingReadersPreservesHeldHostAndGameplayInput) {
    auto& bindings = input->DefineContext("Host");
    bindings.BindAction("Jump", InputSubsystem::Binding::Keyboard(PhysicalKey::Space));
    bindings.BindAction("Fire", InputSubsystem::Binding::Mouse(MouseButton::Left));
    auto hostControls = input->Activate("Host", -10);
    Lua(R"(
        game = Input.GetContext('Gameplay')
        game:BindAction('Jump', PhysicalKey.Space)
        game:BindMouseAction('Fire', MouseButton.Left)
        reader = Input.GetContext('Reader')
        collected = Input.GetContext('CollectedReader')
    )");
    input->OnPhysicalKey(PhysicalKey::Space, true);
    input->OnMouseButton(MouseButton::Left, true);
    Latch();
    Lua(R"(
        reader:Release()
        collected = nil
        collectgarbage('collect')
        collectgarbage('collect')
        assert(Input.Down('Jump') and Input.Down('Fire'))
    )");
    EXPECT_TRUE(hostControls.Down("Jump"));
    EXPECT_TRUE(hostControls.Down("Fire"));
    Latch();
    Lua("assert(Input.Down('Jump') and Input.Down('Fire'))");
    EXPECT_TRUE(hostControls.Down("Jump"));
    EXPECT_TRUE(hostControls.Down("Fire"));
}

TEST_F(GameplayBindingTests, RemovingKeyboardModalSuppressesOnlyLowerPriorityKeyboardReaders) {
    auto& bindings = input->DefineContext("Host");
    bindings.BindAction("Jump", InputSubsystem::Binding::Keyboard(PhysicalKey::Space));
    bindings.BindAction("Fire", InputSubsystem::Binding::Mouse(MouseButton::Left));
    auto lower = input->Activate("Host", -10);
    auto equal = input->Activate("Host", 100);
    auto higher = input->Activate("Host", 200);
    Lua(R"(
        game = Input.GetContext('Gameplay')
        game:BindAction('Jump', PhysicalKey.Space)
        game:BindMouseAction('Fire', MouseButton.Left)
        modal = Input.GetContext('KeyboardDialog')
        assert(modal:Capture(100, true, false))
    )");
    input->OnPhysicalKey(PhysicalKey::Space, true);
    input->OnMouseButton(MouseButton::Left, true);
    Latch();
    EXPECT_FALSE(lower.Down("Jump"));
    EXPECT_TRUE(equal.Down("Jump"));
    EXPECT_TRUE(higher.Down("Jump"));
    Lua(R"(
        assert(not Input.Down('Jump') and Input.Down('Fire'))
        modal:Release()
        assert(not Input.Down('Jump') and Input.Down('Fire'))
    )");
    EXPECT_FALSE(lower.Down("Jump"));
    EXPECT_TRUE(lower.Down("Fire"));
    EXPECT_TRUE(equal.Down("Jump"));
    EXPECT_TRUE(higher.Down("Jump"));
    input->OnPhysicalKey(PhysicalKey::Space, false);
    Latch();
    input->OnPhysicalKey(PhysicalKey::Space, true);
    Latch();
    EXPECT_TRUE(lower.Pressed("Jump"));
    Lua("assert(Input.Pressed('Jump') and Input.Down('Fire'))");
}

TEST_F(GameplayBindingTests, RemovingMouseModalDoesNotSuppressHeldKeyboardInput) {
    Lua(R"(
        game = Input.GetContext('Gameplay')
        game:BindAction('Jump', PhysicalKey.Space)
        game:BindMouseAction('Fire', MouseButton.Left)
        modal = Input.GetContext('MouseDialog')
        assert(modal:Capture(100, false, true))
    )");
    input->OnPhysicalKey(PhysicalKey::Space, true);
    input->OnMouseButton(MouseButton::Left, true);
    Latch();
    Lua(R"(
        assert(Input.Down('Jump') and not Input.Down('Fire'))
        modal:Release()
        assert(Input.Down('Jump') and not Input.Down('Fire'))
    )");
    input->OnMouseButton(MouseButton::Left, false);
    Latch();
    input->OnMouseButton(MouseButton::Left, true);
    Latch();
    Lua("assert(Input.Down('Jump') and Input.Pressed('Fire'))");
}

TEST_F(GameplayBindingTests, ContextAndDefinitionWrappersSurviveDestroyedSubsystem) {
    auto temporaryHost = std::make_unique<Engine>(EngineConfig{.taskThreads = 1});
    auto* temporaryInput = temporaryHost->AddSubsystem<InputSubsystem>();
    context.engine = temporaryHost.get();
    temporaryInput->OnMousePosition(33, 44);
    temporaryInput->Update(FrameContext{});
    Lua(R"(
        old = Input.GetContext('Temporary')
        old:BindAction('Jump', PhysicalKey.Space)
        assert(old:IsValid())
        assert(old:MousePosition() == Vector2.new(33, 44))
    )");
    temporaryHost.reset();
    context.engine = nullptr;
    Lua(R"(
        assert(not old:IsValid() and not old:Down('Jump') and not old:Pressed('Jump'))
        assert(not old:Released('Jump') and old:Axis1D('Move') == 0)
        assert(old:Axis2D('Move'):Length() == 0 and old:MouseDelta():Length() == 0)
        assert(old:MousePosition():Length() == 0 and Input.MousePosition():Length() == 0)
        assert(old:MouseWheel() == 0 and not old:Capture(100, true, true))
        assert(not pcall(function() old:BindAction('Jump', PhysicalKey.Space) end))
        old:Release()
        assert(Input.GetContext('Missing') == nil and not Input.Down('Jump'))
        assert(Input.Axis2D('Move'):Length() == 0)
    )");
    context.engine = &host;
    Lua("assert(Input.GetContext('Gameplay'):IsValid()); assert(not old:IsValid())");
}

TEST_F(GameplayBindingTests, MathExtendsExistingTypesAndPreservesSimpleMathInvariants) {
    const auto entity = host.GetScene().CreateEntity();
    lua["entity"] = LuaEntityHandle::FromScene(host.GetScene(), entity);
    Lua(R"(
        local x, y = Vector3.new(1,0,0), Vector3.new(0,1,0)
        local v = (x + y) * 2 - x
        assert(v.x == 1 and v.y == 2 and (-v).x == -1)
        assert((2 * x / 2) == x and x:Dot(y) == 0)
        assert(x:Cross(y) == Vector3.new(0,0,1))
        assert(Vector3.Lerp(x, y, 0.5) == Vector3.new(0.5,0.5,0))
        assert(math.abs(v:Normalize():Length() - 1) < 0.00001)
        assert(v.x == 1 and v.y == 2) -- normalization returns a value
        assert(not pcall(function() return v / 0 end))
        assert(not pcall(function() return Vector3.new():Normalize() end))
        local a = Vector2.new(3,4)
        assert(a:Length() == 5 and (2*a/2) == a)
        assert(Vector2.Lerp(a, -a, 0.5):Length() == 0)
        local q = Quaternion.AxisAngle(Vector3.new(0,2,0), math.pi/2)
        local rotated = q * x
        assert(math.abs(rotated:Length() - 1) < 0.00001)
        assert((q:Rotate(x) - rotated):Length() < 0.00001)
        local twice = (q*q)*x
        assert((twice + x):Length() < 0.00001)
        local half = Quaternion.Slerp(Quaternion.Identity(), q, 0.5)
        assert(((half*half):Rotate(x) - rotated):Length() < 0.00001)
        assert((Quaternion.Slerp(Quaternion.Identity(), q, 0):Rotate(x) - x):Length() < 0.00001)
        assert((Quaternion.Slerp(Quaternion.Identity(), q, 1):Rotate(x) - rotated):Length() < 0.00001)
        local euler = Quaternion.Euler(0.2, 0.3, 0.4)
        local ypr = Quaternion.YawPitchRoll(0.3, 0.2, 0.4)
        assert((euler:Rotate(x) - ypr:Rotate(x)):Length() < 0.00001)
        assert(math.abs(Quaternion.new(0,0,0,2):Normalize().w - 1) < 0.00001)
        assert(not pcall(function() return Quaternion.new(0,0,0,0):Normalize() end))
        entity:SetPosition(v)
        entity:SetRotation(q)
        assert((entity:GetPosition() - v):Length() < 0.00001)
        assert((entity:GetRotation():Rotate(x) - rotated):Length() < 0.00001)
    )");
    // Check the convention against SimpleMath itself, rather than assuming handedness.
    const auto expected = Vector3::Transform(Vector3(1, 0, 0),
        DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(0.3f, 0.2f, 0.4f));
    lua["expected"] = expected;
    Lua("assert((Quaternion.Euler(0.2,0.3,0.4):Rotate(Vector3.new(1,0,0)) - expected):Length() < 0.00001)");
}

TEST_F(GameplayBindingTests, PhysicsAbsentReturnsErrorInsteadOfThrowing) {
    Lua(R"(
        for _, f in ipairs({Physics.SetLinearVelocity, Physics.AddImpulse, Physics.MoveKinematic, Physics.Teleport}) do
            local ok, why = f(nil, Vector3.new(), Quaternion.Identity())
            assert(ok == false and type(why) == 'string' and #why > 0)
        end
    )");
}

TEST_F(GameplayBindingTests, PhysicsValidatesHandlesNumbersRotationsAndForwardsCommands) {
    PhysicsConfig config;
    config.gravity = Vector3::Zero;
    auto* physics = host.AddSubsystem<PhysicsSubsystem>(config);
    ASSERT_NE(physics, nullptr);
    ASSERT_TRUE(host.Initialize());
    auto& scene = host.GetScene();
    const auto dynamic = scene.CreateEntity();
    scene.AddComponent<RigidBody>(dynamic, RigidBody{.motionType = MotionType::Dynamic});
    scene.AddComponent<Collider>(dynamic);
    const auto kinematic = scene.CreateEntity();
    scene.GetTransform(kinematic).position = {10, 0, 0};
    scene.AddComponent<RigidBody>(kinematic, RigidBody{.motionType = MotionType::Kinematic});
    scene.AddComponent<Collider>(kinematic);
    physics->FixedUpdate(FixedUpdateContext{});
    ASSERT_EQ(physics->GetBodyCount(), 2u);
    lua["dynamic"] = LuaEntityHandle::FromScene(scene, dynamic);
    lua["kinematic"] = LuaEntityHandle::FromScene(scene, kinematic);
    Scene other;
    lua["foreign"] = LuaEntityHandle::FromScene(other, other.CreateEntity());
    lua["invalid"] = LuaEntityHandle{};
    Lua(R"(
        local function rejected(ok, why)
            assert(ok == false and type(why) == 'string' and #why > 0)
        end
        rejected(Physics.SetLinearVelocity(foreign, Vector3.new(1,0,0)))
        rejected(Physics.SetLinearVelocity(invalid, Vector3.new()))
        rejected(Physics.SetLinearVelocity(12, Vector3.new()))
        rejected(Physics.SetLinearVelocity(dynamic, {x=1,y=0,z=0}))
        rejected(Physics.SetLinearVelocity(dynamic, Vector3.new(math.huge,0,0)))
        rejected(Physics.AddImpulse(dynamic, Vector3.new(0/0,0,0)))
        rejected(Physics.Teleport(dynamic, Vector3.new(), Quaternion.new(0,0,0,0)))
        rejected(Physics.Teleport(dynamic, Vector3.new(), Quaternion.new(0,0,0,2)))
        rejected(Physics.Teleport(dynamic, Vector3.new(math.huge,0,0), Quaternion.Identity()))
        rejected(Physics.Teleport(dynamic, Vector3.new(), nil))
        rejected(Physics.MoveKinematic(kinematic, Vector3.new(), Quaternion.new(0,0,math.huge,1)))
        rejected(Physics.MoveKinematic(kinematic, Vector3.new(0/0,0,0), Quaternion.Identity()))
        rejected(Physics.MoveKinematic(dynamic, Vector3.new(), Quaternion.Identity()))
        rejected(Physics.AddImpulse(kinematic, Vector3.new(1,0,0)))
        assert(Physics.SetLinearVelocity(dynamic, Vector3.new(1,0,0)))
        local ok, why = Physics.AddImpulse(dynamic, Vector3.new(1,0,0))
        assert(ok and why == '')
        assert(Physics.MoveKinematic(kinematic, Vector3.new(12,0,0), Quaternion.Identity()))
    )");
    physics->FixedUpdate(FixedUpdateContext{});
    EXPECT_GT(scene.GetTransform(dynamic).position.x, 0.f);
    EXPECT_FLOAT_EQ(scene.GetTransform(kinematic).position.x, 12.f);
    Lua("assert(Physics.Teleport(dynamic, Vector3.new(20,3,4), Quaternion.Identity()))");
    EXPECT_FLOAT_EQ(scene.GetTransform(dynamic).position.x, 20.f);
    EXPECT_FLOAT_EQ(scene.GetTransform(dynamic).position.y, 3.f);
    scene.DestroyEntity(dynamic);
    Lua("local ok, why = Physics.Teleport(dynamic, Vector3.new(), Quaternion.Identity()); assert(not ok and #why > 0)");
    physics->Shutdown();
}

TEST_F(GameplayBindingTests, TimeReadsCurrentHostFactsAndHasEnginelessDefaults) {
    host.SetPaused(true);
    Lua("assert(Time.IsPaused()); assert(Time.Delta() == 0)");
    host.SetPaused(false);
    Lua("assert(not Time.IsPaused())");
    lua["delta"] = host.GetFrame().deltaSeconds;
    lua["simulation"] = host.GetFrame().simulationTimeSeconds;
    lua["realDelta"] = host.GetFrame().realDeltaSeconds;
    lua["realTime"] = host.GetFrame().realTimeSeconds;
    Lua("assert(Time.Delta() == delta and Time.SimulationTime() == simulation); assert(Time.RealDelta() == realDelta and Time.RealTime() == realTime)");
    context.engine = nullptr;
    Lua("assert(Time.Delta() == 0 and Time.RealDelta() == 0 and Time.RealTime() == 0 and Time.SimulationTime() == 0 and not Time.IsPaused())");
}
} // namespace
