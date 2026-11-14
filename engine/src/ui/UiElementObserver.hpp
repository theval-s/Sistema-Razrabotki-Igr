#pragma once
#include <RmlUi/Core/Element.h>
#include <optional>

namespace engine::ui_detail {
// RmlUi 6.3's Windows DLL exports ObserverPtr<Element>'s move constructor but not
// its default/null constructors. An empty optional needs neither; populated values
// still use the upstream observer pool and expire when the DOM element is removed.
class ElementObserver {
public:
    ElementObserver() = default;
    ElementObserver(ElementObserver&&) noexcept = default;
    ElementObserver& operator=(ElementObserver&&) noexcept = default;
    ElementObserver& operator=(Rml::ObserverPtr<Rml::Element>&& value) noexcept {
        value_.emplace(std::move(value));
        return *this;
    }
    Rml::Element* get() const noexcept { return value_ ? value_->get() : nullptr; }
    Rml::Element* operator->() const noexcept { return get(); }
    explicit operator bool() const noexcept { return get() != nullptr; }
    void reset() noexcept { value_.reset(); }
private:
    std::optional<Rml::ObserverPtr<Rml::Element>> value_;
};
}
