#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <entt/entity/entity.hpp>

namespace engine {
class Scene;

// Only authored data belongs in the scene file. Runtime fields in a Controller
// instance are discarded when its scene is unloaded.
struct ControllerDefinition {
    std::string type;
    int version = 1;
    bool enabled = true;
    std::string data = "{}"; // JSON value, kept even if the type is unavailable
};

class Controller {
public:
    virtual ~Controller() = default;
    virtual void OnCreate() {}
    virtual void OnEnable() {}
    virtual void Update(double deltaSeconds) { (void)deltaSeconds; }
    virtual void OnDisable() {}
    virtual void OnDestroy() {}

protected:
    [[nodiscard]] Scene& GetScene() const { return *scene_; }
    [[nodiscard]] entt::entity GetEntity() const { return entity_; }

private:
    friend class Scene;
    Scene* scene_ = nullptr;
    entt::entity entity_ = entt::null;
};

// Validate is pure and called before replacing the scene; factory runs after
// commit. Both receive the controller-owned, JSON-compatible data payload.
struct ControllerRegistration {
    int version = 1;
    std::function<bool(std::string_view data, std::string& error)> validate;
    std::function<std::unique_ptr<Controller>(std::string_view data)> create;
};
} // namespace engine
