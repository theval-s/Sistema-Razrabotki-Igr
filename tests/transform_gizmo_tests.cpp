#include <editor/scene_events.hpp>
#include <editor/selection.hpp>
#include <editor/transform_gizmo.hpp>
#include <editor/viewport_view.hpp>

#include <QCoreApplication>

#include <cmath>

#include <engine/rendering/FPSCamera.hpp>
#include <engine/scene/Scene.hpp>

#include <gtest/gtest.h>

namespace {

using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;
using Mode = editor::TransformGizmo::Mode;

constexpr float Tolerance = 1e-3f;
/// The gizmo's handles are this many pixels long.
constexpr float HandlePixels = 100.0f;

void expectNear(const Vector3& actual, const Vector3& expected) {
    EXPECT_NEAR(actual.x, expected.x, Tolerance);
    EXPECT_NEAR(actual.y, expected.y, Tolerance);
    EXPECT_NEAR(actual.z, expected.z, Tolerance);
}

/// Looks down -Z at the origin from 10 units away, into an 800x600 viewport.
class TransformGizmoTest : public testing::Test {
protected:
    TransformGizmoTest() {
        engine::FPSCamera camera{};
        camera.fov = 1.0f;
        camera.aspectRatio = 800.0f / 600.0f;
        camera.nearPlane = 0.1f;
        camera.farPlane = 1000.0f;
        camera.position = {0.0f, 0.0f, 10.0f};
        view = editor::ViewportView::fromCamera(camera, {800.0f, 600.0f});
    }

    /// Where `point` shows on screen.
    Vector2 screen(const Vector3& point) const {
        const auto projected = view.project(point);
        EXPECT_TRUE(projected.has_value());
        return projected.value_or(Vector2{});
    }

    /// The world length of the handles around `point`.
    float handle(const Vector3& point) const { return HandlePixels * view.pixelSize(point); }

    Vector3 worldPosition(const entt::entity entity) const { return scene.GetWorldMatrix(entity).Translation(); }

    engine::Scene scene;
    editor::SceneEvents events{scene};
    editor::Selection selection{scene, events};
    editor::TransformGizmo gizmo{scene, selection};
    editor::ViewportView view;
};

TEST_F(TransformGizmoTest, ViewProjectsAndCastsRaysThroughTheSamePoints) {
    EXPECT_NEAR(screen(Vector3::Zero).x, 400.0f, Tolerance);
    EXPECT_NEAR(screen(Vector3::Zero).y, 300.0f, Tolerance);
    const Vector3 point{1.0f, 2.0f, -3.0f};
    const auto ray = view.ray(screen(point));
    const Vector3 toPoint = point - ray.origin;
    EXPECT_NEAR(toPoint.Cross(ray.direction).Length(), 0.0f, Tolerance);
    EXPECT_GT(toPoint.Dot(ray.direction), 0.0f);
    EXPECT_FALSE(view.project({0.0f, 0.0f, 20.0f}).has_value());  // behind the camera
}

TEST_F(TransformGizmoTest, PressMissesWithoutSelectionAwayFromHandlesAndWhenDisabled) {
    EXPECT_FALSE(gizmo.press(view, screen(Vector3::Zero)));
    const entt::entity entity = scene.CreateEntity("Cube");
    selection.select(entity);
    EXPECT_FALSE(gizmo.press(view, {20.0f, 20.0f}));
    EXPECT_FALSE(gizmo.hover(view, {20.0f, 20.0f}));
    const Vector2 onX = screen(Vector3::UnitX * handle(Vector3::Zero) * 0.7f);
    EXPECT_TRUE(gizmo.hover(view, onX));
    gizmo.setEnabled(false);
    EXPECT_FALSE(gizmo.press(view, onX));
    EXPECT_FALSE(gizmo.dragging());
}

TEST_F(TransformGizmoTest, TranslateAlongAnAxisFollowsTheCursor) {
    const entt::entity entity = scene.CreateEntity("Cube");
    selection.select(entity);
    const Vector2 start = screen(Vector3::UnitX * handle(Vector3::Zero) * 0.7f);
    ASSERT_TRUE(gizmo.press(view, start));
    // Off the axis, it stays on the axis.
    gizmo.drag(view, start + Vector2{50.0f, 30.0f});
    EXPECT_NEAR(scene.GetTransform(entity).position.y, 0.0f, Tolerance);
    EXPECT_NEAR(scene.GetTransform(entity).position.z, 0.0f, Tolerance);
    // Each move applies the whole drag from the press.
    gizmo.drag(view, start + Vector2{50.0f, 0.0f});
    gizmo.release();
    EXPECT_FALSE(gizmo.dragging());
    expectNear(scene.GetTransform(entity).position, {50.0f * view.pixelSize(Vector3::Zero), 0.0f, 0.0f});
}

TEST_F(TransformGizmoTest, TranslateOnAPlaneMovesInBothItsDirections) {
    const entt::entity entity = scene.CreateEntity("Cube");
    selection.select(entity);
    // The middle of the XY plane's square.
    const Vector2 start = screen(Vector3{1.0f, 1.0f, 0.0f} * handle(Vector3::Zero) * 0.375f);
    ASSERT_TRUE(gizmo.press(view, start));
    gizmo.drag(view, start + Vector2{30.0f, -20.0f});
    gizmo.release();
    const float pixel = view.pixelSize(Vector3::Zero);
    expectNear(scene.GetTransform(entity).position, {30.0f * pixel, 20.0f * pixel, 0.0f});
}

TEST_F(TransformGizmoTest, TranslateWorksInWorldSpaceUnderATurnedScaledParent) {
    const entt::entity parent = scene.CreateEntity("Parent");
    const entt::entity child = scene.CreateEntity("Child");
    ASSERT_TRUE(scene.SetParent(child, parent));
    scene.SetTransform(parent, {{5.0f, 0.0f, 0.0f}, Quaternion::CreateFromAxisAngle(Vector3::UnitY, 1.5707963f),
                                {2.0f, 2.0f, 2.0f}});
    scene.SetTransform(child, {{1.0f, 0.0f, 0.0f}});
    selection.select(child);

    const Vector3 before = worldPosition(child);
    expectNear(before, {5.0f, 0.0f, -2.0f});
    const Vector2 start = screen(before + Vector3::UnitX * handle(before) * 0.7f);
    ASSERT_TRUE(gizmo.press(view, start));
    gizmo.drag(view, start + Vector2{40.0f, 0.0f});
    gizmo.release();
    expectNear(worldPosition(child), before + Vector3{40.0f * view.pixelSize(before), 0.0f, 0.0f});
}

TEST_F(TransformGizmoTest, ASelectedChildMovesOnlyWithItsSelectedAncestor) {
    const entt::entity parent = scene.CreateEntity("Parent");
    const entt::entity child = scene.CreateEntity("Child");
    ASSERT_TRUE(scene.SetParent(child, parent));
    scene.SetTransform(child, {{0.0f, 1.0f, 0.0f}});
    const entt::entity both[] = {child, parent};
    selection.setEntities(both);  // the parent is the primary, so the gizmo sits on it

    const Vector2 start = screen(Vector3::UnitX * handle(Vector3::Zero) * 0.7f);
    ASSERT_TRUE(gizmo.press(view, start));
    gizmo.drag(view, start + Vector2{50.0f, 0.0f});
    gizmo.release();
    const float moved = 50.0f * view.pixelSize(Vector3::Zero);
    expectNear(scene.GetTransform(parent).position, {moved, 0.0f, 0.0f});
    expectNear(scene.GetTransform(child).position, {0.0f, 1.0f, 0.0f});
    expectNear(worldPosition(child), {moved, 1.0f, 0.0f});
}

TEST_F(TransformGizmoTest, RotateTurnsByTheDistanceDraggedAlongTheRing) {
    const entt::entity entity = scene.CreateEntity("Cube");
    selection.select(entity);
    gizmo.setMode(Mode::Rotate);
    // The Z ring faces the camera; grab it half way between the X and Y rings, which are seen
    // edge on.
    const float angle = 0.7853982f;
    const Vector2 start = screen(Vector3{std::cos(angle), std::sin(angle), 0.0f} * handle(Vector3::Zero));
    ASSERT_TRUE(gizmo.press(view, start));
    // Counterclockwise on screen, along the ring: up and to the left.
    Vector2 along{-std::sin(angle), -std::cos(angle)};
    gizmo.drag(view, start + along * 50.0f);
    gizmo.release();
    // An arc as long as the radius (the handle length) is a radian.
    const Vector3 turned = Vector3::Transform(Vector3::UnitX, scene.GetTransform(entity).rotation);
    expectNear(turned, {std::cos(0.5f), std::sin(0.5f), 0.0f});
}

TEST_F(TransformGizmoTest, RotateMovesOtherSelectedEntitiesAroundThePrimary) {
    const entt::entity primary = scene.CreateEntity("Primary");
    const entt::entity other = scene.CreateEntity("Other");
    scene.SetTransform(other, {{2.0f, 0.0f, 0.0f}});
    const entt::entity both[] = {other, primary};
    selection.setEntities(both);
    gizmo.setMode(Mode::Rotate);
    const float angle = 0.7853982f;
    const Vector2 start = screen(Vector3{std::cos(angle), std::sin(angle), 0.0f} * handle(Vector3::Zero));
    ASSERT_TRUE(gizmo.press(view, start));
    // A quarter turn.
    gizmo.drag(view, start + Vector2{-std::sin(angle), -std::cos(angle)} * HandlePixels * 1.5707963f);
    gizmo.release();
    expectNear(scene.GetTransform(other).position, {0.0f, 2.0f, 0.0f});
}

TEST_F(TransformGizmoTest, ScaleStretchesAlongTheEntitysOwnAxis) {
    const entt::entity entity = scene.CreateEntity("Cube");
    selection.select(entity);
    gizmo.setMode(Mode::Scale);
    const Vector2 start = screen(Vector3::UnitX * handle(Vector3::Zero) * 0.7f);
    ASSERT_TRUE(gizmo.press(view, start));
    // Half the axis's length on screen further out: half as big again.
    gizmo.drag(view, start + Vector2{HandlePixels * 0.5f, 0.0f});
    gizmo.release();
    expectNear(scene.GetTransform(entity).scale, {1.5f, 1.0f, 1.0f});

    // The center scales evenly, and never through zero.
    ASSERT_TRUE(gizmo.press(view, screen(Vector3::Zero)));
    gizmo.drag(view, screen(Vector3::Zero) + Vector2{-1000.0f, 0.0f});
    gizmo.release();
    const Vector3 scale = scene.GetTransform(entity).scale;
    EXPECT_GT(scale.x, 0.0f);
    EXPECT_NEAR(scale.x / scale.y, 1.5f, Tolerance);
    EXPECT_NEAR(scale.y, scale.z, Tolerance);
}

TEST_F(TransformGizmoTest, CancelPutsEverythingBack) {
    const entt::entity entity = scene.CreateEntity("Cube");
    scene.SetTransform(entity, {{1.0f, 2.0f, 3.0f}});
    selection.select(entity);
    const Vector3 pivot{1.0f, 2.0f, 3.0f};
    const Vector2 start = screen(pivot + Vector3::UnitX * handle(pivot) * 0.7f);
    ASSERT_TRUE(gizmo.press(view, start));
    gizmo.drag(view, start + Vector2{80.0f, 0.0f});
    EXPECT_GT(scene.GetTransform(entity).position.x, 1.5f);
    gizmo.cancel();
    EXPECT_FALSE(gizmo.dragging());
    expectNear(scene.GetTransform(entity).position, pivot);
}

}  // namespace

int main(int argc, char** argv) {
    // SceneEvents queues its signals on the event loop.
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
