#pragma once

#include <QObject>

#include <cstdint>
#include <span>
#include <vector>

#include <entt/entity/entity.hpp>

#include <editor/editor_core_export.hpp>

namespace engine {
class Scene;
}

namespace editor {

struct SceneChanges;
class SceneEvents;

/// The entities selected in the editor, shared by everything that shows or acts on them
/// (hierarchy, inspector, viewport). Kept in selection order; the last one is the primary,
/// which single-entity views such as the inspector show.
/// Destroyed entities drop out on their own once SceneEvents reports them: until then an entity
/// here may already be gone, so check Scene::IsValid() before touching it.
class EDITOR_CORE_API Selection : public QObject {
    Q_OBJECT

public:
    Selection(const engine::Scene& scene, SceneEvents& events, QObject* parent = nullptr);

    [[nodiscard]] std::span<const entt::entity> entities() const noexcept;
    /// entt::null when nothing is selected.
    [[nodiscard]] entt::entity primary() const noexcept;
    [[nodiscard]] bool contains(entt::entity entity) const;
    [[nodiscard]] bool isEmpty() const noexcept;

    /// Selects just `entity`; entt::null clears the selection.
    void select(entt::entity entity);
    /// Replaces the selection; the last entity becomes the primary.
    /// Invalid entities and repeats are skipped.
    void setEntities(std::span<const entt::entity> entities);
    /// Adds `entity` to the selection as the new primary.
    void add(entt::entity entity);
    void remove(entt::entity entity);
    /// Ctrl+click: removes `entity` if selected, adds it otherwise.
    void toggle(entt::entity entity);
    void clear();

signals:
    /// The selected entities or the primary changed.
    void changed();

private:
    /// Emits changed() if `entities` differs from the current selection.
    void assign(std::vector<entt::entity> entities);
    void dropDestroyed(const SceneChanges& changes);

    const engine::Scene& scene_;
    std::vector<entt::entity> entities_;
};

}  // namespace editor
