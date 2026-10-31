#include <engine/input/InputSubsystem.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/rendering/UglyUtils.hpp"

namespace engine {

std::string_view InputSubsystem::GetName() const {
    return "InputSubsystem";
}

void InputSubsystem::Update(const FrameContext&) {
    current_ = pending_;
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
    std::erase_if(active_, [id](const ActiveContext& c) { return c.id == id; });
    // A modal disappearing must not make already-held keys start moving a camera.
    for (auto& c : active_) {
        c.suppressed |= pending_.physicalDown;
        c.suppressedButtons |= pending_.buttonsDown;
    }
}

const InputSubsystem::ActiveContext* InputSubsystem::Find(std::uint64_t id) const {
    const auto it = std::find_if(active_.begin(), active_.end(),
                                 [id](const ActiveContext& c) { return c.id == id; });
    return it == active_.end() ? nullptr : &*it;
}

bool InputSubsystem::Allowed(const ActiveContext& context, bool mouse) const {
    for (const auto& other : active_) {
        if (other.id != context.id && other.priority > context.priority &&
            (mouse ? other.blockMouse : other.blockKeyboard)) return false;
    }
    return true;
}

bool InputSubsystem::KeyDown(const ActiveContext& context, PhysicalKey key) const {
    if (key == PhysicalKey::Unknown || key == PhysicalKey::Count || !Allowed(context, false)) return false;
    const auto i = Index(key);
    return current_.physicalDown.test(i) && !context.suppressed.test(i);
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
            if (!active->suppressed.test(i) &&
                (state == 0 ? current_.physicalDown.test(i) :
                 state == 1 ? current_.physicalPressed.test(i) : current_.physicalReleased.test(i))) return true;
        } else if (binding.button != MouseButton::Count && Allowed(*active, true)) {
            const auto i = Index(binding.button);
            if (!active->suppressedButtons.test(i) && (state == 0 ? current_.buttonsDown.test(i) :
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
    pending_.mouseDelta = {};
    pending_.wheelDelta = 0.0f;
    pending_.physicalReleased |= pending_.physicalDown;
    pending_.physicalDown.reset();
    for (auto& context : active_) context.suppressed.reset();
    for (auto& context : active_) context.suppressedButtons.reset();
    pending_.buttonsDown.reset();
}

}  // namespace engine
