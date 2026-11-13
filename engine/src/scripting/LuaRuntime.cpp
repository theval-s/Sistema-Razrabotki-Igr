
#include <engine/scripting/LuaRuntime.hpp>

#include <sol/sol.hpp>
#include "LuaUiBindings.hpp"
#include "../ui/UiRuntimeAccess.hpp"

#include <engine/core/Log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <engine/scripting/LuaBindings.hpp>
#include <engine/scripting/LuaGameplayBindings.hpp>
#include <engine/scripting/LuaSaveGameBindings.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <engine/scripting/LuaSceneBindings.hpp>
#include <engine/scripting/LuaStateValue.hpp>
#include <engine/engine.hpp>

#include <engine/core/FileSystem.hpp>
#include <engine/core/HandleMap.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/scene/Scene.hpp>

namespace engine {

    Scene* LuaBindingsContext::GetScene() const noexcept {
        return engine ? &engine->GetScene() : nullptr;
    }

    namespace {

        constexpr std::array physicsCallbackNames{
            "OnCollisionEnter", "OnCollisionExit",
            "OnTriggerEnter", "OnTriggerExit",
            "OnCollisionStay", "OnTriggerStay",
            "OnSleep", "OnWake",
        };

        // Runtime failures are logged here only. Callers propagate the returned text.
        bool SetError(std::string& error, const std::string& message, bool log = true) {
            error = message;
            if (log) engine::log::Lua().error("{}", error);
            return false;
        }

        /// lazy build a string
        template <typename Context>
        std::string Describe(const Context& context) {
            if constexpr (std::is_invocable_v<const Context&>) {
                return std::string(context());
            }
            else {
                return std::string(context);
            }
        }

        template <typename Result, typename Context>
        bool CheckResult(Result& result, std::string& error, const Context& context) {
            if (!result.valid()) {
                sol::error luaError = result;
                return SetError(error, Describe(context) + ": " + luaError.what());
            }
            error.clear();
            return true;
        }

        /// extracted helper, NOTE: rethink about exceptions
        template <typename Result>
        void ThrowIfFailed(Result& result) {
            if (!result.valid()) {
                sol::error failure = result;
                throw failure;
            }
        }

        template <typename Context, typename... Args>
        bool ProtectedCall(const sol::protected_function& callback, std::string& error,
            const Context& context, Args&&... args) {
            try {
                sol::protected_function_result result = callback(std::forward<Args>(args)...);
                return CheckResult(result, error, context);
            }
            catch (const std::exception& exception) {
                return SetError(error, Describe(context) + ": " + exception.what());
            }
            catch (...) {
                return SetError(error, Describe(context) + ": unknown C++ callback exception");
            }
        }

        std::uint64_t AllocateSession() {
            static std::atomic<std::uint64_t> next{ 1 };
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
            std::optional<sol::protected_function> start, update, fixedUpdate, destroy, save, load;
            int saveVersion = 1;
            bool active = false;
            std::array<std::optional<sol::protected_function>, physicsCallbackNames.size()> physicsCallbacks;
            bool started = false;
            bool environmentCallbacks = false;
            bool faulted = false;
            bool destroying = false;
            UiOwner uiOwner;
            std::string failure;


            std::string Context() const {
                const std::string owner = desc.autoloadName.empty()
                    ? ", entity=" + desc.entityId : ", autoload=" + desc.autoloadName;
                return ToUtf8(desc.source) + " [script=" + desc.assetId.ToString() +
                    owner + "]";
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
                if (active) return SetError(error, "Reentrant script receiver", false);
                active = true;
                struct ActiveScope { bool& active; ~ActiveScope() { active = false; } } activeScope{active};
                const auto context = [this, name] { return Context() + " [callback=" + std::string(name) + "]"; };
                const bool clean = environmentCallbacks
                    ? (delta ? ProtectedCall(*callback, error, context, *delta)
                        : ProtectedCall(*callback, error, context))
                    : (delta ? ProtectedCall(*callback, error, context, table, *delta)
                        : ProtectedCall(*callback, error, context, table));
                if (!clean) {
                    faulted = true;
                    failure = error;
                }
                return clean;
            }
        };
        std::unique_ptr<sol::state> state;
        sol::protected_function installAutoloadFacade;
        sol::protected_function snapshotAutoloadValues;
        sol::protected_function lookupAutoloadMember;
        struct UiCallbackRecord { UiCallback token; UiOwner owner; sol::protected_function function; };
        std::vector<UiCallbackRecord> uiCallbacks;
        struct UiLink { Impl* runtime = nullptr; };
        struct UiDispatcher final : UiCallbackDispatcher {
            std::weak_ptr<UiLink> link;
            explicit UiDispatcher(std::weak_ptr<UiLink> link) : link(std::move(link)) {}
            UiDispatchResult Dispatch(UiOwner, UiCallback, const UiEvent&, std::string&) override;
            void ReleaseCallback(UiCallback callback) noexcept override {
                if (const auto locked = link.lock(); locked && locked->runtime)
                    std::erase_if(locked->runtime->uiCallbacks,
                        [&](const auto& entry) { return entry.token == callback; });
            }
        };
        std::shared_ptr<UiLink> uiLink;
        std::shared_ptr<UiDispatcher> uiDispatcher;
        UiSubsystem* ui = nullptr;
        std::weak_ptr<void> uiLifetime;
        UiSession uiSession;
        bool uiSuspended = false;
        bool shutdownPending = false;
        Instance* activeUiOwner = nullptr;
        struct UiModelRecord { UiOwner owner; UiModel model; UiValueSchema schema; };
        std::vector<UiModelRecord> uiModels;
        UiSubsystem* Ui() const noexcept { return ui && !uiLifetime.expired() ? ui : nullptr; }
        void RetireUi(Instance& instance) noexcept {
            if (auto* live = Ui(); live && !instance.uiOwner.IsEmpty()) {
                UiRuntimeAccess::BeginOwnerTeardown(*live, instance.uiOwner);
                UiRuntimeAccess::ReleaseOwner(*live, instance.uiOwner);
            }
            std::erase_if(uiCallbacks, [&](const auto& entry) { return entry.owner == instance.uiOwner; });
            std::erase_if(uiModels, [&](const auto& entry) { return entry.owner == instance.uiOwner; });
        }
        void FailPersistenceUi(Instance& instance, const std::string& error) {
            // Preserve the established retry policy in runtimes without UI. UI-owning
            // receivers must retire their documents/modal capture on a hook failure.
            if (!instance.uiOwner.IsEmpty()) {
                instance.faulted = true;
                instance.failure = error;
            }
        }
        struct OwnerScope {
            Impl& runtime;
            Instance* previous;
            Instance* owner;
            int exceptions = std::uncaught_exceptions();
            OwnerScope(Impl& runtime, Instance* owner) : runtime(runtime),
                previous(runtime.activeUiOwner), owner(owner) { runtime.activeUiOwner = owner; }
            ~OwnerScope() {
                // A native binding/allocation can throw before ProtectedCall returns a
                // result. Such an unwind must retire the owner's modal just like a
                // reported Lua error, rather than leave a live, undispatchable UI.
                if (owner && std::uncaught_exceptions() > exceptions) {
                    owner->faulted = true;
                    try { owner->failure = "Native exception escaped a script callback"; }
                    catch (...) {} // Retirement must still happen on allocation failure.
                }
                if (owner && owner->faulted) runtime.RetireUi(*owner);
                runtime.activeUiOwner = previous;
            }
        };
        bool UiOwnerForCall(UiOwner& owner, std::string& error, bool creation = false) {
            if (!Ui() || uiSession.IsEmpty()) { error = "UI is unavailable: register an initialized UiSubsystem"; return false; }
            if (!activeUiOwner || activeUiOwner->uiOwner.IsEmpty() || !activeUiOwner->started || activeUiOwner->faulted) {
                error = "UI requires a live started script owner (create/register in OnStart)"; return false;
            }
            if (creation && (uiSuspended || shuttingDown || activeUiOwner->destroying ||
                (activeUiOwner->desc.owner && !activeUiOwner->desc.owner->IsValid()))) {
                error = "UI registration is forbidden during owner/session teardown"; return false;
            }
            owner = activeUiOwner->uiOwner;
            return true;
        }
        template<class Method, class Token, class... Args>
        std::tuple<bool, std::string> UiMutate(Method method, Token token, Args&&... args) {
            UiOwner owner;
            std::string error;
            const bool ok = UiOwnerForCall(owner, error) &&
                (Ui()->*method)(owner, token, std::forward<Args>(args)..., error);
            return {ok, std::move(error)};
        }
        template<class T>
        std::tuple<sol::object, std::string> UiResult(bool ok, const T& value, std::string error) {
            return {ok ? sol::make_object(*state, value) : sol::make_object(*state, sol::nil), std::move(error)};
        }
        void RegisterUi();
        void RegisterAutoloadDispatch();
        void InstallAutoloadDispatch(InstanceId instance);
        sol::object StateValueInput(const sol::object& value);
        sol::object ScriptMember(const Instance& instance, const std::string& name);
        sol::variadic_results DispatchAutoload(InstanceId instance, const std::string& method,
            sol::protected_function function, sol::variadic_args args, sol::this_state luaState);
        LevelLoader levelLoader;
        // Declared after state so all Lua references are released first, including at destruction.
        sol::table autoloads;
        // Boxed so a record stays put while Lua callbacks add instances and the map grows.
        using Instances = HandleMap<std::unique_ptr<Instance>>;
        Instances instances;
        // UpdateInstances' list of what to update, kept to not reallocate every frame.
        std::vector<Instances::Handle> updating;
        struct Module {
            sol::object value;
            sol::environment environment;
        };
        std::unordered_map<std::string, Module> modules;
        std::vector<std::string> loadingModules;
        std::filesystem::path moduleRoot;
        LuaBindingsContext bindingsContext;
        std::deque<std::function<void()>> deferredSceneCommands;
        std::uint64_t session = 0;
        bool shuttingDown = false;
        unsigned callbackDepth = 0;
        bool flushingSceneCommands = false;
        std::unordered_map<Scene*, std::weak_ptr<void>> callbackScenes;

        struct Scope {
            Impl& runtime;
            std::vector<Scene*> scenes;
            explicit Scope(Impl& runtime, Scene* owner = nullptr) : runtime(runtime) {
                ++runtime.callbackDepth;
                for (const auto& [scene, lifetime] : runtime.callbackScenes)
                    if (!lifetime.expired()) scenes.push_back(scene);
                if (auto* scene = runtime.bindingsContext.GetScene(); scene &&
                    std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) scenes.push_back(scene);
                if (owner && std::find(scenes.begin(), scenes.end(), owner) == scenes.end()) scenes.push_back(owner);
                for (auto* scene : scenes) {
                    runtime.callbackScenes[scene] = scene->GetLifetimeToken();
                    scene->BeginExternalCallback();
                }
            }
            ~Scope() {
                for (auto* scene : scenes) scene->EndExternalCallback();
                --runtime.callbackDepth;
            }
        };

        Instance* FindOwner(const LuaEntityHandle& entity) {
            if (!entity.IsValid() || shuttingDown) return nullptr;
            const auto* attachment = entity.GetScene()->TryGetComponent<ScriptComponent>(entity.Resolve());
            if (!attachment || !attachment->enabled) return nullptr;
            for (auto handle : instances.Handles()) {
                auto* record = instances.Get(handle).get();
                if (record->desc.owner && record->desc.owner->IsValid() &&
                    record->desc.owner->GetScene() == entity.GetScene() &&
                    record->desc.owner->Resolve() == entity.Resolve() &&
                    record->desc.assetId == attachment->script.id &&
                    record->started && !record->faulted && !record->destroying) return record;
            }
            return nullptr;
        }

        [[nodiscard]] InstanceId Id(const Instances::Handle handle) const {
            InstanceId id;
            id.slot_ = handle.id;
            id.generation_ = handle.generation;
            id.session_ = session;
            return id;
        }
        [[nodiscard]] static Instances::Handle Handle(const InstanceId id) {
            return Instances::Handle(id.slot_, id.generation_);
        }
        /// The live instance `id` names, or null once it's destroyed. `id` must be of this session.
        [[nodiscard]] Instance* Find(const InstanceId id) {
            std::unique_ptr<Instance>* record = instances.TryGet(Handle(id));
            return record != nullptr ? record->get() : nullptr;
        }

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
                for (const auto& candidate : { file, moduleRoot / relative / "init.lua" }) {
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
                } loadingScope{ loadingModules };

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
                modules.emplace(key, Module{ value, std::move(environment) });
                error.clear();
                return true;
            }
            catch (const std::exception& exception) {
                error = "require '" + std::string(name) + "': " + exception.what();
                return false;
            }
            catch (...) {
                error = "require: unknown module loading error";
                return false;
            }
        }
    };

    namespace {

        void RegisterLog(sol::state& state) {
            sol::table log = state.create_named_table("Log");

            log.set_function("info", [](const std::string& message) {
                engine::log::Game().info("{}", message);
                });

            log.set_function("warning", [](const std::string& message) {
                engine::log::Game().warn("{}", message);
                });

            log.set_function("error", [](const std::string& message) {
                engine::log::Game().error("{}", message);
                });
        }

    }

    #include "LuaUiBindings.inl"

    LuaRuntime::LuaRuntime()
        : impl_(std::make_unique<Impl>()) {
    }

    LuaRuntime::~LuaRuntime() {
        Shutdown();
    }

    bool LuaRuntime::Initialize(std::string& error) {
        return Initialize({}, error);
    }

    bool LuaRuntime::Initialize(const std::filesystem::path& root, Engine& engine, std::string& error) {
        // An active runtime must not silently switch the Engine it is bound to.
        if (IsInitialized())
            return Initialize(root, error);

        impl_->bindingsContext.engine = &engine;

        if (!Initialize(root, error)) {
            impl_->bindingsContext.engine = nullptr;
            impl_->bindingsContext.enqueueSceneCommand = {};
            impl_->deferredSceneCommands.clear();
            return false;
        }

        return true;
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
        }
        catch (const std::exception& exception) {
            return SetError(error, exception.what());
        }
        try {
            impl_->moduleRoot = std::move(canonicalRoot);
            impl_->uiSuspended = false;
            impl_->shutdownPending = false;
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
            impl_->autoloads = impl_->state->create_named_table("Autoloads");

            globals["dofile"] = sol::nil;
            globals["loadfile"] = sol::nil;

            RegisterLog(*impl_->state);
            using Vector3 = DirectX::SimpleMath::Vector3;
            impl_->state->new_usertype<Vector3>("Vector3", sol::constructors<Vector3(), Vector3(float, float, float)>(),
                "x", &Vector3::x, "y", &Vector3::y, "z", &Vector3::z);
            sol::table scene = impl_->state->create_named_table("Scene");
            scene.set_function("OpenLevel", [runtime = impl_.get()](const std::string& name) {
                std::string error;
                const bool ok = runtime->levelLoader && runtime->levelLoader(name, error);
                if (!runtime->levelLoader) error = "Scene.OpenLevel is unavailable outside a game session";
                return std::make_tuple(ok, error);
                });

            // Register Entity and the rest of the scene-facing Lua API. The Scene
            // table must exist first because RegisterSceneBindings() extends it.
            impl_->bindingsContext.enqueueSceneCommand = [this](std::function<void()> command) {
                if (!command) return;
                if (auto* scene = impl_->bindingsContext.GetScene()) {
                    impl_->callbackScenes[scene] = scene->GetLifetimeToken();
                    scene->EnqueueExternalCommand(std::move(command));
                } else {
                    // Engine-less hosts have no current Scene queue to own this command.
                    impl_->deferredSceneCommands.push_back(std::move(command));
                }
            };
            RegisterLuaBindings(*impl_->state, impl_->bindingsContext);
            RegisterGameplayBindings(*impl_->state, impl_->bindingsContext);
            RegisterSaveGameBindings(*impl_->state, impl_->bindingsContext);
            impl_->uiLink = std::make_shared<Impl::UiLink>();
            impl_->uiLink->runtime = impl_.get();
            impl_->uiDispatcher = std::make_shared<Impl::UiDispatcher>(impl_->uiLink);
            if (auto* host = impl_->bindingsContext.engine) {
                if (auto* ui = host->GetSubsystem<UiSubsystem>(); ui && ui->IsInitialized()) {
                    impl_->ui = ui;
                    impl_->uiLifetime = ui->GetLifetimeToken();
                    if (!UiRuntimeAccess::BeginScriptSession(*ui, impl_->uiDispatcher, impl_->uiSession, error))
                        throw std::runtime_error(error);
                }
            }
            impl_->RegisterAutoloadDispatch();
            impl_->RegisterUi();
            sol::table entity = (*impl_->state)["Entity"];
            entity.set_function("HasScript", [this](const LuaEntityHandle& handle) { return HasScript(handle); });
            entity.set_function("HasScriptMethod", [this](const LuaEntityHandle& handle, const std::string& name) {
                return HasScriptMethod(handle, name);
            });
            entity.set_function("CallScript", [this](const LuaEntityHandle& handle, const std::string& name,
                sol::variadic_args args, sol::this_state state) { return CallScript(handle, name, args, state); });

            // Do not open package: no native searchers, loadlib, or mutable search paths.
            const sol::object loader = sol::make_object(*impl_->state,
                [runtime = impl_.get()](sol::object name) -> std::tuple<sol::object, std::string> {
                    sol::object value = sol::make_object(*runtime->state, sol::nil);
                    std::string failure;
                    if (name.get_type() != sol::type::string) {
                        return { std::move(value), "require expects a string module name" };
                    }
                    (void)runtime->Require(name.as<std::string>(), value, failure);
                    return { std::move(value), std::move(failure) };
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
            ThrowIfFailed(wrapper);
            sol::protected_function wrapperChunk = wrapper;
            sol::protected_function_result factoryResult = wrapperChunk();
            ThrowIfFailed(factoryResult);
            sol::protected_function factory = factoryResult.get<sol::protected_function>();
            sol::protected_function_result requireResult = factory(loader, globals["error"]);
            ThrowIfFailed(requireResult);
            globals["require"] = requireResult.get<sol::protected_function>();

            impl_->session = AllocateSession();
            if (impl_->session == 0) {
                throw std::runtime_error("Lua session token space exhausted");
            }
            error.clear();

            log::Lua().info(
                "Lua Runtime initialized successfully"
            );

            return true;
        }
        catch (const std::exception& exception) {
            Shutdown();
            SetError(error, exception.what());
            return false;
        }
        catch (...) {
            Shutdown();
            return SetError(error, "Lua initialization failed: unknown C++ exception");
        }
    }

    void LuaRuntime::SuspendUiSession() noexcept {
        if (!impl_ || !impl_->state) return;
        impl_->uiSuspended = true;
        if (auto* ui = impl_->Ui(); ui && !impl_->uiSession.IsEmpty())
            UiRuntimeAccess::SuspendSession(*ui, impl_->uiSession);
    }

    void LuaRuntime::Shutdown() noexcept {
        if (!impl_) return;
        if (!impl_->state) {
            impl_->deferredSceneCommands.clear();
            impl_->bindingsContext.engine = nullptr;
            impl_->bindingsContext.enqueueSceneCommand = {};
            return;
        }
        SuspendUiSession();
        if (impl_->callbackDepth) { impl_->shutdownPending = true; return; }
        if (impl_->shuttingDown) return;
        impl_->shuttingDown = true;
        DiscardSceneCommands();
        // Destroy callbacks run while the state and the instance's table are still alive. Newest
        // first; CreateInstance refuses while shutting down, so this sees every instance.
        const auto handles = impl_->instances.Handles();
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
            try {
                std::string error;
                (void)DestroyInstance(impl_->Id(*it), error);
            }
            catch (...) {
                // Shutdown must release every Lua reference even if diagnostics fail.
                if (auto* record = impl_->instances.TryGet(*it); record && *record) impl_->RetireUi(**record);
                impl_->instances.Remove(*it);
            }
        }
        impl_->instances = {};  // All callbacks, tables and environments die before state.
        DiscardSceneCommands();
        if (auto* ui = impl_->Ui(); ui && !impl_->uiSession.IsEmpty())
            UiRuntimeAccess::EndSession(*ui, impl_->uiSession);
        impl_->uiCallbacks.clear();
        impl_->uiModels.clear();
        if (impl_->uiLink) impl_->uiLink->runtime = nullptr;
        impl_->uiDispatcher.reset();
        impl_->uiLink.reset();
        impl_->ui = nullptr;
        impl_->uiLifetime.reset();
        impl_->uiSession = {};
        impl_->activeUiOwner = nullptr;
        impl_->uiSuspended = false;
        impl_->shutdownPending = false;
        impl_->modules.clear();
        impl_->installAutoloadFacade = sol::protected_function{};
        impl_->snapshotAutoloadValues = sol::protected_function{};
        impl_->lookupAutoloadMember = sol::protected_function{};
        impl_->autoloads = sol::table{};
        impl_->loadingModules.clear();
        impl_->moduleRoot.clear();
        impl_->deferredSceneCommands.clear();
        impl_->bindingsContext.engine = nullptr;
        impl_->bindingsContext.enqueueSceneCommand = {};
        impl_->session = 0;
        impl_->state.reset();
        impl_->shuttingDown = false;

        log::Lua().info("Lua Runtime shutdown");
    }

    void LuaRuntime::SetLevelLoader(LevelLoader loader) {
        impl_->levelLoader = std::move(loader);
    }

    void LuaRuntime::FlushSceneCommands() {
        if (impl_->callbackDepth || impl_->flushingSceneCommands || impl_->shuttingDown) return;
        if (impl_->shutdownPending) { Shutdown(); return; }
        impl_->flushingSceneCommands = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{impl_->flushingSceneCommands};
        // Scene-bound commands share Scene's FIFO with native destruction/removal.
        // Only engine-less commands use the fallback runtime queue.
        do {
            while (!impl_->deferredSceneCommands.empty()) {
                auto command = std::move(impl_->deferredSceneCommands.front());
                impl_->deferredSceneCommands.pop_front();
                command();
            }
            const auto scenes = impl_->callbackScenes;
            for (const auto& [scene, lifetime] : scenes)
                if (!lifetime.expired()) scene->FlushExternalCommands();
        } while (!impl_->deferredSceneCommands.empty());
    }

    void LuaRuntime::DiscardSceneCommands() noexcept {
        impl_->deferredSceneCommands.clear();
        for (const auto& [scene, lifetime] : impl_->callbackScenes)
            if (!lifetime.expired()) scene->DiscardExternalCommands();
        impl_->callbackScenes.clear();
    }

    namespace {
        bool ReservedMethod(std::string_view name) {
            if (name.empty() || name == "OnStart" || name == "OnUpdate" || name == "OnFixedUpdate" ||
                name == "OnDestroy" || name == "OnSaveGame" || name == "OnLoadGame" || name == "SaveVersion") return true;
            return std::find(physicsCallbackNames.begin(), physicsCallbackNames.end(), name) != physicsCallbackNames.end();
        }
    }

    bool LuaRuntime::HasScript(const LuaEntityHandle& entity) const {
        return IsInitialized() && impl_->FindOwner(entity) != nullptr;
    }

    bool LuaRuntime::HasScriptMethod(const LuaEntityHandle& entity, const std::string& method) const {
        if (!IsInitialized() || ReservedMethod(method)) return false;
        auto* record = impl_->FindOwner(entity);
        if (!record || record->active) return false;
        Impl::Scope scope(*impl_, entity.GetScene());
        Impl::OwnerScope ownerScope(*impl_, record);
        record->active = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{record->active};
        try {
            const sol::object value = impl_->ScriptMember(*record, method);
            return value.get_type() == sol::type::function;
        } catch (...) { return false; }
    }

    sol::variadic_results LuaRuntime::CallScript(const LuaEntityHandle& entity, const std::string& method,
        sol::variadic_args args, sol::this_state state) {
        sol::state_view lua(state);
        const auto failure = [&](const std::string& message) {
            sol::variadic_results values;
            values.push_back(sol::make_object(lua, false));
            values.push_back(sol::make_object(lua, message));
            return values;
        };
        if (!IsInitialized() || ReservedMethod(method)) return failure("Invalid or reserved script method");
        auto* record = impl_->FindOwner(entity);
        if (!record) return failure("Invalid, unstarted, faulted or destroying script receiver");
        if (record->active) return failure("Reentrant script receiver");
        Impl::Scope scope(*impl_, entity.GetScene());
        Impl::OwnerScope ownerScope(*impl_, record);
        record->active = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{record->active};
        try {
            sol::object object = impl_->ScriptMember(*record, method);
            if (object.get_type() != sol::type::function) return failure("Script method not found: " + method);
            sol::protected_function callback = object.as<sol::protected_function>();
            sol::protected_function_result result = record->environmentCallbacks ? callback(args) : callback(record->table, args);
            // A same-receiver helper may already have faulted/logged this instance
            // through its public trampoline. Propagate that diagnostic unchanged.
            if (!result.valid() && record->faulted) return failure(record->failure);
            std::string error;
            if (!CheckResult(result, error, record->Context() + " [method=" + method + "]")) {
                record->faulted = true;
                record->failure = error;
                return failure(error);
            }
            sol::variadic_results values;
            values.push_back(sol::make_object(lua, true));
            for (int i = 0; i < result.return_count(); ++i) values.push_back(result.get<sol::object>(i));
            return values;
        } catch (const std::exception& exception) {
            record->faulted = true;
            SetError(record->failure, record->Context() + " [method=" + method + "]: " + exception.what());
            return failure(record->failure);
        } catch (...) {
            record->faulted = true;
            SetError(record->failure, record->Context() + " [method=" + method + "]: unknown callback exception");
            return failure(record->failure);
        }
    }

    bool LuaRuntime::HasPersistenceHooks(InstanceId instance) const {
        if (!IsInitialized() || instance.session_ != impl_->session) return false;
        auto* record = impl_->Find(instance);
        return record && record->save && record->load;
    }

    bool LuaRuntime::ValidateSaveVersion(InstanceId instance, int savedVersion, std::string& error) const {
        if (!HasPersistenceHooks(instance)) return SetError(error, "Script persistence hooks are unavailable", false);
        const auto* record = impl_->Find(instance);
        if (record->faulted || record->destroying) return SetError(error, "Script persistence receiver is faulted/destroying", false);
        if (savedVersion < 1 || savedVersion > record->saveVersion)
            return SetError(error, "Saved script version must be between 1 and the current SaveVersion", false);
        error.clear();
        return true;
    }

    bool LuaRuntime::ValidateAssetSaveVersion(AssetUuid asset, int version, std::string& error) const {
        if (IsInitialized()) {
            for (const auto handle : impl_->instances.Handles()) {
                const auto& record = impl_->instances.Get(handle);
                if (record->desc.assetId == asset && !ValidateSaveVersion(impl_->Id(handle), version, error))
                    return false;
            }
        }
        error.clear();
        return true;
    }

    bool LuaRuntime::CaptureSaveState(InstanceId instance, int& version, StateValue& data, std::string& error) {
        if (!HasPersistenceHooks(instance)) return SetError(error, "Script persistence hooks are unavailable", false);
        auto* record = impl_->Find(instance);
        if (record->faulted || record->destroying) return SetError(error, "Script persistence receiver is faulted/destroying", false);
        if (record->active) return SetError(error, "Reentrant script persistence hook", false);
        Impl::Scope scope(*impl_, record->desc.owner ? record->desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, record);
        record->active = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{record->active};
        try {
            sol::protected_function_result result = record->environmentCallbacks ? (*record->save)() : (*record->save)(record->table);
            if (!CheckResult(result, error, record->Context() + " [callback=OnSaveGame]")) {
                impl_->FailPersistenceUi(*record, error);
                return false;
            }
            if (result.return_count() != 1 || result.get_type() != sol::type::table) {
                SetError(error, record->Context() + " [callback=OnSaveGame]: must return exactly one table", false);
                impl_->FailPersistenceUi(*record, error);
                return false;
            }
            StateValue captured;
            if (!LuaToStateValue(impl_->StateValueInput(result.get<sol::object>()), captured, error)) {
                impl_->FailPersistenceUi(*record, error);
                return false;
            }
            version = record->saveVersion;
            data = std::move(captured);
            error.clear();
            return true;
        } catch (const std::exception& exception) {
            SetError(error, record->Context() + " [callback=OnSaveGame]: " + exception.what());
            impl_->FailPersistenceUi(*record, error);
            return false;
        } catch (...) {
            SetError(error, record->Context() + " [callback=OnSaveGame]: unknown exception");
            impl_->FailPersistenceUi(*record, error);
            return false;
        }
    }

    bool LuaRuntime::RestoreSaveState(InstanceId instance, int savedVersion, const StateValue& data, std::string& error) {
        if (!ValidateSaveVersion(instance, savedVersion, error)) return false;
        auto* record = impl_->Find(instance);
        if (record->active) return SetError(error, "Reentrant script persistence hook", false);
        if (!std::holds_alternative<StateValue::Object>(data.value) && !std::holds_alternative<StateValue::Array>(data.value))
            return SetError(error, "Saved script state must be a table", false);
        Impl::Scope scope(*impl_, record->desc.owner ? record->desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, record);
        record->active = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{record->active};
        try {
            sol::object object = StateValueToLua(*impl_->state, data);
            const auto context = record->Context() + " [callback=OnLoadGame]";
            const bool clean = record->environmentCallbacks ? ProtectedCall(*record->load, error, context, object, savedVersion)
                : ProtectedCall(*record->load, error, context, record->table, object, savedVersion);
            if (!clean) impl_->FailPersistenceUi(*record, error);
            return clean;
        } catch (const std::exception& exception) {
            SetError(error, record->Context() + " [callback=OnLoadGame]: " + exception.what());
            impl_->FailPersistenceUi(*record, error);
            return false;
        } catch (...) {
            SetError(error, record->Context() + " [callback=OnLoadGame]: unknown exception");
            impl_->FailPersistenceUi(*record, error);
            return false;
        }
    }

    bool LuaRuntime::FixedUpdateInstance(InstanceId instance, double deltaSeconds, std::string& error) {
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown)
            return SetError(error, "Invalid or stale script session", false);
        auto* record = impl_->Find(instance);
        if (!record || !record->started || record->destroying) return SetError(error, "Invalid fixed-update receiver", false);
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0) return SetError(error, "Invalid script time step", false);
        Impl::Scope scope(*impl_, record->desc.owner ? record->desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, record);
        return record->Invoke(record->fixedUpdate, "OnFixedUpdate", error, deltaSeconds);
    }

    bool LuaRuntime::FixedUpdateInstances(double deltaSeconds, std::string& error) {
        const auto handles = impl_->instances.Handles();
        std::string firstError;
        for (auto handle : handles) {
            auto* record = impl_->Find(impl_->Id(handle));
            if (!record || !record->started || record->faulted || record->destroying) continue;
            std::string failure;
            if (!FixedUpdateInstance(impl_->Id(handle), deltaSeconds, failure) && firstError.empty()) firstError = std::move(failure);
        }
        error = std::move(firstError);
        return error.empty();
    }

    bool LuaRuntime::IsInitialized() const noexcept {
        return impl_ && static_cast<bool>(impl_->state);
    }

    bool LuaRuntime::CreateInstance(const ScriptInstanceDesc& desc, InstanceId& instance, std::string& error) {
        if (!IsInitialized() || impl_->shuttingDown) {
            SetError(error, "Lua Runtime is not initialized");
            return false;
        }
        Impl::Scope scope(*impl_, desc.owner ? desc.owner->GetScene() : nullptr);
        Impl::OwnerScope noOwner(*impl_, nullptr); // Instance chunks cannot inherit a caller's registration capability.
        auto record = std::make_unique<Impl::Instance>();
        struct PendingUiOwner {
            Impl& runtime; UiOwner owner; bool published = false;
            ~PendingUiOwner() {
                if (!published) if (auto* ui = runtime.Ui(); ui && !owner.IsEmpty())
                    UiRuntimeAccess::ReleaseOwner(*ui, owner);
            }
        } pendingOwner{*impl_};
        record->desc = desc;
        if (!desc.assetId.IsValid() || !desc.source.is_absolute()) {
            return record->Fail(error, "expected a script UUID and an absolute resolved source path");
        }
        try {
            if (!desc.autoloadName.empty() &&
                impl_->autoloads.raw_get<sol::object>(desc.autoloadName).get_type() != sol::type::nil)
                return record->Fail(error, "duplicate autoload name: " + desc.autoloadName);
            std::string source;
            if (!ReadTextFile(desc.source, source, error)) {
                const std::string message = error;
                return record->Fail(error, message);
            }
            // Execute a fresh chunk for every object: table literals (including nested
            ScriptPropertyMap defaults;
            if (!ValidateScriptProperties(desc.properties, error)) return record->Fail(error, error);
            if (!ReadScriptProperties(source, defaults, error)) return record->Fail(error, error);
            // defaults) and closures belong to this instance, never a shared prototype.
            record->environment = sol::environment(*impl_->state, sol::create, impl_->state->globals());
            record->environment["_G"] = record->environment;
            if (desc.owner) record->environment["entity"] = *desc.owner;
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
            if (result.return_count() == 0) {
                record->environmentCallbacks = true;
                record->table = record->environment;
            }
            else if (result.return_count() == 1 && result.get_type() == sol::type::table) {
                record->table = result.get<sol::table>();
            }
            else {
                return record->Fail(error, "script must return nothing (top-level format) or one instance table");
            }
            if (!defaults.empty() || !desc.properties.empty()) {
                sol::table properties = impl_->state->create_table();
                for (const auto& [name, value] : defaults) {
                    const auto override = desc.properties.find(name);
                    const auto& chosen = override != desc.properties.end() && CompatibleScriptProperty(value, override->second)
                        ? override->second : value;
                    if (const auto* text = std::get_if<std::string>(&chosen.value)) properties[name] = *text;
                    else if (const auto* boolean = std::get_if<bool>(&chosen.value)) properties[name] = *boolean;
                    else properties[name] = chosen.Number();
                }
                for (const auto& [name, value] : desc.properties) {
                    const auto definition = defaults.find(name);
                    if (definition == defaults.end() || !CompatibleScriptProperty(definition->second, value))
                        log::Lua().warn("{}: unused or incompatible property '{}'", record->Context(), name);
                }
                record->table["Properties"] = properties;
            }
            for (const auto& [name, callback] : {
                std::pair{ "OnStart", &record->start }, std::pair{ "OnUpdate", &record->update },
                std::pair{ "OnFixedUpdate", &record->fixedUpdate },
                std::pair{ "OnSaveGame", &record->save }, std::pair{ "OnLoadGame", &record->load },
                std::pair{ "OnDestroy", &record->destroy } }) {
                sol::object value = record->environmentCallbacks
                    ? record->table.raw_get<sol::object>(name) : record->table.get<sol::object>(name);
                if (value.get_type() == sol::type::nil) continue;
                if (value.get_type() != sol::type::function) {
                    return record->Fail(error, std::string(name) + " must be a function or nil");
                }
                *callback = value.as<sol::protected_function>();
            }
            for (std::size_t i = 0; i < physicsCallbackNames.size(); ++i) {
                const auto* name = physicsCallbackNames[i];
                sol::object value = record->environmentCallbacks
                    ? record->table.raw_get<sol::object>(name) : record->table.get<sol::object>(name);
                if (value.get_type() == sol::type::nil) continue;
                if (value.get_type() != sol::type::function)
                    return record->Fail(error, std::string(name) + " must be a function or nil");
                record->physicsCallbacks[i] = value.as<sol::protected_function>();
            }
            if (record->save.has_value() != record->load.has_value())
                return record->Fail(error, "OnSaveGame and OnLoadGame must be provided together");
            sol::object version = record->environmentCallbacks
                ? record->table.raw_get<sol::object>("SaveVersion") : record->table.get<sol::object>("SaveVersion");
            if (version.get_type() != sol::type::nil) {
                if (version.get_type() != sol::type::number) return record->Fail(error, "SaveVersion must be a positive integer");
                const double number = version.as<double>();
                if (!std::isfinite(number) || number < 1 || number > std::numeric_limits<int>::max() || std::floor(number) != number)
                    return record->Fail(error, "SaveVersion must be a positive integer");
                record->saveVersion = static_cast<int>(number);
            }
            if (auto* ui = impl_->Ui(); ui && !impl_->uiSession.IsEmpty()) {
                if (!UiRuntimeAccess::CreateScriptOwner(*ui, impl_->uiSession, record->Context(), record->uiOwner, error))
                    return record->Fail(error, error);
                pendingOwner.owner = record->uiOwner;
            }
            // Publish the actual instance before OnStart, retaining its private locals/closures.
            if (!desc.autoloadName.empty()) impl_->autoloads.raw_set(desc.autoloadName, record->table);
            InstanceId registered;
            try {
                registered = impl_->Id(impl_->instances.Add(std::move(record)));
                if (!desc.autoloadName.empty()) impl_->InstallAutoloadDispatch(registered);
                instance = registered;
                pendingOwner.published = true;
            }
            catch (...) {
                if (!desc.autoloadName.empty()) impl_->autoloads.raw_set(desc.autoloadName, sol::nil);
                if (registered.IsValid()) impl_->instances.Remove(Impl::Handle(registered));
                throw;
            }
            error.clear();
            return true;
        }
        catch (const std::exception& exception) {
            return record ? record->Fail(error, exception.what()) : SetError(error, exception.what());
        }
    }

    bool LuaRuntime::StartInstance(InstanceId instance, std::string& error) {
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown) {
            return SetError(error, "Invalid or stale script session", false);
        }
        Impl::Instance* found = impl_->Find(instance);
        if (found == nullptr) {
            return SetError(error, "Invalid or stale script instance", false);
        }
        auto& record = *found;
        Impl::Scope scope(*impl_, record.desc.owner ? record.desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, &record);
        if (record.faulted) return SetError(error, record.failure, false);
        if (record.destroying) return SetError(error, "Cannot start a destroying instance", false);
        if (record.started) {
            error.clear();
            return true;
        }
        record.started = true;
        if (auto* ui = impl_->Ui(); ui && !record.uiOwner.IsEmpty())
            UiRuntimeAccess::EnableScriptOwner(*ui, record.uiOwner);
        return record.Invoke(record.start, "OnStart", error);
    }

    bool LuaRuntime::NotifyPhysicsActivity(InstanceId instance, bool awake, std::string& error) {
        error.clear();
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown) return true;
        auto* record = impl_->Find(instance);
        Impl::Scope scope(*impl_, record && record->desc.owner ? record->desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, record);
        if (!record || !record->started || record->faulted || record->destroying ||
            !record->desc.owner || !record->desc.owner->IsValid()) return true;
        constexpr auto sleep = static_cast<std::size_t>(PhysicsContactKind::TriggerStay) + 1;
        const auto index = sleep + static_cast<std::size_t>(awake);
        return record->Invoke(record->physicsCallbacks[index], physicsCallbackNames[index], error);
    }

    bool LuaRuntime::NotifyPhysicsContact(InstanceId instance, const PhysicsContactEvent& event,
        entt::entity other, std::string& error) {
        error.clear();
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown) return true;
        auto* record = impl_->Find(instance);
        Impl::Scope scope(*impl_, record && record->desc.owner ? record->desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, record);
        if (!record || !record->started || record->faulted || record->destroying || !record->desc.owner) return true;
        const auto index = static_cast<std::size_t>(event.kind);
        if (index > static_cast<std::size_t>(PhysicsContactKind::TriggerStay) || !record->physicsCallbacks[index]) return true;
        const auto& owner = *record->desc.owner;
        Scene* ownerScene = owner.GetScene();
        if (!ownerScene) return true;
        const entt::entity ownerEntity = owner.Resolve();

        sol::object otherEntity = sol::make_object(*impl_->state, sol::nil);
        if (ownerScene->IsValid(other)) {
            otherEntity = sol::make_object(
                *impl_->state,
                LuaEntityHandle::FromScene(*ownerScene, other)
            );
        }

        sol::table contact = impl_->state->create_table();
        contact["point"] = event.point;
        contact["normal"] = ownerEntity == event.first ? -event.normal : event.normal;
        contact["stepIndex"] = event.stepIndex;
        contact["otherId"] = entt::to_integral(other);
        const auto context = [&] { return record->Context() + " [callback=" + physicsCallbackNames[index] + "]"; };
        const auto& callback = *record->physicsCallbacks[index];
        if (record->active) return SetError(error, "Reentrant script receiver", false);
        record->active = true;
        struct ActiveScope { bool& active; ~ActiveScope() { active = false; } } activeScope{record->active};
        const bool clean = record->environmentCallbacks
            ? ProtectedCall(callback, error, context, otherEntity, contact)
            : ProtectedCall(callback, error, context, record->table, otherEntity, contact);
        if (!clean) {
            record->faulted = true;
            record->failure = error;
        }
        return clean;
    }

    bool LuaRuntime::UpdateInstance(InstanceId instance, double deltaSeconds, std::string& error) {
        Impl::Scope scope(*impl_);
        if (!IsInitialized() || instance.session_ != impl_->session || impl_->shuttingDown) {
            return SetError(error, "Invalid or stale script session", false);
        }
        Impl::Instance* found = impl_->Find(instance);
        if (found == nullptr) {
            return SetError(error, "Invalid or stale script instance", false);
        }
        auto& record = *found;
        Impl::Scope sceneScope(*impl_, record.desc.owner ? record.desc.owner->GetScene() : nullptr);
        Impl::OwnerScope ownerScope(*impl_, &record);
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
        // Only CreateInstance hands out IDs of this session, so one that's gone was destroyed:
        // destroying it again succeeds.
        Impl::Instance* found = impl_->Find(instance);
        if (found == nullptr || found->destroying) {
            error.clear();
            return true;
        }
        auto& record = *found;
        if (record.active) return SetError(error, "Cannot destroy an active script instance", false);
        Impl::Scope scope(*impl_, record.desc.owner ? record.desc.owner->GetScene() : nullptr);
        record.destroying = true;
        struct ReleaseUi {
            Impl& runtime; UiOwner owner;
            ~ReleaseUi() {
                if (auto* ui = runtime.Ui(); ui && !owner.IsEmpty()) UiRuntimeAccess::ReleaseOwner(*ui, owner);
                std::erase_if(runtime.uiCallbacks, [&](const auto& entry) { return entry.owner == owner; });
                std::erase_if(runtime.uiModels, [&](const auto& entry) { return entry.owner == owner; });
            }
        } releaseUi{*impl_, record.uiOwner};
        if (auto* ui = impl_->Ui(); ui && !record.uiOwner.IsEmpty())
            UiRuntimeAccess::BeginOwnerTeardown(*ui, record.uiOwner);
        bool clean;
        {
            Impl::OwnerScope ownerScope(*impl_, &record);
            clean = !record.started || record.faulted || record.Invoke(record.destroy, "OnDestroy", error);
        }
        if (!record.desc.autoloadName.empty()) impl_->autoloads.raw_set(record.desc.autoloadName, sol::nil);
        // By handle: OnDestroy may have added instances and moved the records' boxes around.
        impl_->instances.Remove(Impl::Handle(instance));
        if (clean) error.clear();
        return clean;
    }

    void LuaRuntime::DestroyEntityInstances() noexcept {
        // OnDestroy may destroy or create other instances: go round until a pass finds none left.
        // No ordering between entities is promised; all of them precede the autoload services.
        for (bool destroyed = true; destroyed && impl_->state;) {
            destroyed = false;
            for (const auto handle : impl_->instances.Handles()) {
                const std::unique_ptr<Impl::Instance>* record = impl_->instances.TryGet(handle);
                // EndPlay can be requested by an embedding callback. Shutdown already
                // defers state destruction while callbackDepth is nonzero; leave active
                // receivers for that shutdown instead of retrying them forever here.
                if (record == nullptr || !(*record)->desc.autoloadName.empty() || (*record)->destroying || (*record)->active) continue;
                destroyed = true;
                try {
                    std::string error;
                    (void)DestroyInstance(impl_->Id(handle), error);
                }
                catch (...) {
                    // Keep cleanup progressing even if diagnostic allocation fails.
                    if (auto* remaining = impl_->instances.TryGet(handle); remaining && *remaining) impl_->RetireUi(**remaining);
                    impl_->instances.Remove(handle);
                }
            }
        }
    }

    bool LuaRuntime::UpdateInstances(double deltaSeconds, std::string& error) {
        // In creation order, except that a destroyed instance's slot is reused: autoloads, created
        // first and never destroyed mid-session, always update in list order.
        // A snapshot: callbacks may create or destroy instances. Instances created meanwhile wait
        // for the next frame; UpdateInstance refuses ones destroyed meanwhile. Not reentrant: only
        // ScriptSubsystem::Update calls this, never Lua.
        auto& updating = impl_->updating;
        impl_->instances.Handles(updating);
        std::erase_if(updating, [this](const Impl::Instances::Handle handle) {
            const auto& record = impl_->instances.Get(handle);
            return !record->started || record->faulted || record->destroying;
            });
        std::string firstError;
        for (const auto handle : updating) {
            std::string failure;
            if (!UpdateInstance(impl_->Id(handle), deltaSeconds, failure) && firstError.empty()) {
                firstError = std::move(failure);
            }
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

        Impl::Scope scope(*impl_);
        Impl::OwnerScope noOwner(*impl_, nullptr);
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
        }
        catch (const std::exception& exception) {
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

        Impl::Scope scope(*impl_);
        Impl::OwnerScope noOwner(*impl_, nullptr);
        try {
            sol::object object = impl_->state->get<sol::object>(std::string(functionName));
            if (object.get_type() != sol::type::function) {
                return SetError(error, "Lua function not found: " + std::string(functionName));
            }
            const sol::protected_function function = object.as<sol::protected_function>();
            return ProtectedCall(function, error, functionName);
        }
        catch (const std::exception& exception) {
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

        Impl::Scope scope(*impl_);
        Impl::OwnerScope noOwner(*impl_, nullptr);
        try {
            sol::object object = impl_->state->get<sol::object>("Update");
            if (object.get_type() != sol::type::function) {
                error.clear();
                return true;
            }
            const sol::protected_function update = object.as<sol::protected_function>();
            return ProtectedCall(update, error, "Update", deltaSeconds);
        }
        catch (const std::exception& exception) {
            return SetError(error, std::string("Update: ") + exception.what());
        }
    }

}
