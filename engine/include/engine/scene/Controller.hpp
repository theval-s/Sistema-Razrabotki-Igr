#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <entt/entity/entity.hpp>
#include <engine/scene/StateValue.hpp>

namespace engine {
class Scene;

// Only authored data belongs in the scene file. Runtime fields in a Controller
// instance are discarded when its scene is unloaded.
struct ControllerDefinition {
    std::string type;
    int version = 1;
    bool enabled = true;
    StateValue data; // authored JSON-compatible value, retained if unavailable
};

class Controller {
public:
    virtual ~Controller() = default;
    virtual void LoadAuthored(const StateValue& data) { (void)data; }
    virtual void SaveAuthored(StateValue& data) const { (void)data; }
    virtual void LoadGameState(const StateValue& state) { (void)state; }
    virtual void SaveGameState(StateValue& state) const { (void)state; }
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

// Validate is side-effect free and called before replacing the scene.
// Factory creates an unattached controller; LoadAuthored runs after commitment.
struct ControllerRegistration {
    int version = 1;
    std::function<bool(const StateValue& data, std::string& error)> validate;
    std::function<std::unique_ptr<Controller>()> create;
};
} // namespace engine
