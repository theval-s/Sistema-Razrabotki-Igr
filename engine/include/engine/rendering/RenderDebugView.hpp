#pragma once

#include <cstdint>

namespace engine {

/// Transient inspection state, never serialized with scenes or undo history.
enum class RenderDebugView : std::uint32_t { Final = 0, Albedo = 1, Normals = 2, Depth = 3 };

[[nodiscard]] constexpr bool IsValidRenderDebugView(RenderDebugView view) noexcept {
    switch (view) {
    case RenderDebugView::Final:
    case RenderDebugView::Albedo:
    case RenderDebugView::Normals:
    case RenderDebugView::Depth: return true;
    }
    return false;
}

} // namespace engine
