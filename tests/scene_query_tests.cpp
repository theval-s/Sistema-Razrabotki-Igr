#include <engine/engine.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/scene/SceneQueries.hpp>
#include <engine/rendering/FPSCamera.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using namespace engine;
using DirectX::SimpleMath::Vector2;
using DirectX::SimpleMath::Vector3;

namespace {
entt::entity Box(Scene& scene, const char* name, Vector3 position, entt::entity parent = entt::null) {
    const auto entity = scene.CreateEntity(name, parent);
    scene.AddComponent<Bounds>(entity, Bounds{{{-1, -1, -1}, {1, 1, 1}}});
    auto transform = scene.GetTransform(entity);
    transform.position = position;
    scene.SetTransform(entity, transform);
    return entity;
}
}

TEST(SceneQueries, BoundsHitsFilteringAndLegacy) {
    Engine engine;
    Scene scene;
    auto& assets = engine.GetAssets();
    const auto nearest = Box(scene, "near", {0, 0, -3});
    const auto child = Box(scene, "child", {0, 0, -3}, nearest);
    scene.AddComponent<Collider>(nearest, Collider{});
    scene.TryGetComponent<Collider>(nearest)->isTrigger = true;
    Ray ray{{}, {0, 0, -100}, 20};
    std::vector<RayHit> hits;
    std::optional<RayHit> closest;
    std::string error;
    ASSERT_TRUE(RaycastBoundsAll(scene, assets, ray, {}, hits, error)) << error;
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].entity, nearest);
    EXPECT_FLOAT_EQ(hits[0].distance, 2);
    EXPECT_FLOAT_EQ(hits[0].point.z, -2);
    ASSERT_TRUE(hits[0].normal);
    EXPECT_FLOAT_EQ(hits[0].normal->z, 1);
    EXPECT_TRUE(hits[0].isTrigger);
    ASSERT_TRUE(RaycastBounds(scene, assets, ray, {}, closest, error));
    ASSERT_TRUE(closest);
    EXPECT_EQ(closest->entity, nearest);
    ASSERT_TRUE(RaycastBoundsAll(scene, assets, ray, {{nearest}, false}, hits, error));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, child);
    ASSERT_TRUE(RaycastBoundsAll(scene, assets, ray, {{nearest}, true}, hits, error));
    EXPECT_TRUE(hits.empty());
    ray.origin = {0, 0, -3};
    ray.maxDistance = 1;
    ASSERT_TRUE(RaycastBounds(scene, assets, ray, {}, closest, error));
    ASSERT_TRUE(closest);
    EXPECT_FLOAT_EQ(closest->distance, 0);
    EXPECT_FALSE(closest->normal);
    auto legacy = RaycastAll(scene, assets, ray.origin, ray.direction, 0);
    ASSERT_EQ(legacy.size(), 1u);
    EXPECT_EQ(legacy[0].entity, nearest);
    EXPECT_TRUE(legacy[0].isTrigger);
    EXPECT_FLOAT_EQ(legacy[0].point.z, -3);
    EXPECT_TRUE(RaycastAll(scene, assets, {}, {}, 1).empty());
}

TEST(SceneQueries, ValidatesRaysAndExtremeDirections) {
    Engine engine;
    Scene scene;
    auto& assets = engine.GetAssets();
    Box(scene, "box", {0, 0, -3});
    std::optional<RayHit> hit;
    std::string error;
    for (float direction : {1e-30f, 1e30f}) {
        ASSERT_TRUE(RaycastBounds(scene, assets, {{}, {0, 0, -direction}, 4}, {}, hit, error)) << error;
        ASSERT_TRUE(hit);
        EXPECT_FLOAT_EQ(hit->distance, 2);
    }
    EXPECT_FALSE(RaycastBounds(scene, assets, {{}, {}, 4}, {}, hit, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(RaycastBounds(scene, assets, {{}, {0, 0, -1}, 0}, {}, hit, error));
    EXPECT_FALSE(RaycastBounds(scene, assets, {{}, {0, 0, -1}, 1}, {{entt::null}, false}, hit, error));
}

TEST(SceneQueries, ScreenRaysProjectionAndValidation) {
    Scene scene;
    Ray ray;
    std::string error;
    EXPECT_FALSE(ScreenPointToRay(scene, {50, 50}, {100, 100}, entt::null, ray, error));
    const auto camera = scene.CreateEntity("camera");
    scene.AddComponent<Camera>(camera, Camera{DirectX::XM_PIDIV2, 1, 11});
    scene.SetActiveCamera(camera);
    ASSERT_TRUE(ScreenPointToRay(scene, {50, 50}, {100, 100}, entt::null, ray, error)) << error;
    EXPECT_NEAR(ray.origin.z, -1, 1e-5f);
    EXPECT_NEAR(ray.direction.z, -1, 1e-5f);
    EXPECT_NEAR(ray.maxDistance, 10, 1e-5f);
    ASSERT_TRUE(ScreenPointToRay(scene, {0, 0}, {100, 100}, camera, ray, error));
    EXPECT_NEAR(ray.origin.x, -1, 1e-5f);
    EXPECT_NEAR(ray.origin.y, 1, 1e-5f);
    EXPECT_NEAR(ray.maxDistance, 10 * std::sqrt(3.0f), 1e-4f);
    EXPECT_FALSE(ScreenPointToRay(scene, {100, 0}, {100, 100}, camera, ray, error));
    EXPECT_FALSE(ScreenPointToRay(scene, {0, 0}, {0, 100}, camera, ray, error));
    EXPECT_FALSE(ScreenPointToRay(scene, {std::numeric_limits<float>::quiet_NaN(), 0}, {100, 100}, camera, ray, error));
    scene.TryGetComponent<Camera>(camera)->farPlane = 0;
    EXPECT_FALSE(ScreenPointToRay(scene, {0, 0}, {100, 100}, camera, ray, error));
}

TEST(SceneQueries, ScreenRayUsesParentOrientationAndIgnoresRoll) {
    Scene scene;
    const auto parent = scene.CreateEntity("parent");
    auto transform = scene.GetTransform(parent);
    transform.position = {5, 2, 3};
    transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(DirectX::XM_PIDIV2, 0, 0);
    scene.SetTransform(parent, transform);
    const auto camera = scene.CreateEntity("camera", parent);
    scene.AddComponent<Camera>(camera, Camera{DirectX::XM_PIDIV2, 1, 11});
    transform = scene.GetTransform(camera);
    transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(0, 0, DirectX::XM_PIDIV2);
    scene.SetTransform(camera, transform);
    Ray ray;
    std::string error;
    ASSERT_TRUE(ScreenPointToRay(scene, {50, 0}, {100, 100}, camera, ray, error)) << error;
    EXPECT_NEAR(ray.origin.x, 4, 1e-4f);
    EXPECT_NEAR(ray.origin.y, 3, 1e-4f);
    EXPECT_NEAR(ray.origin.z, 3, 1e-4f);
}

TEST(SceneQueries, ScreenRaysReprojectToRendererPixelsAndReverseZClipPlanes) {
    Scene scene;
    const auto cameraEntity = scene.CreateEntity("camera");
    const Camera camera{1.1f, 0.3f, 2500};
    scene.AddComponent<Camera>(cameraEntity, camera);
    auto& transform = scene.GetTransform(cameraEntity);
    transform.position = {4, 3, -7};
    transform.rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(0.7f, -0.3f, 0.8f);
    transform.scale = {2, 3, 4};
    const auto matrix = scene.GetWorldMatrix(cameraEntity);
    auto forward = Vector3::TransformNormal(Vector3::Forward, matrix);
    forward.Normalize();
    FPSCamera rendered;
    rendered.position = matrix.Translation();
    rendered.yaw = std::atan2(-forward.x, -forward.z);
    rendered.pitch = std::asin(forward.y);
    rendered.fov = camera.fovY;
    rendered.nearPlane = camera.nearPlane;
    rendered.farPlane = camera.farPlane;
    rendered.aspectRatio = 1600.0f / 900.0f;
    const auto viewProjection = rendered.GetViewMatrix() * rendered.GetProjectionMatrix();
    for (const Vector2 pixel : {Vector2{0, 0}, {800, 450}, {1599.5f, 899.5f}, {275, 718}}) {
        Ray ray;
        std::string error;
        ASSERT_TRUE(ScreenPointToRay(scene, pixel, {1600, 900}, cameraEntity, ray, error)) << error;
        const auto start = Vector3::Transform(ray.origin, viewProjection);
        const auto end = Vector3::Transform(ray.origin + ray.direction * ray.maxDistance, viewProjection);
        const float ndcX = pixel.x / 800 - 1;
        const float ndcY = 1 - pixel.y / 450;
        EXPECT_NEAR(start.x, ndcX, 2e-5f);
        EXPECT_NEAR(start.y, ndcY, 2e-5f);
        EXPECT_NEAR(start.z, 1, 2e-5f);
        EXPECT_NEAR(end.x, ndcX, 2e-5f);
        EXPECT_NEAR(end.y, ndcY, 2e-5f);
        EXPECT_NEAR(end.z, 0, 2e-5f);
        EXPECT_NEAR(ray.direction.Length(), 1, 1e-6f);
    }
}
