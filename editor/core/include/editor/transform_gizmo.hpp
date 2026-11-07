#pragma once

#include <array>
#include <optional>
#include <vector>

#include <SimpleMath.h>
#include <entt/entity/entity.hpp>

#include <engine/scene/Transform.hpp>

#include <editor/editor_core_export.hpp>
#include <editor/viewport_view.hpp>

namespace engine {
class DebugDraw;
class Scene;
}  // namespace engine

namespace editor {

class SceneHistory;
class Selection;

/// Handles in the viewport for moving, rotating and scaling the selection with the mouse.
/// They sit on the primary entity and stay the same size on screen.
/// Translate and rotate act around the primary entity; scale changes each entity's own scale.
/// A selected entity whose ancestor is selected too moves with that ancestor only.
/// Edits go through Scene::SetTransform(), so they are reported like any other; with a history, each drag is one step.
class EDITOR_CORE_API TransformGizmo {
public:
    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;

    enum class Mode { Translate, Rotate, Scale };
    enum class Space { World, Local };

    TransformGizmo(engine::Scene& scene, const Selection& selection, SceneHistory* history = nullptr);

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    /// Ends a drag first, keeping where it got to.
    void setMode(Mode mode);
    /// The axes for translate and rotate: the world's, or the primary entity's. Scale always
    /// uses the entity's own.
    [[nodiscard]] Space space() const noexcept { return space_; }
    /// Ends a drag first, keeping where it got to.
    void setSpace(Space space);
    /// Off, the gizmo isn't drawn and ignores the mouse, e.g. during Play. Cancels a drag.
    void setEnabled(bool enabled);

    void draw(engine::DebugDraw& debugDraw, const ViewportView& view) const;

    /// Highlights the handle under `cursor`; returns whether there is one.
    bool hover(const ViewportView& view, Vector2 cursor);
    /// Starts dragging the handle under `cursor`. False when there is none: the click is the
    /// scene's.
    bool press(const ViewportView& view, Vector2 cursor);
    /// Applies the whole drag so far, from where the entities were at the press.
    void drag(const ViewportView& view, Vector2 cursor);
    /// Ends the drag, keeping where it got to.
    void release();
    /// Ends the drag and puts everything back.
    void cancel();
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }

private:
    struct Handle {
        enum Kind { None, Axis, Plane, Center } kind = None;
        /// The axis, or the plane's normal.
        int axis = 0;
        friend bool operator==(const Handle&, const Handle&) = default;
    };
    /// Where the gizmo is and how it is turned.
    struct Frame {
        Vector3 pivot;
        std::array<Vector3, 3> axes;
        /// World length of the handles.
        float size = 1.0f;
    };
    struct Target {
        entt::entity entity;
        engine::Transform local;
        DirectX::SimpleMath::Matrix world;
        DirectX::SimpleMath::Matrix parentInverse;
    };
    struct Drag {
        Mode mode;
        Handle handle;
        Frame frame;
        Vector2 cursor;
        /// Translate: where the mouse ray met the axis (x only) or the plane.
        Vector3 grab;
        /// Rotate: the ring's direction on screen; scale: the axis's. Unit length.
        Vector2 direction;
        /// Rotate: pixels per radian; scale: the axis's length on screen.
        float pixels = 1.0f;
        std::vector<Target> targets;
    };

    [[nodiscard]] std::optional<Frame> currentFrame(const ViewportView& view) const;
    [[nodiscard]] Handle pick(const ViewportView& view, const Frame& frame, Vector2 cursor) const;
    [[nodiscard]] std::vector<Target> targets() const;

    engine::Scene& scene_;
    const Selection& selection_;
    SceneHistory* history_;
    Mode mode_ = Mode::Translate;
    Space space_ = Space::World;
    bool enabled_ = true;
    Handle hovered_;
    std::optional<Drag> drag_;
};

}  // namespace editor
