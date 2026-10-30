
#pragma once

#include <engine/engine_export.hpp>
#include <engine/assets/AssetCatalog.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace engine {

    /// Transient handle: never serialized, and never reused across runtimes or Play sessions.
    class InstanceId final {
    public:
        [[nodiscard]] bool IsValid() const noexcept { return value_ != 0; }
        [[nodiscard]] bool operator==(const InstanceId&) const noexcept = default;
    private:
        friend class LuaRuntime;
        std::uint64_t value_ = 0;
    };

    struct ScriptInstanceDesc {
        // Already resolved by the caller from CatalogEntry::source, never AssetReference::path.
        std::filesystem::path source;
        AssetUuid assetId;
        std::string entityId;
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

        void Shutdown() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;

        /// Executes the file in a per-instance environment; it must return a table.
        /// Optional callbacks: Start(self), Update(self, dt), Destroy(self).
        /// Output is unchanged on failure. Does not start the instance.
        bool CreateInstance(const ScriptInstanceDesc& desc, InstanceId& instance, std::string& error);
        /// Starts once; a repeated start succeeds without invoking Start again.
        bool StartInstance(InstanceId instance, std::string& error);
        bool UpdateInstance(InstanceId instance, double deltaSeconds, std::string& error);
        /// Calls Destroy for started instances (including faulted ones), then releases the record.
        /// Invalid/stale IDs fail; cleanup still completes when the callback fails.
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
        bool UpdateInstances(double deltaSeconds, std::string& error);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
