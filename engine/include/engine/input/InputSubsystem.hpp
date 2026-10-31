#pragma once

#include <bitset>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SimpleMath.h>

#include <engine/core/Subsystem.hpp>
#include <engine/input/InputTypes.hpp>

namespace engine {

/// Keyboard and mouse state, per frame. Win32InputHost feeds it for standalone
/// games; embedded hosts (e.g. the editor viewport) feed On*() from their event loop.
///
/// Update() latches everything fed since the previous frame, so during a frame all
/// subsystems see the same state. Register it before readers, after Win32InputHost
/// if using the standalone host.
/// Gameplay uses named context actions and axes bound to physical keys; OnKey()
/// retains the layout-dependent logical key state for legacy readers.
class ENGINE_API InputSubsystem final : public Subsystem {
public:
    struct Binding {
        PhysicalKey key = PhysicalKey::Unknown;
        MouseButton button = MouseButton::Count;
        static Binding Keyboard(PhysicalKey key) { return {key, MouseButton::Count}; }
        static Binding Mouse(MouseButton button) { return {PhysicalKey::Unknown, button}; }
    };

    class ENGINE_API ContextDefinition {
    public:
        void BindAction(std::string name, Binding binding);
        void BindAxis1D(std::string name, PhysicalKey key, float value);
        void BindAxis2D(std::string name, PhysicalKey key, DirectX::SimpleMath::Vector2 value);
    private:
        friend class InputSubsystem;
        struct Axis1 { PhysicalKey key; float value; };
        struct Axis2 { PhysicalKey key; DirectX::SimpleMath::Vector2 value; };
        std::unordered_map<std::string, std::vector<Binding>> actions_;
        std::unordered_map<std::string, std::vector<Axis1>> axes1_;
        std::unordered_map<std::string, std::vector<Axis2>> axes2_;
    };

    /// Move-only activation lease. Destroy/reset it when the owner loses focus.
    /// Safe to destroy after the InputSubsystem; queries then return neutral input.
    class ENGINE_API Context {
    public:
        Context() = default;
        ~Context() { Reset(); }
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&& other) noexcept;
        Context& operator=(Context&& other) noexcept;
        void Reset();
        [[nodiscard]] explicit operator bool() const { return input_ && !lifetime_.expired(); }
        [[nodiscard]] bool Down(std::string_view action) const;
        [[nodiscard]] bool Pressed(std::string_view action) const;
        [[nodiscard]] bool Released(std::string_view action) const;
        [[nodiscard]] float Axis1D(std::string_view axis) const;
        [[nodiscard]] DirectX::SimpleMath::Vector2 Axis2D(std::string_view axis) const;
        [[nodiscard]] DirectX::SimpleMath::Vector2 MouseDelta() const;
        [[nodiscard]] float WheelDelta() const;
    private:
        friend class InputSubsystem;
        Context(InputSubsystem* input, std::uint64_t id)
            : input_(input), id_(id), lifetime_(input->lifetime_) {}
        InputSubsystem* input_ = nullptr;
        std::uint64_t id_ = 0;
        std::weak_ptr<void> lifetime_;
    };

    /// Define bindings once, before activating. Higher-priority blocking contexts
    /// mask the selected device categories for lower-priority contexts.
    ContextDefinition& DefineContext(std::string name);
    [[nodiscard]] Context Activate(std::string_view name, int priority = 0,
                                   bool blockKeyboard = false, bool blockMouse = false);

    ~InputSubsystem() override { lifetime_.reset(); }

    [[nodiscard]] std::string_view GetName() const override;
    void Update(const FrameContext& frame) override;

    // --- Host side -------------------------------------------------------------
    void OnKey(Key key, bool down);
    void OnPhysicalKey(PhysicalKey key, bool down);
    void OnMouseButton(MouseButton button, bool down);
    /// Relative movement in pixels since the previous call (+x right, +y down).
    void OnMouseMove(float dx, float dy);
    /// In wheel notches, positive away from the user.
    void OnMouseWheel(float notches);
    /// Releases every key and button, e.g. when the window loses focus and would
    /// never deliver the release events.
    void OnFocusLost();

    // --- Readers: state of the current frame ------------------------------------
    [[nodiscard]] bool IsDown(Key key) const { return current_.keysDown.test(Index(key)); }
    /// Went down since the previous frame (also true for a tap that already went up).
    [[nodiscard]] bool WasPressed(Key key) const { return current_.keysPressed.test(Index(key)); }
    [[nodiscard]] bool WasReleased(Key key) const { return current_.keysReleased.test(Index(key)); }

    [[nodiscard]] bool IsDown(MouseButton button) const { return current_.buttonsDown.test(Index(button)); }
    [[nodiscard]] bool WasPressed(MouseButton button) const { return current_.buttonsPressed.test(Index(button)); }
    [[nodiscard]] bool WasReleased(MouseButton button) const { return current_.buttonsReleased.test(Index(button)); }

    /// Pixels moved since the previous frame.
    [[nodiscard]] DirectX::SimpleMath::Vector2 GetMouseDelta() const { return current_.mouseDelta; }
    [[nodiscard]] float GetWheelDelta() const { return current_.wheelDelta; }

private:
    struct State {
        std::bitset<KeyCount> keysDown, keysPressed, keysReleased;
        std::bitset<PhysicalKeyCount> physicalDown, physicalPressed, physicalReleased;
        std::bitset<MouseButtonCount> buttonsDown, buttonsPressed, buttonsReleased;
        DirectX::SimpleMath::Vector2 mouseDelta{};
        float wheelDelta = 0.0f;
    };

    static std::size_t Index(Key key) { return static_cast<std::size_t>(key); }
    static std::size_t Index(PhysicalKey key) { return static_cast<std::size_t>(key); }
    static std::size_t Index(MouseButton button) { return static_cast<std::size_t>(button); }

    /// Being fed by the host.
    State pending_{};
    /// Latched in Update(), what readers see.
    State current_{};
    struct ActiveContext {
        std::uint64_t id;
        std::string name;
        int priority;
        bool blockKeyboard;
        bool blockMouse;
        std::bitset<PhysicalKeyCount> suppressed;
        std::bitset<MouseButtonCount> suppressedButtons;
    };
    std::unordered_map<std::string, ContextDefinition> definitions_;
    std::vector<ActiveContext> active_;
    std::uint64_t nextId_ = 1;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    void Deactivate(std::uint64_t id);
    [[nodiscard]] const ActiveContext* Find(std::uint64_t id) const;
    [[nodiscard]] bool Allowed(const ActiveContext& context, bool mouse) const;
    [[nodiscard]] bool KeyDown(const ActiveContext& context, PhysicalKey key) const;
    [[nodiscard]] bool Action(const Context& context, std::string_view name, int state) const;
    [[nodiscard]] float Axis1(const Context& context, std::string_view name) const;
    [[nodiscard]] DirectX::SimpleMath::Vector2 Axis2(const Context& context, std::string_view name) const;
};

}  // namespace engine
