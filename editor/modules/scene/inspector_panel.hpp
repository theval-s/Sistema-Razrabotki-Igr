#pragma once

#include <QWidget>

#include <unordered_set>
#include <vector>

#include <entt/core/fwd.hpp>
#include <entt/entity/entity.hpp>

#include <engine/scene/Scene.hpp>

class PropertyForm;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QVBoxLayout;

namespace editor {
class EditorContext;
}

/// Shows and edits the primary selected entity: it's name, components and etc.
class InspectorPanel : public QWidget {
public:
    explicit InspectorPanel(editor::EditorContext& context, QWidget* parent = nullptr);

private:
    using ComponentList = std::vector<engine::Scene::ComponentType>;

    /// entt::null clears the panel.
    void setEntity(entt::entity entity);
    /// Scene -> widgets.
    void refresh();
    /// Widgets -> scene.
    void applyName();
    /// The entity's components to show, in the order to show them.
    [[nodiscard]] ComponentList listedComponents() const;
    /// Replaces the sections with ones for `types`, from listedComponents().
    void rebuildSections(const ComponentList& types);
    void addSection(const engine::Scene::ComponentType& type);
    /// Lists the components the entity can still get.
    void fillAddMenu();

    editor::EditorContext& context_;
    entt::entity entity_ = entt::null;

    QWidget* header_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLabel* id_ = nullptr;
    QWidget* sections_ = nullptr;
    QVBoxLayout* sections_layout_ = nullptr;
    QPushButton* add_ = nullptr;
    QMenu* add_menu_ = nullptr;
    /// The components the sections show, from listedComponents().
    std::vector<entt::id_type> shown_types_;
    /// Of the sections that have one. Owned by the sections.
    std::vector<PropertyForm*> forms_;
    /// Sections the user folded, kept across entities.
    std::unordered_set<entt::id_type> collapsed_;
};
