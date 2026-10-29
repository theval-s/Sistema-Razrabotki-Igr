#pragma once

#include <QWidget>

#include <array>

#include <entt/entity/entity.hpp>

#include <engine/scene/Transform.hpp>

class QDoubleSpinBox;
class QLineEdit;

namespace engine {
class Scene;
}

namespace editor {
class SceneDocument;
class Selection;
}

/// Shows and edits the primary selected entity's name and local transform. Polls the
/// scene, so changes made elsewhere (e.g. the editor camera flying) show up too.
class InspectorPanel : public QWidget {
public:
    InspectorPanel(engine::Scene& scene, editor::Selection& selection, editor::SceneDocument& document,
                   QWidget* parent = nullptr);

private:
    using Vec3Boxes = std::array<QDoubleSpinBox*, 3>;

    /// entt::null clears the panel.
    void setEntity(entt::entity entity);

    Vec3Boxes makeVec3Row(double step, int decimals);
    /// Scene -> widgets.
    void refresh();
    /// Widgets -> scene.
    void applyName();
    void applyTransform();

    engine::Scene& scene_;
    /// Told about transform edits, which the scene's revision doesn't count.
    editor::SceneDocument& document_;
    entt::entity entity_ = entt::null;
    /// What the widgets currently show, to tell our own writes from changes made elsewhere.
    /// Rotation is edited as Euler angles kept in the widgets: converting the quaternion
    /// back on every edit would make the angles jump around.
    engine::Transform shown_{};

    QWidget* form_ = nullptr;
    QLineEdit* name_ = nullptr;
    Vec3Boxes position_{};
    Vec3Boxes rotation_{};
    Vec3Boxes scale_{};
};
