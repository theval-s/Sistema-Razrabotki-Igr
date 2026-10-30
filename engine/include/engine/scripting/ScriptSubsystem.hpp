#pragma once

#include <string_view>

#include <engine/core/Subsystem.hpp>
#include <engine/scripting/LuaRuntime.hpp>

namespace engine {

/// Owns one scripting session. Lua state remains private to LuaRuntime.
class ENGINE_API ScriptSubsystem final : public Subsystem {
public:
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Update(const FrameContext& frame) override;

    /// root is the project content directory (AssetPathResolver::GetGameRoot()).
    /// Pins require to that root; instance asset resolution is handled by the caller.
    [[nodiscard]] bool BeginPlay(const std::filesystem::path& root, std::string& error);
    void EndPlay() noexcept;
    [[nodiscard]] bool IsPlaying() const noexcept { return playing_; }
    [[nodiscard]] const std::filesystem::path& GetRoot() const noexcept { return root_; }

    /// Execution errors use LuaRuntime's bool + error-string interface.
    [[nodiscard]] LuaRuntime& GetRuntime() noexcept { return runtime_; }
    [[nodiscard]] const LuaRuntime& GetRuntime() const noexcept { return runtime_; }

private:
    LuaRuntime runtime_;
    std::filesystem::path root_;
    bool ready_ = false;
    bool playing_ = false;
};

}  // namespace engine
