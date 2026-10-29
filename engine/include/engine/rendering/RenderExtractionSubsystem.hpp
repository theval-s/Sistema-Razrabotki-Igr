#pragma once

#include <string_view>

#include <engine/core/Subsystem.hpp>

namespace engine {

class RenderingSubsystem;

// Extracts the active scene's already-loaded resources into RenderWorld.
// Does not own, create or update any GPU resources. Runs in LateUpdate, so it
// sees controllers and deferred scene loads from the same frame.
class ENGINE_API RenderExtractionSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void LateUpdate(const FrameContext& frame) override;

private:
    RenderingSubsystem* rendering_ = nullptr;
};
} // namespace engine
