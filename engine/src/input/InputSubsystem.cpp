#include <engine/input/InputSubsystem.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>
#include <stdexcept>

namespace engine {

std::string_view InputSubsystem::GetName() const {
    return "InputSubsystem";
}

void InputSubsystem::Update(const FrameContext&) {
    current_ = pending_;
    uiEvents_.swap(pendingUiEvents_);
    pendingUiEvents_.clear();
    consumedKeys_.reset();
    consumedPhysical_.reset();
    consumedButtons_.reset();
    consumedMotion_ = consumedWheel_ = false;
    heldKeys_ &= current_.keysDown | current_.keysReleased;
    heldPhysical_ &= current_.physicalDown | current_.physicalReleased;
    heldButtons_ &= current_.buttonsDown | current_.buttonsReleased;
    frameKeyboard_ = captureKeyboard_;
    frameMouse_ = captureMouse_;
    SetUiCapture(captureKeyboard_, captureMouse_);
    for (auto& context : active_)
        context.suppressed &= (current_.physicalDown | current_.physicalReleased);
    for (auto& context : active_)
        context.suppressedButtons &= (current_.buttonsDown | current_.buttonsReleased);
    // Edges and deltas are per frame; what is held stays held.
    pending_.keysPressed.reset();
    pending_.keysReleased.reset();
    pending_.physicalPressed.reset();
    pending_.physicalReleased.reset();
    pending_.buttonsPressed.reset();
    pending_.buttonsReleased.reset();
    pending_.mouseDelta = DirectX::SimpleMath::Vector2();
    pending_.wheelDelta = 0.0f;
}

void InputSubsystem::ContextDefinition::BindAction(std::string name, Binding binding) {
    actions_[std::move(name)].push_back(binding);
}
void InputSubsystem::ContextDefinition::BindAxis1D(std::string name, PhysicalKey key, float value) {
    axes1_[std::move(name)].push_back({key, value});
}
void InputSubsystem::ContextDefinition::BindAxis2D(std::string name, PhysicalKey key,
                                                   DirectX::SimpleMath::Vector2 value) {
    axes2_[std::move(name)].push_back({key, value});
}

InputSubsystem::ContextDefinition& InputSubsystem::DefineContext(std::string name) {
    return definitions_[std::move(name)];
}

InputSubsystem::Context InputSubsystem::Activate(std::string_view name, int priority,
                                                 bool blockKeyboard, bool blockMouse) {
    if (!definitions_.contains(std::string(name))) return {};
    const auto id = nextId_++;
    active_.push_back({id, std::string(name), priority, blockKeyboard, blockMouse,
                       pending_.physicalDown, pending_.buttonsDown});
    return Context(this, id);
}

void InputSubsystem::Deactivate(std::uint64_t id) {
    const auto* removed = Find(id);
    if (!removed) return;
    const int priority = removed->priority;
    const bool keyboard = removed->blockKeyboard;
    const bool mouse = removed->blockMouse;
    std::erase_if(active_, [id](const ActiveContext& c) { return c.id == id; });
    // A modal disappearing must not make already-held keys start moving a camera.
    // Only readers/devices masked by that modal need a fresh press. Removing a
    // nonblocking reader must not disturb input in unrelated host/game contexts.
    for (auto& c : active_) {
        if (c.priority >= priority) continue;
        if (keyboard) c.suppressed |= pending_.physicalDown;
        if (mouse) c.suppressedButtons |= pending_.buttonsDown;
    }
}

const InputSubsystem::ActiveContext* InputSubsystem::Find(std::uint64_t id) const {
    const auto it = std::find_if(active_.begin(), active_.end(),
                                 [id](const ActiveContext& c) { return c.id == id; });
    return it == active_.end() ? nullptr : &*it;
}

bool InputSubsystem::Allowed(const ActiveContext& context, bool mouse) const {
    if (mouse ? MouseMasked() : (captureKeyboard_ || frameKeyboard_)) return false;
    for (const auto& other : active_) {
        if (other.id != context.id && other.priority > context.priority &&
            (mouse ? other.blockMouse : other.blockKeyboard)) return false;
    }
    return true;
}

bool InputSubsystem::KeyDown(const ActiveContext& context, PhysicalKey key) const {
    if (key == PhysicalKey::Unknown || key == PhysicalKey::Count || !Allowed(context, false)) return false;
    const auto i = Index(key);
    return current_.physicalDown.test(i) && !context.suppressed.test(i) && !PhysicalMasked(key);
}

bool InputSubsystem::Action(const Context& context, std::string_view name, int state) const {
    const auto* active = Find(context.id_);
    if (!active) return false;
    const auto def = definitions_.find(active->name);
    if (def == definitions_.end()) return false;
    const auto it = def->second.actions_.find(std::string(name));
    if (it == def->second.actions_.end()) return false;
    for (const auto& binding : it->second) {
        if (binding.key != PhysicalKey::Unknown && binding.key != PhysicalKey::Count && Allowed(*active, false)) {
            const auto i = Index(binding.key);
            if (!active->suppressed.test(i) && !PhysicalMasked(binding.key) &&
                (state == 0 ? current_.physicalDown.test(i) :
                 state == 1 ? current_.physicalPressed.test(i) : current_.physicalReleased.test(i))) return true;
        } else if (binding.button != MouseButton::Count && Allowed(*active, true)) {
            const auto i = Index(binding.button);
            if (!active->suppressedButtons.test(i) && !ButtonMasked(binding.button) && (state == 0 ? current_.buttonsDown.test(i) :
                state == 1 ? current_.buttonsPressed.test(i) : current_.buttonsReleased.test(i))) return true;
        }
    }
    return false;
}

float InputSubsystem::Axis1(const Context& context, std::string_view name) const {
    const auto* active = Find(context.id_);
    if (!active) return 0;
    const auto def = definitions_.find(active->name);
    if (def == definitions_.end()) return 0;
    const auto it = def->second.axes1_.find(std::string(name));
    if (it == def->second.axes1_.end()) return 0;
    float value = 0;
    for (const auto& binding : it->second)
        if (KeyDown(*active, binding.key)) value += binding.value;
    return std::clamp(value, -1.0f, 1.0f);
}

DirectX::SimpleMath::Vector2 InputSubsystem::Axis2(const Context& context, std::string_view name) const {
    using DirectX::SimpleMath::Vector2;
    const auto* active = Find(context.id_);
    if (!active) return {};
    const auto def = definitions_.find(active->name);
    if (def == definitions_.end()) return {};
    const auto it = def->second.axes2_.find(std::string(name));
    if (it == def->second.axes2_.end()) return {};
    Vector2 value{};
    for (const auto& binding : it->second)
        if (KeyDown(*active, binding.key)) value += binding.value;
    if (value.LengthSquared() > 1.0f) value.Normalize();
    return value;
}

InputSubsystem::Context::Context(Context&& other) noexcept
    : input_(std::exchange(other.input_, nullptr)), id_(std::exchange(other.id_, 0)),
      lifetime_(std::move(other.lifetime_)) {}
InputSubsystem::Context& InputSubsystem::Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        Reset();
        input_ = std::exchange(other.input_, nullptr);
        id_ = std::exchange(other.id_, 0);
        lifetime_ = std::move(other.lifetime_);
    }
    return *this;
}
void InputSubsystem::Context::Reset() {
    if (input_ && !lifetime_.expired()) input_->Deactivate(id_);
    input_ = nullptr;
    id_ = 0;
    lifetime_.reset();
}
bool InputSubsystem::Context::Down(std::string_view name) const { return *this && input_->Action(*this, name, 0); }
bool InputSubsystem::Context::Pressed(std::string_view name) const { return *this && input_->Action(*this, name, 1); }
bool InputSubsystem::Context::Released(std::string_view name) const { return *this && input_->Action(*this, name, 2); }
float InputSubsystem::Context::Axis1D(std::string_view name) const { return *this ? input_->Axis1(*this, name) : 0; }
DirectX::SimpleMath::Vector2 InputSubsystem::Context::Axis2D(std::string_view name) const {
    return *this ? input_->Axis2(*this, name) : DirectX::SimpleMath::Vector2{};
}
DirectX::SimpleMath::Vector2 InputSubsystem::Context::MousePosition() const {
    const auto* active = *this ? input_->Find(id_) : nullptr;
    return active && input_->Allowed(*active, true) ? input_->GetMousePosition() : DirectX::SimpleMath::Vector2{};
}
DirectX::SimpleMath::Vector2 InputSubsystem::Context::MouseDelta() const {
    const auto* active = *this ? input_->Find(id_) : nullptr;
    return active && input_->Allowed(*active, true) ? input_->GetMouseDelta() : DirectX::SimpleMath::Vector2{};
}
float InputSubsystem::Context::WheelDelta() const {
    const auto* active = *this ? input_->Find(id_) : nullptr;
    return active && input_->Allowed(*active, true) ? input_->GetWheelDelta() : 0;
}

void InputSubsystem::OnPhysicalKey(PhysicalKey key, bool down) {
    if (key == PhysicalKey::Unknown || key == PhysicalKey::Count) return;
    const auto i = Index(key);
    if (pending_.physicalDown.test(i) == down) return;
    if (down && current_.physicalReleased.test(i))
        for (auto& context : active_) context.suppressed.reset(i);
    if (down && current_.physicalReleased.test(i)) heldPhysical_.reset(i);
    pending_.physicalDown.set(i, down);
    (down ? pending_.physicalPressed : pending_.physicalReleased).set(i);
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
    if (down && current_.keysReleased.test(index)) heldKeys_.reset(index);
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
    if (down && current_.buttonsReleased.test(index))
        for (auto& context : active_) context.suppressedButtons.reset(index);
    if (down && current_.buttonsReleased.test(index)) heldButtons_.reset(index);
    pending_.buttonsDown.set(index, down);
    (down ? pending_.buttonsPressed : pending_.buttonsReleased).set(index);
}

void InputSubsystem::OnMouseMove(const float dx, const float dy) {
    pending_.mouseDelta += DirectX::SimpleMath::Vector2{dx, dy};
}

void InputSubsystem::OnMousePosition(const float x, const float y) {
    if (std::isfinite(x) && std::isfinite(y)) pending_.mousePosition = {x, y};
}

void InputSubsystem::OnMouseWheel(const float notches) {
    pending_.wheelDelta += notches;
}

void InputSubsystem::OnFocusLost() {
    pendingUiEvents_.clear();
    SetUiCapture(false, false);
    heldKeys_.reset();
    heldPhysical_.reset();
    heldButtons_.reset();
    pending_.keysPressed.reset();
    pending_.physicalPressed.reset();
    pending_.buttonsPressed.reset();
    pending_.keysReleased |= pending_.keysDown;
    pending_.buttonsReleased |= pending_.buttonsDown;
    pending_.keysDown.reset();
    pending_.mouseDelta = {};
    pending_.mousePosition = {};
    pending_.wheelDelta = 0.0f;
    pending_.physicalReleased |= pending_.physicalDown;
    pending_.physicalDown.reset();
    for (auto& context : active_) context.suppressed.reset();
    for (auto& context : active_) context.suppressedButtons.reset();
    pending_.buttonsDown.reset();
    InputUiEvent event;
    event.payload = InputUiFocus{false};
    OnUiEvent(std::move(event));
}

bool InputSubsystem::KeyMasked(Key key) const {
    const auto i = Index(key);
    return i >= KeyCount || captureKeyboard_ || frameKeyboard_ || consumedKeys_[i] || heldKeys_[i];
}
bool InputSubsystem::PhysicalMasked(PhysicalKey key) const {
    const auto i = Index(key);
    return i >= PhysicalKeyCount || captureKeyboard_ || frameKeyboard_ || consumedPhysical_[i] || heldPhysical_[i];
}
bool InputSubsystem::ButtonMasked(MouseButton button) const {
    const auto i = Index(button);
    return i >= MouseButtonCount || MouseMasked() || consumedButtons_[i] || heldButtons_[i];
}

void InputSubsystem::OnUiEvent(InputUiEvent event) {
    if (nextUiSequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("UI input sequence exhausted");
    event.sequence = nextUiSequence_++;
    if (const auto* focus = std::get_if<InputUiFocus>(&event.payload)) uiFocused_ = focus->focused;
    pendingUiEvents_.push_back(std::move(event));
}

void InputSubsystem::ResetSimulation() {
    // A level transition clears stale presses/composition, but does not change OS
    // focus. Follow cancellation with the actual focus state so autoload menus keep
    // capturing gameplay without requiring a spurious host focus-loss/gain cycle.
    const bool focused = uiFocused_;
    Reset();
    if (focused) {
        InputUiEvent event;
        event.payload = InputUiFocus{true};
        OnUiEvent(std::move(event));
    }
}

void InputSubsystem::ConsumeUiEvent(std::uint64_t sequence) {
    const auto it = std::find_if(uiEvents_.begin(), uiEvents_.end(),
        [sequence](const auto& event) { return event.sequence == sequence; });
    if (it == uiEvents_.end()) return;
    if (const auto* key = std::get_if<InputUiKey>(&it->payload)) {
        if (key->logical != Key::Unknown && Index(key->logical) < KeyCount) {
            consumedKeys_.set(Index(key->logical));
            heldKeys_.set(Index(key->logical));
        }
        if (key->physical != PhysicalKey::Unknown && Index(key->physical) < PhysicalKeyCount) {
            consumedPhysical_.set(Index(key->physical));
            heldPhysical_.set(Index(key->physical));
        }
    } else if (const auto* button = std::get_if<InputUiPointerButton>(&it->payload)) {
        if (Index(button->button) < MouseButtonCount) {
            consumedButtons_.set(Index(button->button));
            heldButtons_.set(Index(button->button));
        }
    } else if (std::holds_alternative<InputUiWheel>(it->payload)) consumedWheel_ = true;
    else if (std::holds_alternative<InputUiPointerMove>(it->payload)) consumedMotion_ = true;
}

void InputSubsystem::SetUiCapture(bool keyboard, bool mouse) {
    captureKeyboard_ = keyboard;
    captureMouse_ = mouse;
    frameKeyboard_ |= keyboard;
    frameMouse_ |= mouse;
    if (keyboard) {
        consumedKeys_ |= current_.keysPressed | current_.keysReleased | current_.keysDown;
        consumedPhysical_ |= current_.physicalPressed | current_.physicalReleased | current_.physicalDown;
        heldKeys_ |= current_.keysDown;
        heldPhysical_ |= current_.physicalDown;
    }
    if (mouse) {
        consumedButtons_ |= current_.buttonsPressed | current_.buttonsReleased | current_.buttonsDown;
        heldButtons_ |= current_.buttonsDown;
    }
}

void InputSubsystem::Reset() {
    pending_ = {};
    current_ = {};
    pendingUiEvents_.clear();
    uiEvents_.clear();
    consumedKeys_.reset(); heldKeys_.reset();
    consumedPhysical_.reset(); heldPhysical_.reset();
    consumedButtons_.reset(); heldButtons_.reset();
    captureKeyboard_ = captureMouse_ = frameKeyboard_ = frameMouse_ = false;
    consumedMotion_ = consumedWheel_ = false;
    for (auto& context : active_) {
        context.suppressed.reset();
        context.suppressedButtons.reset();
    }
    InputUiEvent event;
    event.payload = InputUiFocus{false};
    OnUiEvent(std::move(event));
}

}  // namespace engine
