
#pragma once

#include <engine/engine_export.hpp>
#include <engine/assets/AssetCatalog.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <string>
#include <string_view>

namespace engine {

    /// Transient handle: a local slot plus a unique runtime-session token. Never serialized.
    class InstanceId final {
    public:
        [[nodiscard]] bool IsValid() const noexcept { return value_ != 0 && session_ != 0; }
        [[nodiscard]] bool operator==(const InstanceId&) const noexcept = default;
    private:
        friend class LuaRuntime;
        std::uint64_t value_ = 0;
        std::uint64_t session_ = 0;
    };

    struct ScriptInstanceDesc {
        // Already resolved by the caller from CatalogEntry::source, never AssetReference::path.
        std::filesystem::path source;
        AssetUuid assetId;
        std::string entityId;
        // Empty for entity-owned instances. Autoloads set this and leave entityId empty.
        std::string autoloadName{};
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
        /// Pins require to this absolute project content root for the session.
        /// The compatibility overload above leaves filesystem require unconfigured.
        bool Initialize(const std::filesystem::path& root, std::string& error);

        void Shutdown() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;
        using LevelLoader = std::function<bool(std::string_view, std::string&)>;
        void SetLevelLoader(LevelLoader loader);
        void ClearEntityInstances() noexcept { DestroyEntityInstances(); }

        /// Executes the file anew in a per-instance environment; it must return a table.
        /// Table literals, including nested defaults, are created separately for every instance.
        /// Optional callbacks: OnStart(self), OnUpdate(self, deltaSeconds), OnDestroy(self).
        /// Output is unchanged on failure. Does not start the instance.
        bool CreateInstance(const ScriptInstanceDesc& desc, InstanceId& instance, std::string& error);
        /// Starts once; a repeated successful start does not invoke OnStart again.
        bool StartInstance(InstanceId instance, std::string& error);
        bool UpdateInstance(InstanceId instance, double deltaSeconds, std::string& error);
        /// Calls OnDestroy only for successfully started, non-faulted instances, then releases references.
        /// Repeated destruction in the same session succeeds. Invalid/stale session IDs fail.
        /// Cleanup still completes when OnDestroy fails; faulted instances skip all Lua callbacks.
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
