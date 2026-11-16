#include <engine/animation/AnimationSubsystem.hpp>
#include <engine/engine.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/assets/AssetPathResolver.hpp>

#include <gtest/gtest.h>

TEST(AnimationLuaBindings, ControlsAndInvalidatedEntities) {
    struct RootScope {
        std::filesystem::path previous = engine::AssetPathResolver::GetGameRoot();
        RootScope() { engine::AssetPathResolver::SetGameRoot(std::filesystem::path(SRI_SOURCE_ROOT) / "example_game"); }
        ~RootScope() { engine::AssetPathResolver::SetGameRoot(previous); }
    } rootScope;
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    ASSERT_NE(host.AddSubsystem<engine::AnimationSubsystem>(), nullptr);
    ASSERT_TRUE(host.Initialize());
    const auto raw = host.GetScene().CreateEntity("animated");
    ASSERT_NE(host.GetScene().AddComponent<engine::Animator>(raw), nullptr);
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize({}, host, error)) << error;
    ASSERT_TRUE(runtime.ExecuteString(R"(
        retained = Scene.FindByName('animated')
        assert(retained:animation_state() == nil)
        local ok, err = retained:set_animation_speed(2)
        assert(ok and err == '')
        local state = retained:animation_state()
        assert(state and state.playing and not state.ready and not state.transitioning)
        assert(state.time_seconds == 0 and state.duration == 0 and state.error == '')
        state.playing = false
        assert(retained:animation_state().playing)
        assert(retained:pause_animation())
        assert(not retained:animation_state().playing)
        assert(retained:pause_animation(false))
        assert(retained:animation_state().playing)
        ok, err = retained:set_animation_speed(-1)
        assert(not ok and #err > 0)
        ok, err = retained:seek_animation(0)
        assert(not ok and #err > 0)
        ok, err = retained:play_animation({id='invalid', path='game/missing'})
        assert(not ok and #err > 0)
        assert(retained:stop_animation())
        assert(not retained:animation_state().playing)
    )", "animation assertions", error)) << error;
    host.GetScene().DestroyEntity(raw);
    ASSERT_TRUE(runtime.ExecuteString(R"(
        assert(retained:animation_state() == nil)
        local ok, err = retained:stop_animation()
        assert(not ok and #err > 0)
    )", "stale animation handle", error)) << error;
    runtime.Shutdown();
    host.Shutdown();
}
