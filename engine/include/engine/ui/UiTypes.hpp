#pragma once

#include <cstdint>
#include <string>
#include <engine/scene/StateValue.hpp>

namespace engine {
class UiSubsystem;
class UiRuntimeAccess;

// Value tokens, never owning pointers or serialized IDs. Nonempty is NOT a liveness check.
// session_ is a process-unique UI session (native and Lua sessions share its allocator).
template<class Tag>
class UiHandle final {
public:
    [[nodiscard]] bool IsEmpty() const noexcept { return session_ == 0; }
    [[nodiscard]] bool operator==(const UiHandle&) const noexcept = default;
private:
    friend class UiSubsystem;
    friend class UiRuntimeAccess;
    std::uint64_t session_ = 0;
    std::uint32_t slot_ = 0;
    std::uint32_t generation_ = 0;
};

using UiSession = UiHandle<struct UiSessionTag>;
using UiOwner = UiHandle<struct UiOwnerTag>;
using UiDocument = UiHandle<struct UiDocumentTag>;
using UiElement = UiHandle<struct UiElementTag>;
using UiModel = UiHandle<struct UiModelTag>;
using UiSubscription = UiHandle<struct UiSubscriptionTag>;
using UiCallback = UiHandle<struct UiCallbackTag>;

enum class UiDocumentMode { Hud, Interactive, Modal };
enum class UiRendererBackend { Dx11, Null };

struct UiSurfaceMetrics {
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    float dpRatio = 1.0f;
};

struct UiCaptureState {
    bool keyboard = false;
    bool pointer = false;
    bool textInput = false;
    bool wantsVisibleCursor = false;
};

// Entirely copied at listener time. parameters contains only supported StateValue trees,
// never Rml pointers/Variants or Lua objects. targetId can be empty for anonymous elements.
struct UiEvent {
    UiDocument document;
    std::string sourcePath;
    std::string targetId;
    std::string currentTargetId;
    std::string type;
    StateValue::Object parameters;
    std::uint64_t inputSequence = 0; // zero for events unrelated to a host input event
};
} // namespace engine
