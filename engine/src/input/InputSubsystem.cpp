#include <engine/input/InputSubsystem.hpp>

#include "engine/rendering/UglyUtils.hpp"

namespace engine {

std::string_view InputSubsystem::GetName() const {
    return "InputSubsystem";
}

void InputSubsystem::Update(const FrameContext&) {
    current_ = pending_;
    // Edges and deltas are per frame; what is held stays held.
    pending_.keysPressed.reset();
    pending_.keysReleased.reset();
    pending_.buttonsPressed.reset();
    pending_.buttonsReleased.reset();
    pending_.mouseDelta = DirectX::SimpleMath::Vector2();
    pending_.wheelDelta = 0.0f;
}

void InputSubsystem::OnKey(const Key key, const bool down) {
    if (key == Key::Unknown || key == Key::Count) {
        return;
    }
    const std::size_t index = Index(key);
    if (pending_.keysDown.test(index) == down) {
        return;  // auto-repeat, or a release we never saw the press of
    }
    pending_.keysDown.set(index, down);
    (down ? pending_.keysPressed : pending_.keysReleased).set(index);
}

void InputSubsystem::OnMouseButton(const MouseButton button, const bool down) {
    if (button == MouseButton::Count) {
        return;
    }
    const std::size_t index = Index(button);
    if (pending_.buttonsDown.test(index) == down) {
        return;
    }
    pending_.buttonsDown.set(index, down);
    (down ? pending_.buttonsPressed : pending_.buttonsReleased).set(index);
}

void InputSubsystem::OnMouseMove(const float dx, const float dy) {
    pending_.mouseDelta += DirectX::SimpleMath::Vector2{dx, dy};
}

void InputSubsystem::OnMouseWheel(const float notches) {
    pending_.wheelDelta += notches;
}

void InputSubsystem::OnFocusLost() {
    pending_.keysReleased |= pending_.keysDown;
    pending_.buttonsReleased |= pending_.buttonsDown;
    pending_.keysDown.reset();
    pending_.buttonsDown.reset();
}

}  // namespace engine
