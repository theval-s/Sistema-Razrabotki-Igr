
#pragma once

#include <engine/engine_export.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/scripting/ScriptProperties.hpp>
#include <engine/physics/PhysicsEvents.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>
#include <engine/scene/GameDocument.hpp>
#include <sol/forward.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <optional>

#include <SimpleMath.h>
#include <entt/entity/entity.hpp>

namespace engine {

    class Engine;
    class Scene;
    class ScriptSubsystem;

    /// Transient handle: a HandleMap slot and generation, plus the runtime session it belongs to.
    /// Never serialized.
    class InstanceId final {
    public:
        [[nodiscard]] bool IsValid() const noexcept { return session_ != 0; }
        [[nodiscard]] bool operator==(const InstanceId&) const noexcept = default;

    private:
        friend class LuaRuntime;

        std::uint32_t slot_ = 0;
        std::int32_t generation_ = 0;
        std::uint64_t session_ = 0;
    };

    struct ScriptInstanceDesc {
        // Already resolved by the caller from CatalogEntry::source.
        std::filesystem::path source;
        AssetUuid assetId;
        std::string entityId;

        // Empty for entity-owned instances. Autoloads set this and leave entityId empty.
        std::string autoloadName{};

        std::optional<ScriptEntityHandle> owner{};
        ScriptPropertyMap properties;
    };

    class ENGINE_API LuaRuntime final {
    public:
        LuaRuntime();
        ~LuaRuntime();

        LuaRuntime(const LuaRuntime&) = delete;
        LuaRuntime& operator=(const LuaRuntime&) = delete;

        LuaRuntime(LuaRuntime&&) = delete;
        LuaRuntime& operator=(LuaRuntime&&) = delete;

        bool Initialize(std::string& error);
        bool Initialize(const std::filesystem::path& root, Engine& engine, std::string& error);

        /// Pins require to this absolute project content root for the session.
        bool Initialize(const std::filesystem::path& root, std::string& error);

        void Shutdown() noexcept;
        /// Stop UI delivery/registration before entity teardown; existing UI remains usable in OnDestroy.
        void SuspendUiSession() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;

        using LevelLoader = std::function<bool(std::string_view, std::string&)>;

        void SetLevelLoader(LevelLoader loader);
        /// Only call outside runtime callbacks and subsystem iteration/teardown.
        void FlushSceneCommands();
        void TickBehaviorTrees(Scene& scene, double dt);
        void RemoveBehaviorTree(Scene& scene, entt::entity entity) noexcept;
        void ClearBehaviorTrees() noexcept;
        struct BehaviorTiming {
            std::size_t updates = 0;
            double meanMilliseconds = 0;
            double maxMilliseconds = 0;
        };
        [[nodiscard]] BehaviorTiming GetBehaviorTiming() const noexcept;
        void DiscardSceneCommands() noexcept;
        [[nodiscard]] bool HasScript(const LuaEntityHandle& entity) const;
        [[nodiscard]] bool HasScriptMethod(const LuaEntityHandle& entity, const std::string& method) const;
        sol::variadic_results CallScript(const LuaEntityHandle& entity, const std::string& method,
            sol::variadic_args args, sol::this_state state);
        [[nodiscard]] bool HasPersistenceHooks(InstanceId instance) const;
        /// Accepts 1 <= savedVersion <= current SaveVersion. OnLoadGame receives
        /// the original saved version and owns migration. UI-owning hook failures fault and
        /// retire UI; runtimes without UI retain the existing persistence retry policy.
        bool ValidateSaveVersion(InstanceId instance, int savedVersion, std::string& error) const;
        /// Non-mutating preflight against every current instance of this asset.
        /// Unknown assets succeed; prepared-instance validation remains necessary.
        bool ValidateAssetSaveVersion(AssetUuid asset, int version, std::string& error) const;
        bool CaptureSaveState(InstanceId instance, int& version, StateValue& data, std::string& error);
        bool RestoreSaveState(InstanceId instance, int savedVersion, const StateValue& data, std::string& error);
        bool FixedUpdateInstance(InstanceId instance, double deltaSeconds, std::string& error);
        void ClearEntityInstances() noexcept {
            DiscardSceneCommands();
            DestroyEntityInstances();
            DiscardSceneCommands();
        }

        /// Executes the file anew in a per-instance environment.
        bool CreateInstance(
            const ScriptInstanceDesc& desc,
            InstanceId& instance,
            std::string& error
        );

        bool StartInstance(InstanceId instance, std::string& error);

        bool NotifyPhysicsActivity(
            InstanceId instance,
            bool awake,
            std::string& error
        );

        bool NotifyPhysicsContact(
            InstanceId instance,
            const PhysicsContactEvent& event,
            entt::entity other,
            std::string& error
        );

        bool UpdateInstance(
            InstanceId instance,
            double deltaSeconds,
            std::string& error
        );

        bool DestroyInstance(InstanceId instance, std::string& error);

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
        friend class ScriptSubsystem;

        /// Finish entity-owned instances while autoload services and shared modules still exist.
        void DestroyEntityInstances() noexcept;

        bool UpdateInstances(double deltaSeconds, std::string& error);
        bool FixedUpdateInstances(double deltaSeconds, std::string& error);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
