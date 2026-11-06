#pragma once

#include <engine/input/InputTypes.hpp>

namespace engine {

/// Win32 set-1 scan code (lParam bits 16-23 or QKeyEvent::nativeScanCode on Windows).
/// The extended bit distinguishes e.g. right Control from left Control.
inline PhysicalKey PhysicalKeyFromWin32Scan(unsigned scan, bool extended) {
    if (extended) {
        switch (scan) {
            case 0x1c: return PhysicalKey::NumpadEnter;
            case 0x1d: return PhysicalKey::RightControl;
            case 0x38: return PhysicalKey::RightAlt;
            case 0x4b: return PhysicalKey::Left;
            case 0x4d: return PhysicalKey::Right;
            case 0x48: return PhysicalKey::Up;
            case 0x50: return PhysicalKey::Down;
            case 0x53: return PhysicalKey::Delete;
            case 0x5b: return PhysicalKey::LeftMeta;
            case 0x5c: return PhysicalKey::RightMeta;
            default: return PhysicalKey::Unknown;
        }
    }
    switch (scan) {
        case 0x01: return PhysicalKey::Escape;
        case 0x02: return PhysicalKey::Num1;
        case 0x03: return PhysicalKey::Num2;
        case 0x04: return PhysicalKey::Num3;
        case 0x05: return PhysicalKey::Num4;
        case 0x06: return PhysicalKey::Num5;
        case 0x07: return PhysicalKey::Num6;
        case 0x08: return PhysicalKey::Num7;
        case 0x09: return PhysicalKey::Num8;
        case 0x0a: return PhysicalKey::Num9;
        case 0x0b: return PhysicalKey::Num0;
        case 0x0e: return PhysicalKey::Backspace;
        case 0x0f: return PhysicalKey::Tab;
        case 0x10: return PhysicalKey::Q;
        case 0x11: return PhysicalKey::W;
        case 0x12: return PhysicalKey::E;
        case 0x13: return PhysicalKey::R;
        case 0x14: return PhysicalKey::T;
        case 0x15: return PhysicalKey::Y;
        case 0x16: return PhysicalKey::U;
        case 0x17: return PhysicalKey::I;
        case 0x18: return PhysicalKey::O;
        case 0x19: return PhysicalKey::P;
        case 0x1c: return PhysicalKey::Enter;
        case 0x1d: return PhysicalKey::LeftControl;
        case 0x1e: return PhysicalKey::A;
        case 0x1f: return PhysicalKey::S;
        case 0x20: return PhysicalKey::D;
        case 0x21: return PhysicalKey::F;
        case 0x22: return PhysicalKey::G;
        case 0x23: return PhysicalKey::H;
        case 0x24: return PhysicalKey::J;
        case 0x25: return PhysicalKey::K;
        case 0x26: return PhysicalKey::L;
        case 0x2a: return PhysicalKey::LeftShift;
        case 0x2c: return PhysicalKey::Z;
        case 0x2d: return PhysicalKey::X;
        case 0x2e: return PhysicalKey::C;
        case 0x2f: return PhysicalKey::V;
        case 0x30: return PhysicalKey::B;
        case 0x31: return PhysicalKey::N;
        case 0x32: return PhysicalKey::M;
        case 0x36: return PhysicalKey::RightShift;
        case 0x38: return PhysicalKey::LeftAlt;
        case 0x39: return PhysicalKey::Space;
        case 0x3b: return PhysicalKey::F1;
        case 0x3c: return PhysicalKey::F2;
        case 0x3d: return PhysicalKey::F3;
        case 0x3e: return PhysicalKey::F4;
        case 0x3f: return PhysicalKey::F5;
        case 0x40: return PhysicalKey::F6;
        case 0x41: return PhysicalKey::F7;
        case 0x42: return PhysicalKey::F8;
        case 0x43: return PhysicalKey::F9;
        case 0x44: return PhysicalKey::F10;
        case 0x57: return PhysicalKey::F11;
        case 0x58: return PhysicalKey::F12;
        default: return PhysicalKey::Unknown;
    }
}

}  // namespace engine
