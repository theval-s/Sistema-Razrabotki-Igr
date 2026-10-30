
#include <engine/scripting/LuaRuntime.hpp>

#include <sol/sol.hpp>

#include <spdlog/spdlog.h>

#include <fstream>
#include <atomic>
#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {

    struct LuaRuntime::Impl {
        struct Instance {
            ScriptInstanceDesc desc;
            sol::environment environment;
            sol::table table;
            std::optional<sol::protected_function> start, update, destroy;
            bool started = false;
            bool faulted = false;
            bool destroying = false;

            std::string Context() const {
                return ToGenericUtf8(desc.source) + " [script=" + desc.assetId.ToString() +
                    ", entity=" + desc.entityId + "]";
            }
            bool Fail(std::string& error, const std::string& message) {
                error = Context() + ": " + message;
                spdlog::error("[Lua] {}", error);
                return false;
            }
            bool Invoke(const std::optional<sol::protected_function>& callback,
                        std::string_view name, std::string& error, std::optional<double> delta = {}) {
                if (!callback) {
                    error.clear();
                    return true;
                }
                try {
                    sol::protected_function_result result = delta
                        ? (*callback)(table, *delta) : (*callback)(table);
                    if (!result.valid()) {
                        sol::error failure = result;
                        faulted = true;
                        return Fail(error, std::string(name) + ": " + failure.what());
                    }
                } catch (const std::exception& exception) {
                    faulted = true;
                    return Fail(error, std::string(name) + ": " + exception.what());
                }
                error.clear();
                return true;
            }
        };
        std::unique_ptr<sol::state> state;
        // Declared after state so all Lua references are released first, including at destruction.
        std::unordered_map<std::uint64_t, std::unique_ptr<Instance>> instances;
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
        if (!impl_->state) return;
        // Destroy callbacks run while the state and the instance's table are still alive.
        while (!impl_->instances.empty()) {
            InstanceId id;
            id.value_ = impl_->instances.begin()->first;
            try {
                std::string error;
                (void)DestroyInstance(id, error);
            } catch (...) {
                // Shutdown must release every Lua reference even if diagnostics fail.
                impl_->instances.erase(id.value_);
            }
        }
        impl_->state.reset();

        spdlog::info("Lua Runtime shutdown");
    }

    bool LuaRuntime::IsInitialized() const noexcept {
        return impl_ && static_cast<bool>(impl_->state);
    }

    bool LuaRuntime::CreateInstance(const ScriptInstanceDesc& desc, InstanceId& instance, std::string& error) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }
        auto record = std::make_unique<Impl::Instance>();
        record->desc = desc;
        if (!desc.assetId.IsValid() || !desc.source.is_absolute()) {
            return record->Fail(error, "expected a script UUID and an absolute resolved source path");
        }
        try {
            std::string source;
            if (!ReadTextFile(desc.source, source, error)) {
                const std::string message = error;
                return record->Fail(error, message);
            }
            // Execute a fresh chunk for every object: table literals (including nested
            // defaults) and closures belong to this instance, never a shared prototype.
            record->environment = sol::environment(*impl_->state, sol::create, impl_->state->globals());
            record->environment["_G"] = record->environment;
            sol::load_result loaded = impl_->state->load(source, "@" + ToGenericUtf8(desc.source));
            if (!loaded.valid()) {
                sol::error failure = loaded;
                return record->Fail(error, failure.what());
            }
            sol::protected_function chunk = loaded;
            sol::set_environment(record->environment, chunk);
            sol::protected_function_result result = chunk();
            if (!result.valid()) {
                sol::error failure = result;
                return record->Fail(error, failure.what());
            }
            if (result.return_count() != 1 || result.get_type() != sol::type::table) {
                return record->Fail(error, "script must return one instance table");
            }
            record->table = result.get<sol::table>();
            for (const auto& [name, callback] : {
                std::pair{"OnStart", &record->start}, std::pair{"OnUpdate", &record->update},
                std::pair{"OnDestroy", &record->destroy}}) {
                sol::object value = record->table.get<sol::object>(name);
                if (value.get_type() == sol::type::nil) continue;
                if (value.get_type() != sol::type::function) {
                    return record->Fail(error, std::string(name) + " must be a function or nil");
                }
                *callback = value.as<sol::protected_function>();
            }
            // Process-wide allocation prevents stale IDs from aliasing another runtime/session.
            static std::atomic<std::uint64_t> next{1};
            InstanceId id;
            id.value_ = next.fetch_add(1, std::memory_order_relaxed);
            if (id.value_ == 0) return record->Fail(error, "instance ID space exhausted");
            auto [entry, inserted] = impl_->instances.try_emplace(id.value_);
            if (!inserted) return record->Fail(error, "instance ID space exhausted");
            entry->second = std::move(record);
            instance = id;
            error.clear();
            return true;
        } catch (const std::exception& exception) {
            return record->Fail(error, exception.what());
        }
    }

    bool LuaRuntime::StartInstance(InstanceId instance, std::string& error) {
        const auto found = impl_->instances.find(instance.value_);
        if (found == impl_->instances.end()) {
            SetError(error, "Invalid or stale script instance");
            return false;
        }
        auto& record = *found->second;
        if (record.destroying || record.faulted) return record.Fail(error, "cannot start a destroying/faulted instance");
        if (record.started) {
            error.clear();
            return true;
        }
        record.started = true;
        return record.Invoke(record.start, "OnStart", error);
    }

    bool LuaRuntime::UpdateInstance(InstanceId instance, double deltaSeconds, std::string& error) {
        const auto found = impl_->instances.find(instance.value_);
        if (found == impl_->instances.end()) {
            SetError(error, "Invalid or stale script instance");
            return false;
        }
        auto& record = *found->second;
        if (!record.started || record.faulted || record.destroying) {
            return record.Fail(error, "cannot update an unstarted/faulted/destroying instance");
        }
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0) {
            return record.Fail(error, "invalid script time step");
        }
        return record.Invoke(record.update, "OnUpdate", error, deltaSeconds);
    }

    bool LuaRuntime::DestroyInstance(InstanceId instance, std::string& error) {
        const auto found = impl_->instances.find(instance.value_);
        if (found == impl_->instances.end()) {
            SetError(error, "Invalid or stale script instance");
            return false;
        }
        auto& record = *found->second;
        if (record.destroying) {
            error.clear();
            return true;
        }
        record.destroying = true;
        const bool clean = !record.started || record.Invoke(record.destroy, "OnDestroy", error);
        impl_->instances.erase(found);
        if (clean) error.clear();
        return clean;
    }

    bool LuaRuntime::UpdateInstances(double deltaSeconds, std::string& error) {
        std::vector<InstanceId> ids;
        for (const auto& [value, record] : impl_->instances) {
            if (!record->started || record->faulted || record->destroying) continue;
            InstanceId id;
            id.value_ = value;
            ids.push_back(id);
        }
        std::string firstError;
        for (const auto id : ids) {
            std::string failure;
            if (!UpdateInstance(id, deltaSeconds, failure) && firstError.empty()) firstError = std::move(failure);
        }
        error = std::move(firstError);
        return error.empty();
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
