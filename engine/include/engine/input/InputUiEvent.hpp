#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <engine/input/InputTypes.hpp>

namespace engine {
struct InputUiModifiers {
    bool shift = false, control = false, alt = false, meta = false;
    bool capsLock = false, numLock = false;
};

// Extra navigation keys missing from legacy Key. Character insertion is always Text/IME.
enum class InputUiNavigationKey { None, Home, End, PageUp, PageDown, Insert };
struct InputUiKey {
    Key logical = Key::Unknown;
    PhysicalKey physical = PhysicalKey::Unknown;
    InputUiNavigationKey navigation = InputUiNavigationKey::None;
    bool down = false;
    bool repeat = false;
};
struct InputUiPointerMove {};
struct InputUiPointerButton { MouseButton button = MouseButton::Left; bool down = false; };
struct InputUiWheel { float horizontal = 0, vertical = 0; }; // notches; vertical positive away
struct InputUiText { std::string utf8; }; // committed non-IME text, never control characters
struct InputUiFocus { bool focused = false; };

enum class InputUiCompositionPhase { Begin, Update, Commit, Cancel };
struct InputUiCompositionClause {
    std::uint32_t start = 0, length = 0;
    bool selected = false;
};
struct InputUiComposition {
    InputUiCompositionPhase phase = InputUiCompositionPhase::Begin;
    std::string utf8; // full preedit on Update, committed result on Commit
    std::uint32_t cursor = 0;
    std::uint32_t selectionStart = 0, selectionLength = 0;
    // Replacement relative to cursor in surrounding text; scalar indices throughout.
    std::int32_t replacementStart = 0;
    std::uint32_t replacementLength = 0;
    std::vector<InputUiCompositionClause> clauses;
};

// InputSubsystem assigns monotonically increasing nonzero sequence when enqueuing.
// Position is captured at event time, in physical surface pixels; false means unknown,
// not (0,0). Focus/key/IME events need not carry a pointer position.
struct InputUiEvent {
    std::uint64_t sequence = 0;
    double realTimeSeconds = 0;
    float pointerX = 0, pointerY = 0;
    bool hasPointerPosition = false;
    InputUiModifiers modifiers;
    std::variant<InputUiPointerMove, InputUiPointerButton, InputUiWheel, InputUiKey,
                 InputUiText, InputUiFocus, InputUiComposition> payload;
};
} // namespace engine
