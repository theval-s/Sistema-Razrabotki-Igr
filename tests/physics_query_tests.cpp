#include <engine/engine.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace {
using namespace engine;
using Vector3 = DirectX::SimpleMath::Vector3;
using Quaternion = DirectX::SimpleMath::Quaternion;

class PhysicsQueryTests : public testing::Test {
protected:
    Engine host{EngineConfig{.taskThreads = 1}};
    Scene& scene = host.GetScene();
    PhysicsSubsystem* physics = nullptr;
    std::string error;

    void SetUp() override {
        PhysicsConfig config;
        config.gravity = Vector3::Zero;
        config.workerThreads = 0;
        physics = host.AddSubsystem<PhysicsSubsystem>(config);
        ASSERT_NE(physics, nullptr);
        ASSERT_TRUE(host.Initialize());
    }
    entt::entity Body(Vector3 position, Collider collider = {}, MotionType motion = MotionType::Static) {
        const auto entity = scene.CreateEntity();
        scene.GetTransform(entity).position = position;
        scene.AddComponent<RigidBody>(entity, RigidBody{.motionType = motion});
        scene.AddComponent<Collider>(entity, collider);
        return entity;
    }
    Ray Forward(Vector3 origin = {0, 0, 0}) const { return {origin, {0, 0, 1}, 20}; }
    std::optional<RayHit> Cast(const Ray& ray, const PhysicsRaycastOptions& options = {}) {
        std::optional<RayHit> hit;
        EXPECT_TRUE(physics->Raycast(ray, options, hit, error)) << error;
        EXPECT_TRUE(error.empty()) << error;
        return hit;
    }
};

TEST_F(PhysicsQueryTests, FiltersBeforeClosestAndAllResultsHaveDeterministicEntityOrder) {
    const auto trigger = Body({0, 0, 2}, Collider{.isTrigger = true});
    const auto parent = scene.CreateEntity();
    const auto child = Body({0, 0, 4});
    ASSERT_TRUE(scene.SetParent(child, parent));
    const auto solid = Body({0, 0, 6});
    const auto tied = Body({0, 0, 6});
    auto hit = Cast(Forward());
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, child); // closest trigger was rejected before selecting
    EXPECT_FALSE(hit->isTrigger);

    PhysicsRaycastOptions options;
    options.includeTriggers = true;
    hit = Cast(Forward(), options);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, trigger);
    EXPECT_TRUE(hit->isTrigger);
    options.filter.ignore = {trigger, parent};
    hit = Cast(Forward(), options);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, child); // no implicit descendant filtering
    options.filter.ignoreDescendants = true;
    hit = Cast(Forward(), options);
    ASSERT_TRUE(hit);
    EXPECT_TRUE(hit->entity == solid || hit->entity == tied);

    std::vector<RayHit> hits;
    ASSERT_TRUE(physics->RaycastAll(Forward(), options, hits, error)) << error;
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].entity, (std::min)(solid, tied));
    EXPECT_EQ(hits[1].entity, (std::max)(solid, tied));
    EXPECT_NEAR(hits[0].distance, 5.5f, 1e-4f);
    options = {};
    ASSERT_TRUE(physics->RaycastAll(Forward(), options, hits, error)) << error;
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits.front().entity, child);
    options.includeTriggers = true;
    ASSERT_TRUE(physics->RaycastAll(Forward(), options, hits, error)) << error;
    ASSERT_EQ(hits.size(), 4u);
    EXPECT_EQ(hits.front().entity, trigger);
}

TEST_F(PhysicsQueryTests, DistancesPointsUnitNormalsAndInsideHitsUseActualSphere) {
    const auto sphere = Body({0, 0, 5}, Collider{.shape = ColliderShape::Sphere, .radius = 1});
    auto ray = Forward();
    ray.direction = {0, 0, 10000};
    auto hit = Cast(ray);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, sphere);
    EXPECT_NEAR(hit->distance, 4, 1e-5f);
    EXPECT_NEAR(hit->point.z, 4, 1e-5f);
    ASSERT_TRUE(hit->normal);
    EXPECT_NEAR(hit->normal->Length(), 1, 1e-5f);
    EXPECT_NEAR(hit->normal->z, -1, 1e-5f);
    hit = Cast(Forward({0, 0, 5}));
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit->distance, 0);
    EXPECT_FALSE(hit->normal);
    EXPECT_EQ(hit->point, Vector3(0, 0, 5));
    // This ray crosses the sphere's AABB, but not its surface.
    EXPECT_FALSE(Cast(Forward({0.9f, 0.9f, 0})));

    std::vector<RayHit> hits;
    ASSERT_TRUE(physics->RaycastAll(Forward(), {}, hits, error)) << error;
    ASSERT_EQ(hits.size(), 1u); // not one entry per surface intersection
    EXPECT_EQ(hits[0].entity, sphere);
    EXPECT_NEAR(hits[0].distance, 4, 1e-5f);
    ray.direction.z = (std::numeric_limits<float>::max)();
    EXPECT_TRUE(Cast(ray)); // normalization cannot overflow
    ray.direction.z = (std::numeric_limits<float>::min)();
    EXPECT_TRUE(Cast(ray)); // normalization cannot underflow
    ray.maxDistance = 3;
    EXPECT_FALSE(Cast(ray));
}

TEST_F(PhysicsQueryTests, RotatedThinBoxRejectsAabbFalsePositiveAndRotatesNormal) {
    const auto box = Body({0, 0, 5}, Collider{.halfExtents = {2, 0.1f, 0.2f}});
    constexpr float angle = 0.78539816339f;
    scene.GetTransform(box).rotation = Quaternion::CreateFromAxisAngle(Vector3::UnitZ, angle);
    EXPECT_FALSE(Cast(Forward({1, -1, 0})));
    auto hit = Cast(Ray{{0, -5, 5}, {0, 1, 0}, 10});
    ASSERT_TRUE(hit);
    ASSERT_TRUE(hit->normal);
    EXPECT_NEAR(hit->normal->x, std::sqrt(0.5f), 1e-4f);
    EXPECT_NEAR(hit->normal->y, -std::sqrt(0.5f), 1e-4f);
    EXPECT_NEAR(hit->normal->z, 0, 1e-4f);
    EXPECT_NEAR(hit->distance, 5 - 0.1f * std::sqrt(2.0f), 1e-4f);
}

TEST_F(PhysicsQueryTests, CapsuleRoundedGeometryAndColliderOffsetAreNotBoundsQueries) {
    const auto capsule = Body({0, 0, 5},
        Collider{.center = {2, 0, 0}, .shape = ColliderShape::Capsule, .radius = 0.5f, .halfHeight = 1});
    EXPECT_FALSE(Cast(Forward()));
    EXPECT_FALSE(Cast(Forward({2.45f, 1.45f, 0})));
    auto hit = Cast(Forward({2, 0, 0}));
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, capsule);
    EXPECT_NEAR(hit->distance, 4.5f, 1e-5f);
}

TEST_F(PhysicsQueryTests, ExactMaxDistanceEndpointIsIncludedByClosestAndAllCollectors) {
    const auto box = Body({0, 0, 4});
    const Ray endpoint{{0, 0, 0}, {0, 0, 1}, 3.5f};
    auto hit = Cast(endpoint);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, box);
    EXPECT_FLOAT_EQ(hit->distance, endpoint.maxDistance);
    EXPECT_EQ(hit->point, Vector3(0, 0, 3.5f));
    std::vector<RayHit> hits;
    ASSERT_TRUE(physics->RaycastAll(endpoint, {}, hits, error)) << error;
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, box);
    EXPECT_FLOAT_EQ(hits[0].distance, endpoint.maxDistance);
    EXPECT_EQ(hits[0].point, hit->point);
    Ray shorter = endpoint;
    shorter.maxDistance = 3.49f;
    EXPECT_FALSE(Cast(shorter));
    ASSERT_TRUE(physics->RaycastAll(shorter, {}, hits, error)) << error;
    EXPECT_TRUE(hits.empty());
}

// Mesh assets are published by the public asset loader only after GPU upload.
// Keep the hidden rendering surface confined to these tests, not the primitive suite.
class PhysicsMeshQueryTests : public PhysicsQueryTests {
protected:
    std::filesystem::path oldRoot = AssetPathResolver::GetGameRoot();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-physics-query-" + NewUuid());
    HWND window = nullptr;

    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        window = CreateWindowExW(0, L"STATIC", L"Physics mesh query tests", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(window, nullptr);
        host.SetSurface({window, 64, 64});
        auto* rendering = host.AddSubsystem<RenderingSubsystem>();
        ASSERT_NE(rendering, nullptr);
        PhysicsConfig config;
        config.gravity = Vector3::Zero;
        config.workerThreads = 0;
        physics = host.AddSubsystem<PhysicsSubsystem>(config);
        ASSERT_NE(physics, nullptr);
        const bool initialized = host.Initialize();
        const auto diagnostics = rendering->GetRendering().GetDiagnostics();
        if (!initialized && diagnostics.failure &&
            diagnostics.failure->operation == "D3D11CreateDeviceAndSwapChain")
            GTEST_SKIP() << diagnostics.failure->summary;
        ASSERT_TRUE(initialized) << (diagnostics.failure ? diagnostics.failure->detail : "");
    }
    void TearDown() override {
        host.Shutdown();
        if (window) DestroyWindow(window);
        AssetPathResolver::SetGameRoot(oldRoot);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TEST_F(PhysicsMeshQueryTests, TriangleBackFacesAreOptInAndAllReturnsOnlyNearestTrianglePerEntity) {
    // Three separated, equally wound triangles. A central ray intersects all three.
    std::ofstream file(root / "layers.obj");
    file << "v -2 -1 0\nv 2 -1 0\nv 0 2 0\n"
            "v -2 -1 3\nv 2 -1 3\nv 0 2 3\n"
            "v -2 -1 6\nv 2 -1 6\nv 0 2 6\n"
            "vt 0 0\nvt 1 0\nvt 0.5 1\n"
            "f 1/1 2/2 3/3\nf 4/1 5/2 6/3\nf 7/1 8/2 9/3\n";
    file.close();
    ASSERT_TRUE(file);
    auto& assets = host.GetSceneAssets();
    const auto mesh = assets.LoadMesh("game/layers.obj");
    ASSERT_TRUE(mesh);
    ASSERT_TRUE(mesh->geometry);
    const auto& geometry = *mesh->geometry;
    ASSERT_EQ(geometry.indices.size(), 9u);
    const auto a = geometry.positions.at(geometry.indices[0]);
    const auto b = geometry.positions.at(geometry.indices[1]);
    const auto c = geometry.positions.at(geometry.indices[2]);
    Vector3 normal = (b - a).Cross(c - a);
    normal.Normalize();
    const Vector3 center = (a + b + c) / 3;
    // Derive front/back from imported winding, accounting for importer handedness.
    float minimum = center.Dot(normal), maximum = minimum;
    for (std::size_t i = 0; i < geometry.indices.size(); i += 3) {
        const auto triangleCenter = (geometry.positions.at(geometry.indices[i]) +
            geometry.positions.at(geometry.indices[i + 1]) +
            geometry.positions.at(geometry.indices[i + 2])) / 3;
        minimum = (std::min)(minimum, triangleCenter.Dot(normal));
        maximum = (std::max)(maximum, triangleCenter.Dot(normal));
    }
    EXPECT_NEAR(maximum - minimum, 6, 1e-5f);
    const auto entity = Body({}, Collider{.shape = ColliderShape::TriangleMesh});
    MeshRenderer renderer;
    renderer.mesh = assets.RequestMesh("game/layers.obj");
    ASSERT_TRUE(assets.IsReady(renderer.mesh));
    ASSERT_NE(scene.AddComponent<MeshRenderer>(entity, renderer), nullptr);

    const Ray front{center + normal * (maximum - center.Dot(normal) + 2), -normal, 10};
    const Ray back{center + normal * (minimum - center.Dot(normal) - 2), normal, 10};
    auto hit = Cast(front);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, entity);
    EXPECT_NEAR(hit->distance, 2, 1e-5f);
    ASSERT_TRUE(hit->normal);
    EXPECT_NEAR(hit->normal->Dot(normal), 1, 1e-5f);
    EXPECT_FALSE(Cast(back));
    std::vector<RayHit> hits;
    ASSERT_TRUE(physics->RaycastAll(back, {}, hits, error)) << error;
    EXPECT_TRUE(hits.empty());
    ASSERT_TRUE(physics->RaycastAll(front, {}, hits, error)) << error;
    ASSERT_EQ(hits.size(), 1u); // three intersected triangles, one entity result
    EXPECT_EQ(hits[0].entity, entity);
    EXPECT_NEAR(hits[0].distance, 2, 1e-5f);

    PhysicsRaycastOptions twoSided;
    twoSided.hitBackFaces = true;
    hit = Cast(back, twoSided);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, entity);
    EXPECT_NEAR(hit->distance, 2, 1e-5f);
    ASSERT_TRUE(hit->normal);
    EXPECT_NEAR(hit->normal->Length(), 1, 1e-5f);
    ASSERT_TRUE(physics->RaycastAll(back, twoSided, hits, error)) << error;
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].entity, entity);
    EXPECT_NEAR(hits[0].distance, 2, 1e-5f);
    ASSERT_TRUE(physics->RaycastAll(front, twoSided, hits, error)) << error;
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_NEAR(hits[0].distance, 2, 1e-5f);

    // Triangle casts use a different narrowphase path from the primitive endpoint test.
    Ray endpoint = front;
    endpoint.maxDistance = 2;
    hit = Cast(endpoint);
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(hit->distance, 2);
    ASSERT_TRUE(physics->RaycastAll(endpoint, {}, hits, error)) << error;
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FLOAT_EQ(hits[0].distance, 2);
}

TEST_F(PhysicsQueryTests, InitialPreparationAndExplicitSyncDoNotAdvanceSimulation) {
    const auto first = Body({0, 0, 4});
    const auto dynamic = Body({5, 0, 0}, {}, MotionType::Dynamic);
    EXPECT_EQ(physics->GetBodyCount(), 0u);
    auto hit = Cast(Forward());
    ASSERT_TRUE(hit);
    EXPECT_EQ(physics->GetBodyCount(), 2u);
    EXPECT_NEAR(hit->distance, 3.5f, 1e-4f);
    EXPECT_EQ(scene.GetTransform(dynamic).position, Vector3(5, 0, 0));
    scene.GetTransform(first).position.z = 6;
    const auto added = Body({0, 0, 2});
    hit = Cast(Forward());
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, first);
    EXPECT_NEAR(hit->distance, 3.5f, 1e-4f);
    EXPECT_EQ(physics->GetBodyCount(), 2u); // no full scene reconciliation per cast
    ASSERT_TRUE(physics->SyncQueries(error)) << error;
    EXPECT_EQ(physics->GetBodyCount(), 3u);
    std::vector<RayHit> hits;
    ASSERT_TRUE(physics->RaycastAll(Forward(), {}, hits, error)) << error;
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].entity, added);
    EXPECT_NEAR(hits[0].distance, 1.5f, 1e-4f);
    EXPECT_EQ(hits[1].entity, first);
    EXPECT_NEAR(hits[1].distance, 5.5f, 1e-4f);
}

TEST_F(PhysicsQueryTests, QuerySyncDoesNotTeleportKinematicTargetsOrDynamicAuthoredPoses) {
    const auto kinematic = Body({0, 0, 5}, {}, MotionType::Kinematic);
    const auto dynamic = Body({5, 0, 5}, {}, MotionType::Dynamic);
    ASSERT_TRUE(Cast(Forward()));
    ASSERT_TRUE(physics->MoveKinematic(kinematic, {10, 0, 5}, Quaternion::Identity, error)) << error;
    scene.GetTransform(dynamic).position = {15, 0, 5};
    ASSERT_TRUE(physics->SyncQueries(error)) << error;
    auto hit = Cast(Forward());
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, kinematic);
    EXPECT_FALSE(Cast(Forward({10, 0, 0})));
    hit = Cast(Forward({5, 0, 0}));
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, dynamic);
    EXPECT_FALSE(Cast(Forward({15, 0, 0})));
    // Shape rebuilds during query sync must preserve the current physical pose too.
    scene.TryGetComponent<Collider>(dynamic)->halfExtents = {1, 1, 1};
    scene.TryGetComponent<Collider>(kinematic)->halfExtents = {1, 1, 1};
    ASSERT_TRUE(physics->SyncQueries(error)) << error;
    EXPECT_TRUE(Cast(Forward()));
    EXPECT_TRUE(Cast(Forward({5, 0, 0})));
    EXPECT_FALSE(Cast(Forward({10, 0, 0})));
    EXPECT_FALSE(Cast(Forward({15, 0, 0})));
    physics->FixedUpdate(FixedUpdateContext{});
    EXPECT_FALSE(Cast(Forward()));
    hit = Cast(Forward({10, 0, 0}));
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, kinematic);
    EXPECT_EQ(scene.GetTransform(dynamic).position, Vector3(5, 0, 5));
}

TEST_F(PhysicsQueryTests, FixedStepEstablishesSnapshotAndDeletionAndResetCannotReturnStaleHandles) {
    const auto entity = Body({0, 0, 4});
    physics->FixedUpdate(FixedUpdateContext{});
    scene.GetTransform(entity).position.x = 10;
    auto hit = Cast(Forward());
    ASSERT_TRUE(hit); // first cast reads fixed-step snapshot, not authored move
    EXPECT_EQ(hit->entity, entity);
    scene.DestroyEntity(entity);
    EXPECT_FALSE(Cast(Forward()));
    const auto replacement = Body({0, 0, 8});
    EXPECT_FALSE(Cast(Forward()));
    physics->ResetSimulation();
    EXPECT_EQ(physics->GetBodyCount(), 0u);
    hit = Cast(Forward());
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entity, replacement);
    EXPECT_NEAR(hit->distance, 7.5f, 1e-4f);
    scene.RemoveComponent<Collider>(replacement);
    EXPECT_FALSE(Cast(Forward()));
}

TEST_F(PhysicsQueryTests, ValidationClearsOutputsAndCoincidentLineStillChecksOptionsAndSubsystem) {
    Body({0, 0, 4});
    std::optional<RayHit> hit = RayHit{};
    auto ray = Forward();
    ray.direction = Vector3::Zero;
    EXPECT_FALSE(physics->Raycast(ray, {}, hit, error));
    EXPECT_FALSE(hit);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(physics->GetBodyCount(), 0u); // invalid queries do not prepare bodies
    for (const float distance : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()}) {
        ray = Forward();
        ray.maxDistance = distance;
        std::vector<RayHit> hits{RayHit{}};
        EXPECT_FALSE(physics->RaycastAll(ray, {}, hits, error));
        EXPECT_TRUE(hits.empty());
        EXPECT_FALSE(error.empty());
    }
    ray = Forward();
    ray.origin.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(physics->Raycast(ray, {}, hit, error));
    ray = Forward();
    ray.direction.y = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(physics->Raycast(ray, {}, hit, error));
    PhysicsRaycastOptions options;
    options.filter.ignore = {entt::null};
    EXPECT_FALSE(physics->Raycast(Forward(), options, hit, error));
    EXPECT_FALSE(physics->Linecast({}, {}, options, hit, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(physics->Linecast({}, {}, {}, hit, error)) << error;
    EXPECT_FALSE(hit);
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(physics->Linecast({}, {0, 0, 10}, {}, hit, error)) << error;
    ASSERT_TRUE(hit);
    EXPECT_NEAR(hit->distance, 3.5f, 1e-4f);
    EXPECT_FALSE(physics->Linecast({}, {0, 0, std::numeric_limits<float>::infinity()}, {}, hit, error));
    physics->Shutdown();
    EXPECT_FALSE(physics->Linecast({}, {}, {}, hit, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(physics->SyncQueries(error));
    EXPECT_FALSE(physics->Raycast(Forward(), {}, hit, error));
    std::vector<RayHit> hits{RayHit{}};
    EXPECT_FALSE(physics->RaycastAll(Forward(), {}, hits, error));
    EXPECT_TRUE(hits.empty());
}
} // namespace
