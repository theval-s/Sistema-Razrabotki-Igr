#include <editor/transform_gizmo.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <engine/rendering/DebugDraw.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/scene_history.hpp>
#include <editor/selection.hpp>

namespace editor {

namespace {

using DirectX::SimpleMath::Color;
using DirectX::SimpleMath::Matrix;
using DirectX::SimpleMath::Quaternion;
using Vector2 = TransformGizmo::Vector2;
using Vector3 = TransformGizmo::Vector3;

// Sizes in logical pixels, or as a fraction of the handles' length.
/// The handles' length.
constexpr float Size = 100.0f;
/// How close the cursor must get to a handle to grab it.
constexpr float HitDistance = 8.0f;
constexpr float CenterHitDistance = 12.0f;
/// An axis shorter than this on screen points at the viewer: it can't be grabbed or seen.
constexpr float MinAxisPixels = 12.0f;
/// A plane seen more edge on than this (the cosine to the view) can't be grabbed or seen.
constexpr float MinPlaneFacing = 0.15f;
constexpr float ArrowHead = 0.2f;
constexpr float PlaneNear = 0.25f;
constexpr float PlaneFar = 0.5f;
constexpr float ScaleBox = 0.12f;
constexpr float CenterBox = 0.16f;
constexpr int RingSegments = 64;
constexpr float Tau = 6.28318530718f;
/// Scale can't drag below this factor, where the entity would vanish or turn inside out.
constexpr float MinScaleFactor = 0.01f;

const Color HighlightColor{1.0f, 0.85f, 0.1f};
const Color CenterColor{0.85f, 0.85f, 0.85f};
const Color OutlineColor{0.6f, 0.6f, 0.6f};

const std::array<Vector3, 3> WorldAxes{Vector3::UnitX, Vector3::UnitY, Vector3::UnitZ};

float& component(Vector3& vector, const int axis) {
    return axis == 0 ? vector.x : axis == 1 ? vector.y : vector.z;
}

float distanceToSegment(const Vector2 point, const Vector2 a, const Vector2 b) {
    const Vector2 ab = b - a;
    const float length = ab.LengthSquared();
    const float t = length > 0.0f ? std::clamp((point - a).Dot(ab) / length, 0.0f, 1.0f) : 0.0f;
    return Vector2::Distance(point, a + ab * t);
}

float cross(const Vector2 a, const Vector2 b) {
    return a.x * b.y - a.y * b.x;
}

bool insideQuad(const Vector2 point, const std::array<Vector2, 4>& corners) {
    bool positive = false;
    bool negative = false;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const float side = cross(corners[(i + 1) % 4] - corners[i], point - corners[i]);
        positive |= side > 0.0f;
        negative |= side < 0.0f;
    }
    return !(positive && negative);
}

/// Where along the line through `origin` in `direction` (unit) it comes closest to the ray;
/// nothing when they are parallel.
std::optional<float> closestOnLine(const ViewportView::Ray& ray, const Vector3& origin, const Vector3& direction) {
    const float cosine = direction.Dot(ray.direction);
    const float denominator = 1.0f - cosine * cosine;
    if (denominator < 1e-6f) return std::nullopt;
    const Vector3 offset = origin - ray.origin;
    return (cosine * ray.direction.Dot(offset) - direction.Dot(offset)) / denominator;
}

/// Where the ray meets the plane through `origin`; nothing when it doesn't, ahead of the camera.
std::optional<Vector3> onPlane(const ViewportView::Ray& ray, const Vector3& origin, const Vector3& normal) {
    const float facing = normal.Dot(ray.direction);
    if (std::abs(facing) < 1e-6f) return std::nullopt;
    const float distance = normal.Dot(origin - ray.origin) / facing;
    if (distance < 0.0f) return std::nullopt;
    return ray.origin + ray.direction * distance;
}

Vector3 towardCamera(const ViewportView& view, const Vector3& point) {
    Vector3 direction = view.cameraPosition() - point;
    direction.Normalize();
    return direction;
}

/// The plane's four corners, between the other two axes.
std::array<Vector3, 4> planeCorners(const Vector3& pivot, const std::array<Vector3, 3>& axes, const int normal,
                                    const float size) {
    const Vector3 u = axes[(normal + 1) % 3] * size;
    const Vector3 v = axes[(normal + 2) % 3] * size;
    return {pivot + u * PlaneNear + v * PlaneNear, pivot + u * PlaneFar + v * PlaneNear,
            pivot + u * PlaneFar + v * PlaneFar, pivot + u * PlaneNear + v * PlaneFar};
}

/// A point on the ring around `axis`, `angle` radians round.
Vector3 ringPoint(const Vector3& pivot, const std::array<Vector3, 3>& axes, const int axis, const float size,
                  const float angle) {
    return pivot + size * (std::cos(angle) * axes[(axis + 1) % 3] + std::sin(angle) * axes[(axis + 2) % 3]);
}

/// Only the half of a ring facing the viewer is shown: the back half would cross the front.
bool facesViewer(const Vector3& point, const Vector3& pivot, const Vector3& toCamera, const float size) {
    return (point - pivot).Dot(toCamera) >= -0.05f * size;
}

}  // namespace

TransformGizmo::TransformGizmo(engine::Scene& scene, const Selection& selection, SceneHistory* history)
    : scene_(scene), selection_(selection), history_(history) {}

void TransformGizmo::setMode(const Mode mode) {
    release();
    mode_ = mode;
    hovered_ = {};
}

void TransformGizmo::setSpace(const Space space) {
    release();
    space_ = space;
}

void TransformGizmo::setEnabled(const bool enabled) {
    if (!enabled) cancel();
    enabled_ = enabled;
    hovered_ = {};
}

std::optional<TransformGizmo::Frame> TransformGizmo::currentFrame(const ViewportView& view) const {
    const entt::entity primary = selection_.primary();
    if (!enabled_ || !scene_.IsValid(primary)) return std::nullopt;
    const Matrix world = scene_.GetWorldMatrix(primary);
    Frame frame{world.Translation(), WorldAxes, 1.0f};
    if (space_ == Space::Local || mode_ == Mode::Scale) {
        for (int i = 0; i < 3; ++i) {
            Vector3 axis = Vector3::TransformNormal(WorldAxes[i], world);
            // A zero scale leaves no direction: keep the world's.
            if (axis.LengthSquared() > 1e-12f) {
                axis.Normalize();
                frame.axes[i] = axis;
            }
        }
    }
    frame.size = Size * view.pixelSize(frame.pivot);
    return frame;
}

TransformGizmo::Handle TransformGizmo::pick(const ViewportView& view, const Frame& frame, const Vector2 cursor) const {
    const auto center = view.project(frame.pivot);
    if (!center) return {};
    const Vector3 toCamera = towardCamera(view, frame.pivot);

    if (mode_ == Mode::Rotate) {
        Handle nearest;
        float best = HitDistance;
        for (int axis = 0; axis < 3; ++axis) {
            for (int i = 0; i < RingSegments; ++i) {
                const Vector3 a = ringPoint(frame.pivot, frame.axes, axis, frame.size, Tau * i / RingSegments);
                const Vector3 b = ringPoint(frame.pivot, frame.axes, axis, frame.size, Tau * (i + 1) / RingSegments);
                if (!facesViewer((a + b) * 0.5f, frame.pivot, toCamera, frame.size)) continue;
                const auto screenA = view.project(a);
                const auto screenB = view.project(b);
                if (!screenA || !screenB) continue;
                if (const float distance = distanceToSegment(cursor, *screenA, *screenB); distance <= best) {
                    best = distance;
                    nearest = {Handle::Axis, axis};
                }
            }
        }
        return nearest;
    }

    if (mode_ == Mode::Scale && Vector2::Distance(cursor, *center) <= CenterHitDistance) {
        return {Handle::Center, 0};
    }
    if (mode_ == Mode::Translate) {
        for (int normal = 0; normal < 3; ++normal) {
            if (std::abs(frame.axes[normal].Dot(toCamera)) < MinPlaneFacing) continue;
            std::array<Vector2, 4> corners;
            bool visible = true;
            const auto world = planeCorners(frame.pivot, frame.axes, normal, frame.size);
            for (std::size_t i = 0; i < corners.size() && visible; ++i) {
                const auto screen = view.project(world[i]);
                visible = screen.has_value();
                if (visible) corners[i] = *screen;
            }
            if (visible && insideQuad(cursor, corners)) return {Handle::Plane, normal};
        }
    }
    Handle nearest;
    float best = HitDistance;
    for (int axis = 0; axis < 3; ++axis) {
        const auto end = view.project(frame.pivot + frame.axes[axis] * frame.size);
        if (!end || Vector2::Distance(*center, *end) < MinAxisPixels) continue;
        if (const float distance = distanceToSegment(cursor, *center, *end); distance <= best) {
            best = distance;
            nearest = {Handle::Axis, axis};
        }
    }
    return nearest;
}

std::vector<TransformGizmo::Target> TransformGizmo::targets() const {
    std::vector<Target> targets;
    for (const entt::entity entity : selection_.entities()) {
        if (!scene_.IsValid(entity)) continue;
        bool underSelected = false;
        for (entt::entity parent = scene_.GetParent(entity); parent != entt::null && !underSelected;
             parent = scene_.GetParent(parent)) {
            underSelected = selection_.contains(parent);
        }
        if (underSelected) continue;
        const entt::entity parent = scene_.GetParent(entity);
        targets.push_back({entity, scene_.GetTransform(entity), scene_.GetWorldMatrix(entity),
                           parent != entt::null ? scene_.GetWorldMatrix(parent).Invert() : Matrix::Identity});
    }
    return targets;
}

void TransformGizmo::draw(engine::DebugDraw& debugDraw, const ViewportView& view) const {
    const auto found = currentFrame(view);
    if (!found) return;
    const Frame& frame = *found;
    const Handle active = drag_ ? drag_->handle : hovered_;
    const auto color = [&](const Handle handle, const Color& normal) {
        return handle == active ? HighlightColor : normal;
    };
    const Vector3 toCamera = towardCamera(view, frame.pivot);
    const auto center = view.project(frame.pivot);
    const auto axisVisible = [&](const int axis) {
        const auto end = view.project(frame.pivot + frame.axes[axis] * frame.size);
        return center && end && Vector2::Distance(*center, *end) >= MinAxisPixels;
    };
    const Matrix basis(frame.axes[0], frame.axes[1], frame.axes[2]);

    switch (mode_) {
        case Mode::Translate:
            for (int normal = 0; normal < 3; ++normal) {
                if (std::abs(frame.axes[normal].Dot(toCamera)) < MinPlaneFacing) continue;
                const auto corners = planeCorners(frame.pivot, frame.axes, normal, frame.size);
                const Color planeColor = color({Handle::Plane, normal}, engine::DebugDraw::AxisColor(normal));
                for (std::size_t i = 0; i < corners.size(); ++i) {
                    debugDraw.Line(corners[i], corners[(i + 1) % corners.size()], planeColor);
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                if (!axisVisible(axis)) continue;
                debugDraw.Arrow(frame.pivot, frame.pivot + frame.axes[axis] * frame.size, frame.size * ArrowHead,
                           color({Handle::Axis, axis}, engine::DebugDraw::AxisColor(axis)));
            }
            break;
        case Mode::Rotate: {
            const Matrix camera = view.view.Invert();
            debugDraw.Circle(frame.pivot, camera.Right(), camera.Up(), frame.size, OutlineColor);
            for (int axis = 0; axis < 3; ++axis) {
                const Color ringColor = color({Handle::Axis, axis}, engine::DebugDraw::AxisColor(axis));
                for (int i = 0; i < RingSegments; ++i) {
                    const Vector3 a = ringPoint(frame.pivot, frame.axes, axis, frame.size, Tau * i / RingSegments);
                    const Vector3 b =
                        ringPoint(frame.pivot, frame.axes, axis, frame.size, Tau * (i + 1) / RingSegments);
                    if (facesViewer((a + b) * 0.5f, frame.pivot, toCamera, frame.size)) debugDraw.Line(a, b, ringColor);
                }
            }
            break;
        }
        case Mode::Scale:
            for (int axis = 0; axis < 3; ++axis) {
                if (!axisVisible(axis)) continue;
                const Color axisColor = color({Handle::Axis, axis}, engine::DebugDraw::AxisColor(axis));
                const Vector3 end = frame.pivot + frame.axes[axis] * frame.size;
                debugDraw.Line(frame.pivot, end, axisColor);
                debugDraw.Box(Matrix::CreateScale(frame.size * ScaleBox) * basis * Matrix::CreateTranslation(end),
                         axisColor);
            }
            debugDraw.Box(Matrix::CreateScale(frame.size * CenterBox) * basis * Matrix::CreateTranslation(frame.pivot),
                     color({Handle::Center, 0}, CenterColor));
            break;
    }
}

bool TransformGizmo::hover(const ViewportView& view, const Vector2 cursor) {
    if (drag_) return true;
    const auto found = currentFrame(view);
    hovered_ = found ? pick(view, *found, cursor) : Handle{};
    return hovered_.kind != Handle::None;
}

bool TransformGizmo::press(const ViewportView& view, const Vector2 cursor) {
    if (drag_) return true;
    const auto found = currentFrame(view);
    if (!found) return false;
    Drag started{mode_, pick(view, *found, cursor), *found, cursor};
    if (started.handle.kind == Handle::None) return false;
    const Frame& frame = started.frame;
    const auto ray = view.ray(cursor);
    const Vector3& axis = frame.axes[started.handle.axis];

    switch (mode_) {
        case Mode::Translate:
            if (started.handle.kind == Handle::Axis) {
                const auto along = closestOnLine(ray, frame.pivot, axis);
                if (!along) return false;
                started.grab.x = *along;
            } else {
                const auto point = onPlane(ray, frame.pivot, axis);
                if (!point) return false;
                started.grab = *point;
            }
            break;
        case Mode::Rotate: {
            // The ring's direction where it was grabbed: dragging along it turns the ring with
            // the cursor, however the ring is seen.
            Vector3 grabbed = ringPoint(frame.pivot, frame.axes, started.handle.axis, frame.size, 0.0f);
            float best = std::numeric_limits<float>::max();
            for (int i = 0; i < RingSegments; ++i) {
                const Vector3 point = ringPoint(frame.pivot, frame.axes, started.handle.axis, frame.size,
                                                Tau * i / RingSegments);
                const auto screen = view.project(point);
                if (screen && Vector2::Distance(*screen, cursor) < best) {
                    best = Vector2::Distance(*screen, cursor);
                    grabbed = point;
                }
            }
            Vector3 tangent = axis.Cross(grabbed - frame.pivot);
            tangent.Normalize();
            const auto from = view.project(grabbed);
            const auto to = view.project(grabbed + tangent * frame.size * 0.1f);
            started.direction = from && to ? *to - *from : Vector2{1.0f, 0.0f};
            if (started.direction.LengthSquared() < 1e-6f) started.direction = Vector2{1.0f, 0.0f};
            started.direction.Normalize();
            // An arc as long as the radius is a radian.
            started.pixels = Size;
            break;
        }
        case Mode::Scale:
            if (started.handle.kind == Handle::Axis) {
                const auto from = view.project(frame.pivot);
                const auto to = view.project(frame.pivot + axis * frame.size);
                if (!from || !to) return false;
                started.direction = *to - *from;
                started.pixels = started.direction.Length();
                if (started.pixels < 1.0f) return false;
                started.direction /= started.pixels;
            } else {
                started.pixels = Size;
            }
            break;
    }
    started.targets = targets();
    if (started.targets.empty()) return false;
    hovered_ = started.handle;
    drag_ = std::move(started);
    if (history_ != nullptr) {
        history_->begin(mode_ == Mode::Translate ? QObject::tr("Move") : mode_ == Mode::Rotate ? QObject::tr("Rotate")
                                                                                              : QObject::tr("Scale"));
    }
    return true;
}

void TransformGizmo::drag(const ViewportView& view, const Vector2 cursor) {
    if (!drag_) return;
    const Drag& active = *drag_;
    const Frame& frame = active.frame;
    const Vector3& axis = frame.axes[active.handle.axis];
    const auto ray = view.ray(cursor);

    switch (active.mode) {
        case Mode::Translate: {
            Vector3 offset;
            if (active.handle.kind == Handle::Axis) {
                const auto along = closestOnLine(ray, frame.pivot, axis);
                if (!along) return;
                offset = axis * (*along - active.grab.x);
            } else {
                const auto point = onPlane(ray, frame.pivot, axis);
                if (!point) return;
                offset = *point - active.grab;
            }
            for (const Target& target : active.targets) {
                if (!scene_.IsValid(target.entity)) continue;
                engine::Transform transform = target.local;
                transform.position = Vector3::Transform(target.world.Translation() + offset, target.parentInverse);
                scene_.SetTransform(target.entity, transform);
            }
            break;
        }
        case Mode::Rotate: {
            const float angle = (cursor - active.cursor).Dot(active.direction) / active.pixels;
            const Matrix turn = Matrix::CreateTranslation(-frame.pivot) *
                                Matrix::CreateFromQuaternion(Quaternion::CreateFromAxisAngle(axis, angle)) *
                                Matrix::CreateTranslation(frame.pivot);
            for (const Target& target : active.targets) {
                if (!scene_.IsValid(target.entity)) continue;
                Matrix local = target.world * turn * target.parentInverse;
                Vector3 scale;
                Quaternion rotation;
                Vector3 position;
                engine::Transform transform = target.local;
                transform.position = local.Translation();
                // The scale stays as it was rather than picking up rounding from the decomposition.
                if (local.Decompose(scale, rotation, position)) transform.rotation = rotation;
                scene_.SetTransform(target.entity, transform);
            }
            break;
        }
        case Mode::Scale: {
            const Vector2 moved = cursor - active.cursor;
            const float factor = std::max(MinScaleFactor, active.handle.kind == Handle::Axis
                                                              ? 1.0f + moved.Dot(active.direction) / active.pixels
                                                              : 1.0f + (moved.x - moved.y) / active.pixels);
            for (const Target& target : active.targets) {
                if (!scene_.IsValid(target.entity)) continue;
                engine::Transform transform = target.local;
                if (active.handle.kind == Handle::Axis) {
                    component(transform.scale, active.handle.axis) *= factor;
                } else {
                    transform.scale *= factor;
                }
                scene_.SetTransform(target.entity, transform);
            }
            break;
        }
    }
}

void TransformGizmo::release() {
    if (!drag_) return;
    drag_.reset();
    if (history_ != nullptr) history_->end();
}

void TransformGizmo::cancel() {
    if (!drag_) return;
    for (const Target& target : drag_->targets) {
        if (scene_.IsValid(target.entity)) scene_.SetTransform(target.entity, target.local);
    }
    drag_.reset();
    if (history_ != nullptr) history_->cancel();
}

}  // namespace editor
