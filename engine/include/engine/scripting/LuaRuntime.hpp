
#pragma once

#include <engine/engine_export.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/scripting/ScriptProperties.hpp>
#include <engine/physics/PhysicsEvents.hpp>
#include <engine/scripting/LuaEntityHandle.hpp>

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

        [[nodiscard]] bool IsInitialized() const noexcept;

        using LevelLoader = std::function<bool(std::string_view, std::string&)>;

        void SetLevelLoader(LevelLoader loader);
        void ClearEntityInstances() noexcept { DestroyEntityInstances(); }

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

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}