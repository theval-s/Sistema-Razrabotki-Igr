
#pragma once

#include <engine/engine_export.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace engine {

    class ENGINE_API LuaRuntime final {
    public:
        LuaRuntime();
        ~LuaRuntime();

        LuaRuntime(const LuaRuntime&) = delete;
        LuaRuntime& operator=(const LuaRuntime&) = delete;

        LuaRuntime(LuaRuntime&&) = delete;
        LuaRuntime& operator=(LuaRuntime&&) = delete;

        bool Initialize(std::string& error);

        void Shutdown() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;

        bool ExecuteFile(
            const std::filesystem::path& path,
            std::string& error
        );

        bool ExecuteString(
            std::string_view source,
            std::string_view chunkName,
            std::string& error
        );

        bool CallFunction(
            std::string_view functionName,
            std::string& error
        );

        bool CallUpdate(
            double deltaSeconds,
            std::string& error
        );

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
