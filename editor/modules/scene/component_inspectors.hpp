#pragma once

#include <QString>

#include <span>

#include <entt/core/fwd.hpp>
#include <entt/entity/entity.hpp>

class PropertyForm;

namespace editor {
class EditorContext;
}

/// What the inspector knows about one component type: its title, the fields that edit it, and how
/// to add and remove it. Component types missing here still show up, by their C++ name, without
/// fields, and can't be added or removed.
struct ComponentInspector {
    /// entt::type_hash<T>::value(), as Scene::GetComponentTypes() reports it.
    entt::id_type type;
    QString title;
    /// Adds fields bound to the component of `entity` to `form`.
    void (*build)(PropertyForm& form, editor::EditorContext& context, entt::entity entity);
    /// Gives `entity` the component with sensible defaults. Null for required components,
    /// which every entity has.
    void (*add)(editor::EditorContext& context, entt::entity entity) = nullptr;
    /// Null for required components.
    void (*remove)(editor::EditorContext& context, entt::entity entity) = nullptr;
};

/// Gives `entity` a MeshRenderer drawing the built-in cube with the default material, referenced
/// by asset id so it saves.
void addMeshRenderer(editor::EditorContext& context, entt::entity entity);

/// Every component type the inspector knows, in the order it lists them.
[[nodiscard]] std::span<const ComponentInspector> componentInspectors();

/// Components the inspector doesn't list, as it or another panel shows them already (the name,
/// the ID, the hierarchy).
[[nodiscard]] bool isHiddenComponent(entt::id_type type);
