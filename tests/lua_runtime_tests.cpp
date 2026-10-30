#include <engine/scripting/LuaRuntime.hpp>

#include <gtest/gtest.h>

#include <string>

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
