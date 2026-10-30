
#include <engine/scripting/LuaRuntime.hpp>

#include <sol/sol.hpp>

#include <spdlog/spdlog.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace engine {

    struct LuaRuntime::Impl {
        std::unique_ptr<sol::state> state;
    };

    namespace {

        void SetError(std::string& error, const std::string& message) {
            error = message;
            spdlog::error("[Lua] {}", message);
        }

        bool CheckResult(
            sol::protected_function_result& result,
            std::string& error
        ) {
            if (!result.valid()) {
                sol::error luaError = result;
                SetError(error, luaError.what());
                return false;
            }

            error.clear();
            return true;
        }

        void RegisterLog(sol::state& state) {
            sol::table log = state.create_named_table("Log");

            log.set_function("info", [](const std::string& message) {
                spdlog::info("[Lua] {}", message);
                });

            log.set_function("warning", [](const std::string& message) {
                spdlog::warn("[Lua] {}", message);
                });

            log.set_function("error", [](const std::string& message) {
                spdlog::error("[Lua] {}", message);
                });
        }

    }

    LuaRuntime::LuaRuntime()
        : impl_(std::make_unique<Impl>()) {
    }

    LuaRuntime::~LuaRuntime() {
        Shutdown();
    }

    bool LuaRuntime::Initialize(std::string& error) {
        if (IsInitialized()) {
            error.clear();
            return true;
        }

        try {
            impl_->state = std::make_unique<sol::state>();

            impl_->state->open_libraries(
                sol::lib::base,
                sol::lib::coroutine,
                sol::lib::string,
                sol::lib::table,
                sol::lib::math,
                sol::lib::utf8
            );

            sol::table globals = impl_->state->globals();

            globals["dofile"] = sol::nil;
            globals["loadfile"] = sol::nil;

            RegisterLog(*impl_->state);

            error.clear();

            spdlog::info(
                "Lua Runtime initialized successfully"
            );

            return true;
        }
        catch (const std::exception& exception) {
            Shutdown();
            SetError(error, exception.what());
            return false;
        }
    }

    void LuaRuntime::Shutdown() noexcept {
        impl_->state.reset();

        spdlog::info("Lua Runtime shutdown");
    }

    bool LuaRuntime::IsInitialized() const noexcept {
        return impl_ && static_cast<bool>(impl_->state);
    }

    bool LuaRuntime::ExecuteString(
        std::string_view source,
        std::string_view chunkName,
        std::string& error
    ) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }

        try {
            sol::load_result loaded = impl_->state->load(
                std::string(source),
                std::string(chunkName)
            );

            if (!loaded.valid()) {
                sol::error luaError = loaded;
                SetError(error, luaError.what());
                return false;
            }

            sol::protected_function script = loaded;

            sol::protected_function_result result = script();

            return CheckResult(result, error);
        }
        catch (const std::exception& exception) {
            SetError(error, exception.what());
            return false;
        }
    }

    bool LuaRuntime::ExecuteFile(
        const std::filesystem::path& path,
        std::string& error
    ) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }

        std::ifstream file(path, std::ios::binary);

        if (!file.is_open()) {
            SetError(
                error,
                "Cannot open Lua file: " + path.string()
            );

            return false;
        }

        std::string source{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        };

        if (file.bad()) {
            SetError(
                error,
                "Failed to read Lua file: " + path.string()
            );

            return false;
        }

        return ExecuteString(
            source,
            path.string(),
            error
        );
    }

    bool LuaRuntime::CallFunction(
        std::string_view functionName,
        std::string& error
    ) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }

        sol::object object = impl_->state->get<sol::object>(
            std::string(functionName)
        );

        if (object.get_type() != sol::type::function) {
            SetError(
                error,
                "Lua function not found: " + std::string(functionName)
            );

            return false;
        }

        sol::protected_function function =
            object.as<sol::protected_function>();

        sol::protected_function_result result = function();

        return CheckResult(result, error);
    }

    bool LuaRuntime::CallUpdate(
        double deltaSeconds,
        std::string& error
    ) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }

        sol::object object = impl_->state->get<sol::object>("Update");

        if (object.get_type() != sol::type::function) {
            error.clear();
            return true;
        }

        sol::protected_function update =
            object.as<sol::protected_function>();

        sol::protected_function_result result = update(deltaSeconds);

        return CheckResult(result, error);
    }

}
