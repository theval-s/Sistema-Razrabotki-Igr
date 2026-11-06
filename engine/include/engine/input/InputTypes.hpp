#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

/// Logical keys (layout-dependent). Use PhysicalKey for gameplay bindings.
enum class Key : std::uint8_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space, Enter, Escape, Tab, Backspace, Delete,
    Left, Right, Up, Down,
    Shift, Control, Alt,

    Count
};

/// USB HID keyboard usages. These identify positions, independent of the active layout.
enum class PhysicalKey : std::uint8_t {
    Unknown = 0,
    A = 0x04, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 0x1e, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Enter, Escape, Backspace, Tab, Space,
    F1 = 0x3a, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Delete = 0x4c, Right, Left, Down, Up,
    NumpadEnter = 0x58,
    LeftControl = 0xe0, LeftShift, LeftAlt, LeftMeta,
    RightControl, RightShift, RightAlt, RightMeta,
    Count
};

enum class MouseButton : std::uint8_t {
    Left = 0,
    Right,
    Middle,

    Count
};

inline constexpr std::size_t KeyCount = static_cast<std::size_t>(Key::Count);
inline constexpr std::size_t PhysicalKeyCount = static_cast<std::size_t>(PhysicalKey::Count);
inline constexpr std::size_t MouseButtonCount = static_cast<std::size_t>(MouseButton::Count);

}  // namespace engine
