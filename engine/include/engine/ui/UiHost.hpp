#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <engine/engine_export.hpp>
#include <engine/ui/UiTypes.hpp>

namespace engine {
struct UiCaretRect {
    float x = 0, y = 0, width = 0, height = 0; // physical surface pixels
};

// Snapshot for Qt inputMethodQuery / native IME. Unicode scalar indices, NOT UTF-8 bytes
// or UTF-16 units. Host converts once at its platform boundary. No borrowed DOM storage.
struct UiTextInputState {
    bool active = false;
    UiCaretRect caret;
    std::string surroundingText; // UTF-8
    std::uint32_t selectionStart = 0;
    std::uint32_t selectionLength = 0;
    std::uint32_t cursor = 0;
};

// UI retains a shared_ptr for the configured lifetime. Implementations with a shorter-lived
// native window must use their own weak lifetime check. Calls occur on the engine thread;
// host callbacks must queue input, never reenter UI processing or Lua.
class ENGINE_API UiHost {
public:
    virtual ~UiHost() = default;
    // nullopt leaves explicitly configured metrics in control; zero dimensions mean
    // that a platform surface is unavailable (including while minimized).
    virtual std::optional<UiSurfaceMetrics> GetSurfaceMetrics() const { return std::nullopt; }
    virtual bool ReadClipboard(std::string& utf8, std::string& error) = 0;
    virtual bool WriteClipboard(std::string_view utf8, std::string& error) = 0;
    virtual void SetCursor(std::string_view rmlCursorName) = 0;
    virtual void SetTextInput(const UiTextInputState& state) = 0;
    virtual void CancelComposition() = 0;
    virtual void SetCapture(const UiCaptureState& state) = 0;
};
} // namespace engine
