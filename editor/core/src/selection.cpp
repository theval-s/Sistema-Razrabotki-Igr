#include <editor/selection.hpp>

#include <algorithm>
#include <utility>

#include <engine/scene/Scene.hpp>

#include <editor/scene_events.hpp>

namespace editor {

Selection::Selection(const engine::Scene& scene, SceneEvents& events, QObject* parent)
    : QObject(parent), scene_(scene) {
    connect(&events, &SceneEvents::changed, this, &Selection::dropDestroyed);
}

std::span<const entt::entity> Selection::entities() const noexcept {
    return entities_;
}

entt::entity Selection::primary() const noexcept {
    return entities_.empty() ? entt::null : entities_.back();
}

bool Selection::contains(const entt::entity entity) const {
    return std::ranges::find(entities_, entity) != entities_.end();
}

bool Selection::isEmpty() const noexcept {
    return entities_.empty();
}

void Selection::select(const entt::entity entity) {
    setEntities({&entity, entity != entt::null ? 1u : 0u});
}

void Selection::setEntities(const std::span<const entt::entity> entities) {
    std::vector<entt::entity> kept;
    kept.reserve(entities.size());
    for (const entt::entity entity : entities) {
        if (scene_.IsValid(entity) && std::ranges::find(kept, entity) == kept.end()) {
            kept.push_back(entity);
        }
    }
    assign(std::move(kept));
}

void Selection::add(const entt::entity entity) {
    if (!scene_.IsValid(entity)) {
        return;
    }
    std::vector<entt::entity> entities = entities_;
    std::erase(entities, entity);
    entities.push_back(entity);
    assign(std::move(entities));
}

void Selection::remove(const entt::entity entity) {
    std::vector<entt::entity> entities = entities_;
    std::erase(entities, entity);
    assign(std::move(entities));
}

void Selection::toggle(const entt::entity entity) {
    if (contains(entity)) {
        remove(entity);
    } else {
        add(entity);
    }
}

void Selection::clear() {
    assign({});
}

void Selection::assign(std::vector<entt::entity> entities) {
    if (entities != entities_) {
        entities_ = std::move(entities);
        emit changed();
    }
}

void Selection::dropDestroyed(const SceneChanges& changes) {
    if (!changes.structure) {
        return;
    }
    std::vector<entt::entity> entities = entities_;
    std::erase_if(entities, [this](const entt::entity entity) { return !scene_.IsValid(entity); });
    assign(std::move(entities));
}

}  // namespace editor
