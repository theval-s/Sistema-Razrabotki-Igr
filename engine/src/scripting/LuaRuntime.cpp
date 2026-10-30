
#include <engine/scripting/LuaRuntime.hpp>

#include <sol/sol.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {

    namespace {

        // Runtime failures are logged here only. Callers propagate the returned text.
        bool SetError(std::string& error, const std::string& message, bool log = true) {
            error = message;
            if (log) spdlog::error("[Lua] {}", error);
            return false;
        }

        template <typename Result>
        bool CheckResult(Result& result, std::string& error, std::string_view context) {
            if (!result.valid()) {
                sol::error luaError = result;
                return SetError(error, std::string(context) + ": " + luaError.what());
            }
            error.clear();
            return true;
        }

        template <typename... Args>
        bool ProtectedCall(const sol::protected_function& callback, std::string& error,
                           std::string_view context, Args&&... args) {
            try {
                sol::protected_function_result result = callback(std::forward<Args>(args)...);
                return CheckResult(result, error, context);
            } catch (const std::exception& exception) {
                return SetError(error, std::string(context) + ": " + exception.what());
            } catch (...) {
                return SetError(error, std::string(context) + ": unknown C++ callback exception");
            }
        }

        std::uint64_t AllocateSession() {
            static std::atomic<std::uint64_t> next{1};
            auto token = next.load(std::memory_order_relaxed);
            while (token != std::numeric_limits<std::uint64_t>::max()) {
                if (next.compare_exchange_weak(token, token + 1, std::memory_order_relaxed)) return token;
            }
            return 0;  // Never wrap and alias an old session.
        }

        bool IsUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
            auto inner = path.begin();
            for (const auto& component : root) {
                if (inner == path.end()) return false;
#ifdef _WIN32
                if (_wcsicmp(inner->c_str(), component.c_str()) != 0) return false;
#else
                if (*inner != component) return false;
#endif
                ++inner;
            }
            return true;
        }

        bool ModulePath(std::string_view name, std::filesystem::path& relative) {
            if (name.empty()) return false;
            std::size_t start = 0;
            bool first = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                const char c = name[i];
                if (c == '.') {
                    if (first) return false;
                    relative /= std::string(name.substr(start, i - start));
                    start = i + 1;
                    first = true;
                    continue;
                }
                const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                if (!letter && (first || c < '0' || c > '9')) return false;
                first = false;
            }
            if (first) return false;
            relative /= std::string(name.substr(start));
            return true;
        }

    }  // namespace

    struct LuaRuntime::Impl {
        struct Instance {
            ScriptInstanceDesc desc;
            sol::environment environment;
            sol::table table;
            std::optional<sol::protected_function> start, update, destroy;
            bool started = false;
            bool faulted = false;
            bool destroying = false;
            std::string failure;

            std::string Context() const {
                return ToUtf8(desc.source) + " [script=" + desc.assetId.ToString() +
                    ", entity=" + desc.entityId + "]";
            }
            bool Fail(std::string& error, const std::string& message) {
                faulted = true;
                failure = Context() + " [callback=CreateInstance]: " + message;
                return SetError(error, failure);
            }
            bool Invoke(const std::optional<sol::protected_function>& callback,
                        std::string_view name, std::string& error, std::optional<double> delta = {}) {
                if (faulted) return SetError(error, failure, false);
                if (!callback) {
                    error.clear();
                    return true;
                }
                const std::string context = Context() + " [callback=" + std::string(name) + "]";
                const bool clean = delta
                    ? ProtectedCall(*callback, error, context, table, *delta)
                    : ProtectedCall(*callback, error, context, table);
                if (!clean) {
                    faulted = true;
                    failure = error;
                }
                return clean;
            }
        };
        std::unique_ptr<sol::state> state;
        // Declared after state so all Lua references are released first, including at destruction.
        std::unordered_map<std::uint64_t, std::unique_ptr<Instance>> instances;
        struct Module {
            sol::object value;
            sol::environment environment;
        };
        std::unordered_map<std::string, Module> modules;
        std::vector<std::string> loadingModules;
        std::filesystem::path moduleRoot;
        std::unordered_set<std::uint64_t> destroyed;
        std::uint64_t session = 0;
        std::uint64_t nextInstance = 1;
        bool shuttingDown = false;

        // No logging here: Lua's wrapper raises the returned error, and the outer
        // protected instance/compatibility call reports it once with its own context.
        bool Require(std::string_view name, sol::object& value, std::string& error) {
            try {
                std::filesystem::path relative;
                if (!ModulePath(name, relative)) {
                    error = "invalid module name: expected dot-separated ASCII identifiers";
                    return false;
                }
                if (moduleRoot.empty()) {
                    error = "require has no configured project content root";
                    return false;
                }
                const std::string key(name);
                if (const auto cached = modules.find(key); cached != modules.end()) {
                    value = cached->second.value;
                    return true;
                }
                if (std::find(loadingModules.begin(), loadingModules.end(), key) != loadingModules.end()) {
                    error = "cyclic require: ";
                    for (const auto& loading : loadingModules) error += loading + " -> ";
                    error += key;
                    return false;
                }

                std::filesystem::path sourcePath;
                auto file = moduleRoot / relative;
                file += ".lua";
                for (const auto& candidate : {file, moduleRoot / relative / "init.lua"}) {
                    const auto canonical = std::filesystem::weakly_canonical(candidate);
                    if (!IsUnderRoot(canonical, moduleRoot)) {
                        error = "require path escapes project content root: " + ToUtf8(candidate);
                        return false;
                    }
                    if (std::filesystem::is_regular_file(canonical)) {
                        sourcePath = canonical;
                        break;
                    }
                }
                if (sourcePath.empty()) {
                    error = "module '" + key + "' not found in project content root " + ToUtf8(moduleRoot);
                    return false;
                }
                std::string source;
                if (!ReadTextFile(sourcePath, source, error)) return false;

                loadingModules.push_back(key);
                struct LoadingScope {
                    std::vector<std::string>& stack;
                    ~LoadingScope() { stack.pop_back(); }
                } loadingScope{loadingModules};

                sol::environment environment(*state, sol::create, state->globals());
                environment["_G"] = environment;
                sol::load_result loaded = state->load(source, "@" + ToUtf8(sourcePath));
                if (!loaded.valid()) {
                    sol::error failure = loaded;
                    error = failure.what();
                    return false;
                }
                sol::protected_function chunk = loaded;
                sol::set_environment(environment, chunk);
                sol::protected_function_result result = chunk();
                if (!result.valid()) {
                    sol::error failure = result;
                    error = failure.what();
                    return false;
                }
                value = result.return_count() == 0 || result.get_type() == sol::type::nil
                    ? sol::make_object(*state, true) : result.get<sol::object>();
                modules.emplace(key, Module{value, std::move(environment)});
                error.clear();
                return true;
            } catch (const std::exception& exception) {
                error = "require '" + std::string(name) + "': " + exception.what();
                return false;
            } catch (...) {
                error = "require: unknown module loading error";
                return false;
            }
        }
    };

    namespace {

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
        return Initialize({}, error);
    }

    bool LuaRuntime::Initialize(const std::filesystem::path& root, std::string& error) {
        std::filesystem::path canonicalRoot;
        try {
            if (!root.empty()) {
                if (!root.is_absolute() || !std::filesystem::is_directory(root)) {
                    return SetError(error, "require root must be an existing absolute project content directory");
                }
                canonicalRoot = std::filesystem::canonical(root);
            }
            if (IsInitialized()) {
                if (!root.empty() && canonicalRoot != impl_->moduleRoot) {
                    return SetError(error, "Cannot change require root during an active Lua session");
                }
                error.clear();
                return true;
            }
        } catch (const std::exception& exception) {
            return SetError(error, exception.what());
        }
        try {
            impl_->moduleRoot = std::move(canonicalRoot);
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

            // Do not open package: no native searchers, loadlib, or mutable search paths.
            const sol::object loader = sol::make_object(*impl_->state,
                [runtime = impl_.get()](sol::object name) -> std::tuple<sol::object, std::string> {
                    sol::object value = sol::make_object(*runtime->state, sol::nil);
                    std::string failure;
                    if (name.get_type() != sol::type::string) {
                        return {std::move(value), "require expects a string module name"};
                    }
                    (void)runtime->Require(name.as<std::string>(), value, failure);
                    return {std::move(value), std::move(failure)};
                });
            // Raise from Lua, after all C++ loader locals have unwound normally.
            sol::load_result wrapper = impl_->state->load(R"(
                return function(loader, raise)
                    return function(name)
                        local value, failure = loader(name)
                        if failure ~= '' then raise(failure, 2) end
                        return value
                    end
                end
            )", "require wrapper");
            if (!wrapper.valid()) { sol::error failure = wrapper; throw failure; }
            sol::protected_function wrapperChunk = wrapper;
            sol::protected_function_result factoryResult = wrapperChunk();
            if (!factoryResult.valid()) { sol::error failure = factoryResult; throw failure; }
            sol::protected_function factory = factoryResult.get<sol::protected_function>();
            sol::protected_function_result requireResult = factory(loader, globals["error"]);
            if (!requireResult.valid()) { sol::error failure = requireResult; throw failure; }
            globals["require"] = requireResult.get<sol::protected_function>();

            impl_->session = AllocateSession();
            if (impl_->session == 0) {
                throw std::runtime_error("Lua session token space exhausted");
            }
            impl_->nextInstance = 1;

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
        if (!impl_->state || impl_->shuttingDown) return;
        impl_->shuttingDown = true;
        // Destroy callbacks run while the state and the instance's table are still alive.
        while (!impl_->instances.empty()) {
            InstanceId id;
            id.value_ = impl_->instances.begin()->first;
            id.session_ = impl_->session;
            try {
                std::string error;
                (void)DestroyInstance(id, error);
            } catch (...) {
                // Shutdown must release every Lua reference even if diagnostics fail.
                impl_->instances.erase(id.value_);
            }
        }
        impl_->instances.clear();  // All callbacks, tables and environments die before state.
        impl_->modules.clear();
        impl_->loadingModules.clear();
        impl_->moduleRoot.clear();
        impl_->destroyed.clear();
        impl_->session = 0;
        impl_->state.reset();
        impl_->shuttingDown = false;

        spdlog::info("Lua Runtime shutdown");
    }

    bool LuaRuntime::IsInitialized() const noexcept {
        return impl_ && static_cast<bool>(impl_->state);
    }

    bool LuaRuntime::CreateInstance(const ScriptInstanceDesc& desc, InstanceId& instance, std::string& error) {
        if (!IsInitialized() || impl_->shuttingDown) {
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
            const std::string context = record->Context() + " [callback=CreateInstance]";
            sol::load_result loaded = impl_->state->load(source, "@" + ToUtf8(desc.source));
            if (!CheckResult(loaded, error, context)) {
                record->faulted = true;
                record->failure = error;
                return false;
            }
            sol::protected_function chunk = loaded;
            sol::set_environment(record->environment, chunk);
            sol::protected_function_result result = chunk();
            if (!CheckResult(result, error, context)) {
                record->faulted = true;
                record->failure = error;
                return false;
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
            InstanceId id;
            if (impl_->nextInstance == std::numeric_limits<std::uint64_t>::max()) {
                return record->Fail(error, "instance ID space exhausted");
            }
            id.value_ = impl_->nextInstance++;
            id.session_ = impl_->session;
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
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown) {
            return SetError(error, "Invalid or stale script session", false);
        }
        const auto found = impl_->instances.find(instance.value_);
        if (found == impl_->instances.end()) {
            SetError(error, "Invalid or stale script instance", false);
            return false;
        }
        auto& record = *found->second;
        if (record.faulted) return SetError(error, record.failure, false);
        if (record.destroying) return SetError(error, "Cannot start a destroying instance", false);
        if (record.started) {
            error.clear();
            return true;
        }
        record.started = true;
        return record.Invoke(record.start, "OnStart", error);
    }

    bool LuaRuntime::UpdateInstance(InstanceId instance, double deltaSeconds, std::string& error) {
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown) {
            return SetError(error, "Invalid or stale script session", false);
        }
        const auto found = impl_->instances.find(instance.value_);
        if (found == impl_->instances.end()) {
            SetError(error, "Invalid or stale script instance", false);
            return false;
        }
        auto& record = *found->second;
        if (record.faulted) return SetError(error, record.failure, false);
        if (!record.started || record.destroying) {
            return SetError(error, "Cannot update an unstarted/destroying instance", false);
        }
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0) {
            return SetError(error, "Invalid script time step", false);
        }
        return record.Invoke(record.update, "OnUpdate", error, deltaSeconds);
    }

    bool LuaRuntime::DestroyInstance(InstanceId instance, std::string& error) {
        if (!IsInitialized() || instance.session_ != impl_->session) {
            return SetError(error, "Invalid or stale script session", false);
        }
        const auto found = impl_->instances.find(instance.value_);
        if (found == impl_->instances.end()) {
            if (impl_->destroyed.contains(instance.value_)) {
                error.clear();
                return true;
            }
            SetError(error, "Invalid or stale script instance", false);
            return false;
        }
        auto& record = *found->second;
        if (record.destroying) {
            error.clear();
            return true;
        }
        // Allocate the tombstone before invoking Lua; a repeated destroy never runs it again.
        impl_->destroyed.insert(instance.value_);
        record.destroying = true;
        const bool clean = !record.started || record.faulted || record.Invoke(record.destroy, "OnDestroy", error);
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
            id.session_ = impl_->session;
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

            if (!CheckResult(loaded, error, chunkName)) return false;

            sol::protected_function script = loaded;

            return ProtectedCall(script, error, chunkName);
        }
        catch (const std::exception& exception) {
            SetError(error, std::string(chunkName) + ": " + exception.what());
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

        try {
            std::string source;
            if (!ReadTextFile(path, source, error)) {
                const std::string message = error;
                return SetError(error, message);
            }
            return ExecuteString(source, "@" + ToUtf8(path), error);
        } catch (const std::exception& exception) {
            return SetError(error, "ExecuteFile " + ToUtf8(path) + ": " + exception.what());
        }
    }

    bool LuaRuntime::CallFunction(
        std::string_view functionName,
        std::string& error
    ) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }

        try {
            sol::object object = impl_->state->get<sol::object>(std::string(functionName));
            if (object.get_type() != sol::type::function) {
                return SetError(error, "Lua function not found: " + std::string(functionName));
            }
            const sol::protected_function function = object.as<sol::protected_function>();
            return ProtectedCall(function, error, functionName);
        } catch (const std::exception& exception) {
            return SetError(error, std::string(functionName) + ": " + exception.what());
        }
    }

    bool LuaRuntime::CallUpdate(
        double deltaSeconds,
        std::string& error
    ) {
        if (!IsInitialized()) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }

        try {
            sol::object object = impl_->state->get<sol::object>("Update");
            if (object.get_type() != sol::type::function) {
                error.clear();
                return true;
            }
            const sol::protected_function update = object.as<sol::protected_function>();
            return ProtectedCall(update, error, "Update", deltaSeconds);
        } catch (const std::exception& exception) {
            return SetError(error, std::string("Update: ") + exception.what());
        }
    }

}
