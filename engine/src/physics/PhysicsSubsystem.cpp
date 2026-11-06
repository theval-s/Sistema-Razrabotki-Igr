#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>

#include <engine/physics/PhysicsSubsystem.hpp>
#include <engine/physics/PhysicsEvents.hpp>
#include <engine/engine.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/core/FrameContext.hpp>
#include <engine/rendering/DebugDraw.hpp>
#include "PhysicsValidation.hpp"
#include <engine/core/Log.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <optional>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <tuple>

namespace engine {
namespace {
using Vector3 = DirectX::SimpleMath::Vector3;
using Quaternion = DirectX::SimpleMath::Quaternion;
JPH::Vec3 Vec(Vector3 v) { return {v.x, v.y, v.z}; }
JPH::RVec3 Position(Vector3 v) { return {v.x, v.y, v.z}; }
JPH::Quat Quat(Quaternion q) { return {q.x, q.y, q.z, q.w}; }
void Trace(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log::Physics().info("Jolt: {}", text);
}
#ifdef JPH_ENABLE_ASSERTS
bool Assert(const char* expression, const char* message, const char* file, JPH::uint line) {
    log::Physics().error("Jolt assertion {}:{}: {} ({})", file, line, expression, message ? message : "");
    return true;
}
#endif
// Jolt registration is process-wide for this linked library, not per scene/world.
std::mutex runtimeMutex;
unsigned runtimeUsers = 0;
struct RuntimeLease {
    RuntimeLease() {
        std::lock_guard lock(runtimeMutex);
        if (runtimeUsers == 0) {
            if (JPH::Factory::sInstance) throw std::runtime_error("Jolt Factory is already owned externally");
            JPH::RegisterDefaultAllocator();
            JPH::Trace = Trace;
            JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = Assert;)
            JPH::Factory::sInstance = new JPH::Factory;
            JPH::RegisterTypes();
        }
        ++runtimeUsers;
    }
    ~RuntimeLease() {
        std::lock_guard lock(runtimeMutex);
        if (--runtimeUsers == 0) {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }
};
constexpr JPH::ObjectLayer StaticLayer = 0, MovingLayer = 1, TriggerLayer = 2;
constexpr std::string_view WaitingForMesh = "waiting for mesh collision geometry";
JPH::EMotionType Motion(MotionType type) {
    switch (type) {
    case MotionType::Dynamic: return JPH::EMotionType::Dynamic;
    case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
    default: return JPH::EMotionType::Static;
    }
}
JPH::ShapeSettings::ShapeResult CreateShape(const Collider& collider, Vector3 scale, const MeshGeometry* geometry) {
    using Result = JPH::ShapeSettings::ShapeResult;
    const auto fail = [](const char* message) { Result result; result.SetError(message); return result; };
    const Vector3 offset = collider.center * scale;
    if (!detail::Finite(offset)) return fail("scaled collider center is not finite");
    Result base;
    switch (collider.shape) {
    case ColliderShape::Cube:
    case ColliderShape::Box: {
        const auto half = collider.shape == ColliderShape::Cube ? Vector3(collider.halfExtents.x) : collider.halfExtents;
        const Vector3 extent = half * scale;
        if (!detail::Finite(extent)) return fail("scaled collider extents are not finite");
        const float radius = (std::min)(0.05f, (std::min)({extent.x, extent.y, extent.z}) * 0.1f);
        base = JPH::BoxShapeSettings(Vec(extent), radius).Create();
        break;
    }
    case ColliderShape::Sphere: {
        const float radius = collider.radius * (std::max)({scale.x, scale.y, scale.z});
        if (!std::isfinite(radius)) return fail("scaled sphere radius is not finite");
        base = JPH::SphereShapeSettings(radius).Create();
        break;
    }
    case ColliderShape::Capsule: {
        const float radius = collider.radius * (std::max)(scale.x, scale.z);
        const float height = collider.halfHeight * scale.y;
        if (!std::isfinite(radius) || !std::isfinite(height)) return fail("scaled capsule dimensions are not finite");
        base = JPH::CapsuleShapeSettings(height, radius).Create();
        break;
    }
    case ColliderShape::ConvexMesh:
    case ColliderShape::TriangleMesh: {
        if (!geometry || geometry->positions.empty()) return fail("mesh has no CPU collision geometry");
        JPH::Array<JPH::Vec3> points;
        points.reserve(geometry->positions.size());
        for (const auto position : geometry->positions) {
            const Vector3 scaled = position * scale;
            if (!detail::Finite(scaled)) return fail("scaled mesh position is not finite");
            points.push_back(Vec(scaled));
        }
        if (collider.shape == ColliderShape::ConvexMesh) {
            base = JPH::ConvexHullShapeSettings(points).Create();
        } else {
            if (geometry->indices.empty() || geometry->indices.size() % 3 != 0) return fail("mesh requires triangle indices");
            JPH::VertexList vertices;
            vertices.reserve(points.size());
            for (const auto point : points) { JPH::Float3 vertex; point.StoreFloat3(&vertex); vertices.push_back(vertex); }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(geometry->indices.size() / 3);
            for (std::size_t i = 0; i < geometry->indices.size(); i += 3) {
                const auto a = geometry->indices[i], b = geometry->indices[i + 1], c = geometry->indices[i + 2];
                if (a >= points.size() || b >= points.size() || c >= points.size()) return fail("mesh index is out of range");
                triangles.emplace_back(a, b, c);
            }
            base = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
        }
        break;
    }
    default: return fail("invalid collider shape");
    }
    if (base.HasError()) return base;
    return JPH::RotatedTranslatedShapeSettings(Vec(offset), JPH::Quat::sIdentity(), base.Get()).Create();
}
struct ContactListener final : JPH::ContactListener {
    struct Contact {
        PhysicsContactEvent event;
        bool removed = false;
    };
    std::mutex mutex;
    std::unordered_map<JPH::SubShapeIDPair, Contact> contacts;
    std::vector<PhysicsContactEvent> previous, current;

    void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        const auto p = manifold.GetWorldSpaceContactPointOn1(0);
        const auto n = manifold.mWorldSpaceNormal;
        const auto a = static_cast<entt::entity>(first.GetUserData());
        const auto b = static_cast<entt::entity>(second.GetUserData());
        const auto kind = first.IsSensor() || second.IsSensor()
            ? PhysicsContactKind::TriggerEnter : PhysicsContactKind::CollisionEnter;
        PhysicsContactEvent event{kind, a, b,
            {static_cast<float>(p.GetX()), static_cast<float>(p.GetY()), static_cast<float>(p.GetZ())},
            {n.GetX(), n.GetY(), n.GetZ()}, 0};
        if (event.second < event.first) {
            std::swap(event.first, event.second);
            event.normal = -event.normal;
        }
        const JPH::SubShapeIDPair key(first.GetID(), manifold.mSubShapeID1, second.GetID(), manifold.mSubShapeID2);
        std::lock_guard lock(mutex);
        contacts.insert_or_assign(key, Contact{event});
    }

    void OnContactPersisted(const JPH::Body& first, const JPH::Body& second,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override {
        OnContactAdded(first, second, manifold, settings);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& key) override {
        std::lock_guard lock(mutex);
        if (const auto it = contacts.find(key); it != contacts.end()) it->second.removed = true;
    }

    // Called on the main thread outside PhysicsSystem::Update.
    void Invalidate(JPH::BodyID id) {
        std::erase_if(contacts, [id](const auto& item) {
            return item.first.GetBody1ID() == id || item.first.GetBody2ID() == id;
        });
    }

    void Publish(const JPH::BodyInterface& bodies, entt::dispatcher& events, std::uint64_t step) {
        std::erase_if(contacts, [&](const auto& item) {
            if (!item.second.removed) return false;
            const auto a = item.first.GetBody1ID(), b = item.first.GetBody2ID();
            // Jolt discards contacts when bodies sleep. Keep the logical pair until
            // either body moves, wakes without a renewed contact, or is removed.
            return !bodies.IsAdded(a) || !bodies.IsAdded(b) || bodies.IsActive(a) || bodies.IsActive(b);
        });
        const auto less = [](const auto& a, const auto& b) {
            return std::tie(a.first, a.second, a.kind) < std::tie(b.first, b.second, b.kind);
        };
        current.clear();
        for (const auto& [key, contact] : contacts) current.push_back(contact.event);
        std::sort(current.begin(), current.end(), less);
        current.erase(std::unique(current.begin(), current.end(), [&](const auto& a, const auto& b) {
            return !less(a, b) && !less(b, a);
        }), current.end());
        for (auto event : previous) {
            if (std::binary_search(current.begin(), current.end(), event, less)) continue;
            event.kind = event.kind == PhysicsContactKind::TriggerEnter
                ? PhysicsContactKind::TriggerExit : PhysicsContactKind::CollisionExit;
            event.stepIndex = step;
            events.enqueue(event);
        }
        for (auto event : current) {
            if (std::binary_search(previous.begin(), previous.end(), event, less)) {
                event.kind = event.kind == PhysicsContactKind::TriggerEnter
                    ? PhysicsContactKind::TriggerStay : PhysicsContactKind::CollisionStay;
            }
            event.stepIndex = step;
            events.enqueue(event);
        }
        previous.swap(current);
    }
};

bool ValidPose(Vector3 position, Quaternion rotation, std::string& error) {
    if (!detail::Finite(position) || !std::isfinite(rotation.LengthSquared()) || rotation.LengthSquared() < 1e-8f) {
        error = "position/rotation must be finite and rotation must be nonzero";
        return false;
    }
    return true;
}
}

struct PhysicsSubsystem::Impl {
    RuntimeLease runtime; // first constructed / last destroyed, before Jolt allocations
    JPH::BroadPhaseLayerInterfaceTable broadPhase{3, 3};
    JPH::ObjectLayerPairFilterTable pairs{3};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> broadPhaseFilter;
    JPH::TempAllocatorImpl allocator;
    JPH::JobSystemThreadPool jobs;
    ContactListener listener;
    JPH::PhysicsSystem world;
    struct Body {
        JPH::BodyID id;
        RigidBody settings;
        Collider collider;
        Vector3 scale;
        Vector3 position;
        Quaternion rotation;
        std::shared_ptr<const MeshGeometry> geometry;
        bool awake = true;
    };
    struct ShapeInput {
        Collider collider;
        Vector3 scale;
        std::shared_ptr<const MeshGeometry> geometry;
        bool operator==(const ShapeInput&) const = default;
    };
    std::optional<ShapeInput> previewInput;
    std::vector<Vector3> previewLines;
    std::string previewError;
    void ClearPreview() { previewInput.reset(); previewLines.clear(); previewError.clear(); }
    std::unordered_map<entt::entity, ShapeInput> failedShapes;
    std::unordered_map<entt::entity, Body> bodies;
    std::unordered_map<entt::entity, std::string> failures;
    std::vector<JPH::BodyID> added;
    Scene* scene = nullptr;
    AssetSystem* assets = nullptr;
    std::size_t observer = 0;

    explicit Impl(const PhysicsConfig& config)
        : allocator(config.tempAllocatorBytes), jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                     static_cast<int>(config.workerThreads)) {
        broadPhase.MapObjectToBroadPhaseLayer(StaticLayer, JPH::BroadPhaseLayer(0));
        broadPhase.MapObjectToBroadPhaseLayer(MovingLayer, JPH::BroadPhaseLayer(1));
        broadPhase.MapObjectToBroadPhaseLayer(TriggerLayer, JPH::BroadPhaseLayer(2));
        pairs.EnableCollision(TriggerLayer, MovingLayer);
        pairs.EnableCollision(StaticLayer, MovingLayer);
        pairs.EnableCollision(MovingLayer, MovingLayer);
        broadPhaseFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broadPhase, 3, pairs, 3);
        world.Init(config.maxBodies, 0, config.maxBodyPairs, config.maxContactConstraints,
                   broadPhase, *broadPhaseFilter, pairs);
        world.SetGravity(Vec(config.gravity));
        world.SetContactListener(&listener);
    }
    void Remove(entt::entity entity) {
        if (const auto it = bodies.find(entity); it != bodies.end()) {
            listener.Invalidate(it->second.id);
            world.GetBodyInterface().RemoveBody(it->second.id);
            world.GetBodyInterface().DestroyBody(it->second.id);
            bodies.erase(it);
        }
        failures.erase(entity);
        failedShapes.erase(entity);
    }
    void Clear() {
        while (!bodies.empty()) Remove(bodies.begin()->first);
        failures.clear();
        failedShapes.clear();
        listener.contacts.clear();
        listener.previous.clear();
        listener.current.clear();
        ClearPreview();
    }
    void Reject(entt::entity entity, const std::string& error) {
        if (failures[entity] != error) {
            log::Physics().error("Physics '{}': {}", scene->GetName(entity), error);
            failures[entity] = error;
        }
    }
    bool ReadShapeInput(entt::entity entity, const RigidBody& settings, const Collider& collider,
                        detail::PhysicsPose& pose, ShapeInput& input, std::string& error) {
        const auto scale = scene->GetTransform(entity).scale;
        if (!detail::Finite(scale) || scale.x <= 0 || scale.y <= 0 || scale.z <= 0) {
            error = "physics entities require positive local scale";
            return false;
        }
        if (!detail::ValidatePhysics(settings, collider, error) ||
            !detail::GetPhysicsPose(scene->GetWorldMatrix(entity), pose, error)) return false;
        if (settings.motionType == MotionType::Dynamic && scene->GetParent(entity) != entt::null) {
            error = "dynamic bodies must be root entities";
            return false;
        }
        // Root authored scale avoids decomposition jitter as a dynamic body rotates.
        if (scene->GetParent(entity) == entt::null) pose.scale = scale;
        input = {collider, pose.scale, {}};
        // Display preferences must never invalidate a simulation body or shape cache.
        input.collider.showDebug = true;
        if (collider.shape == ColliderShape::ConvexMesh || collider.shape == ColliderShape::TriangleMesh) {
            const auto* renderer = scene->TryGetComponent<MeshRenderer>(entity);
            if (!renderer) { error = "mesh collider requires MeshRenderer"; return false; }
            const auto mesh = assets->Peek(renderer->mesh);
            if (!mesh || !mesh->geometry) { error = WaitingForMesh; return false; }
            input.geometry = mesh->geometry;
        }
        error.clear();
        return true;
    }
    void Synchronize(float dt) {
        added.clear();
        // The scene lifecycle observer removes bodies when entities or components are removed.
        // TODO: Use change notifications to avoid polling authored body settings.
        auto& api = world.GetBodyInterface();
        scene->ForEach<RigidBody>([&](entt::entity entity, const RigidBody& settings) {
            const auto* collider = scene->TryGetComponent<Collider>(entity);
            if (!collider) { Remove(entity); return; }
            std::string error;
            detail::PhysicsPose pose;
            ShapeInput input;
            if (!ReadShapeInput(entity, settings, *collider, pose, input, error)) {
                if (bodies.contains(entity)) Remove(entity);
                if (error == WaitingForMesh) failures[entity] = error;
                else Reject(entity, error);
                return;
            }
            const auto& geometry = input.geometry;
            auto it = bodies.find(entity);
            if (it != bodies.end() && (it->second.settings != settings || it->second.collider != input.collider || it->second.scale != pose.scale || it->second.geometry != geometry)) {
                Remove(entity); // authoring changes rebuild the shape/body and reset velocity
                it = bodies.end();
            }
            if (it == bodies.end()) {
                if (const auto failed = failedShapes.find(entity); failed != failedShapes.end() && failed->second == input) return;
                const auto shape = CreateShape(*collider, pose.scale, geometry.get());
                if (shape.HasError()) {
                    failedShapes.insert_or_assign(entity, input); // do not re-cook invalid geometry every step
                    Reject(entity, shape.GetError().c_str());
                    return;
                }
                failedShapes.erase(entity);
                const auto layer = collider->isTrigger ? TriggerLayer
                    : settings.motionType == MotionType::Static ? StaticLayer : MovingLayer;
                JPH::BodyCreationSettings creation(shape.Get(), Position(pose.position), Quat(pose.rotation),
                    Motion(settings.motionType), layer);
                creation.mIsSensor = collider->isTrigger;
                creation.mFriction = settings.friction;
                creation.mRestitution = settings.restitution;
                creation.mUserData = static_cast<JPH::uint64>(entt::to_integral(entity));
                if (settings.motionType == MotionType::Dynamic) {
                    creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                    creation.mMassPropertiesOverride.mMass = settings.mass;
                }
                auto* body = api.CreateBody(creation);
                if (!body) { Reject(entity, "body capacity exceeded"); return; }
                const auto id = body->GetID();
                bodies.emplace(entity, Body{id, settings, input.collider, pose.scale, pose.position, pose.rotation, geometry});
                added.push_back(id);
                failures.erase(entity);
            } else if (dt > 0 && settings.motionType != MotionType::Dynamic) {
                if (settings.motionType == MotionType::Kinematic && dt > 0)
                    api.MoveKinematic(it->second.id, Position(pose.position), Quat(pose.rotation), dt);
                else if (it->second.position != pose.position || it->second.rotation != pose.rotation) {
                    listener.Invalidate(it->second.id);
                    api.SetPositionAndRotation(it->second.id, Position(pose.position), Quat(pose.rotation), JPH::EActivation::Activate);
                }
                it->second.position = pose.position;
                it->second.rotation = pose.rotation;
            }
        });
        if (!added.empty()) {
            const int count = static_cast<int>(added.size());
            const auto state = api.AddBodiesPrepare(added.data(), count);
            api.AddBodiesFinalize(added.data(), count, state, JPH::EActivation::Activate);
        }
    }
    Body* Find(entt::entity entity, std::string& error) {
        Synchronize(0);
        const auto it = bodies.find(entity);
        if (it == bodies.end()) {
            error = failures.contains(entity) ? failures.at(entity) : "entity has no RigidBody/Collider";
            return nullptr;
        }
        error.clear();
        return &it->second;
    }
};

PhysicsSubsystem::PhysicsSubsystem(PhysicsConfig config) : config_(config) {}
PhysicsSubsystem::~PhysicsSubsystem() { Shutdown(); }
bool PhysicsSubsystem::Initialize() {
    if (impl_) return true;
    if (!detail::Finite(config_.gravity) || config_.maxBodies == 0 || config_.maxBodies > 65536 ||
        config_.maxBodyPairs == 0 || config_.maxContactConstraints == 0 || config_.tempAllocatorBytes == 0 || config_.workerThreads > 64) {
        log::Physics().error("Invalid physics configuration");
        return false;
    }
    try {
        impl_ = std::make_unique<Impl>(config_);
        impl_->scene = &GetEngine().GetScene();
        impl_->assets = &GetEngine().GetSceneAssets();
        impl_->observer = impl_->scene->AddLifecycleObserver([this](Scene::LifecycleEvent event, entt::entity entity, entt::id_type type) {
            if (event == Scene::LifecycleEvent::EntityDestroying || type == entt::type_hash<RigidBody>::value() ||
                type == entt::type_hash<Collider>::value()) impl_->Remove(entity);
        });
        return true;
    } catch (const std::exception& e) {
        log::Physics().error("Physics initialization: {}", e.what());
        impl_.reset();
        return false;
    }
}
void PhysicsSubsystem::Shutdown() {
    if (!impl_) return;
    impl_->scene->RemoveLifecycleObserver(impl_->observer);
    impl_->Clear();
    impl_.reset();
}
void PhysicsSubsystem::ResetSimulation() {
    if (impl_) impl_->Clear();
    GetEngine().GetEvents().clear<PhysicsContactEvent>();
    GetEngine().GetEvents().clear<PhysicsActivityEvent>();
}
void PhysicsSubsystem::FixedUpdate(const FixedUpdateContext& step) {
    if (!impl_) return;
    const float dt = static_cast<float>(step.deltaSeconds);
    if (!std::isfinite(dt) || dt <= 0) return;
    impl_->Synchronize(dt);
    const auto result = impl_->world.Update(dt, 1, &impl_->allocator, &impl_->jobs);
    if (result != JPH::EPhysicsUpdateError::None) log::Physics().error("Jolt capacity error: {}", static_cast<unsigned>(result));
    auto& events = GetEngine().GetEvents();
    for (auto& [entity, body] : impl_->bodies) {
        if (body.settings.motionType != MotionType::Dynamic) continue;
        const JPH::BodyLockRead lock(impl_->world.GetBodyLockInterface(), body.id);
        if (!lock.Succeeded()) continue;
        const auto& simulated = lock.GetBody();
        const auto position = simulated.GetPosition();
        const auto rotation = simulated.GetRotation();
        auto& transform = impl_->scene->GetTransform(entity);
        transform.position = {static_cast<float>(position.GetX()), static_cast<float>(position.GetY()), static_cast<float>(position.GetZ())};
        transform.rotation = {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()};
        if (!body.collider.isTrigger && simulated.IsActive() != body.awake) {
            body.awake = simulated.IsActive();
            events.enqueue(PhysicsActivityEvent{entity, body.awake, step.stepIndex});
        }
    }
    impl_->listener.Publish(impl_->world.GetBodyInterface(), events, step.stepIndex);
}
bool PhysicsSubsystem::SetLinearVelocity(entt::entity entity, Vector3 velocity, std::string& error) {
    if (!impl_ || !detail::Finite(velocity)) { error = "physics is not initialized or velocity is not finite"; return false; }
    auto* body = impl_->Find(entity, error);
    if (!body) return false;
    if (body->settings.motionType != MotionType::Dynamic) { error = "SetLinearVelocity requires a dynamic body; use MoveKinematic for kinematic bodies"; return false; }
    impl_->world.GetBodyInterface().SetLinearVelocity(body->id, Vec(velocity));
    error.clear(); return true;
}
bool PhysicsSubsystem::AddImpulse(entt::entity entity, Vector3 impulse, std::string& error) {
    if (!impl_ || !detail::Finite(impulse)) { error = "physics is not initialized or impulse is not finite"; return false; }
    auto* body = impl_->Find(entity, error);
    if (!body) return false;
    if (body->settings.motionType != MotionType::Dynamic) { error = "impulses require a dynamic body"; return false; }
    impl_->world.GetBodyInterface().AddImpulse(body->id, Vec(impulse));
    error.clear(); return true;
}
bool PhysicsSubsystem::MoveKinematic(entt::entity entity, Vector3 position, Quaternion rotation, std::string& error) {
    if (!impl_ || !ValidPose(position, rotation, error)) { if (!impl_) error = "physics is not initialized"; return false; }
    auto* body = impl_->Find(entity, error);
    if (!body) return false;
    if (body->settings.motionType != MotionType::Kinematic) { error = "MoveKinematic requires a kinematic body"; return false; }
    // Position/rotation here are local to the entity's parent, as in Transform.
    rotation.Normalize();
    auto transform = impl_->scene->GetTransform(entity);
    transform.position = position;
    transform.rotation = rotation;
    const auto parent = impl_->scene->GetParent(entity);
    const auto matrix = parent == entt::null ? transform.ToMatrix() : transform.ToMatrix() * impl_->scene->GetWorldMatrix(parent);
    detail::PhysicsPose pose;
    if (!detail::GetPhysicsPose(matrix, pose, error)) return false;
    impl_->scene->GetTransform(entity) = transform;
    error.clear(); return true;
}
bool PhysicsSubsystem::Teleport(entt::entity entity, Vector3 position, Quaternion rotation, std::string& error) {
    if (!impl_ || !ValidPose(position, rotation, error)) { if (!impl_) error = "physics is not initialized"; return false; }
    auto* body = impl_->Find(entity, error);
    if (!body) return false;
    rotation.Normalize();
    auto transform = impl_->scene->GetTransform(entity);
    transform.position = position;
    transform.rotation = rotation;
    const auto parent = impl_->scene->GetParent(entity);
    const auto matrix = parent == entt::null ? transform.ToMatrix() : transform.ToMatrix() * impl_->scene->GetWorldMatrix(parent);
    detail::PhysicsPose pose;
    if (!detail::GetPhysicsPose(matrix, pose, error)) return false;
    impl_->scene->GetTransform(entity) = transform;
    impl_->listener.Invalidate(body->id);
    impl_->world.GetBodyInterface().SetPositionAndRotation(body->id, Position(pose.position), Quat(pose.rotation), JPH::EActivation::Activate);
    body->position = pose.position;
    body->rotation = pose.rotation;
    error.clear(); return true;
}
bool PhysicsSubsystem::DrawCollider(entt::entity entity, DebugDraw& draw, const DirectX::SimpleMath::Color& color,
                                    std::string& error) {
    error.clear();
    if (!impl_) { error = "physics is not initialized"; return false; }
    if (entity == entt::null) { impl_->ClearPreview(); return true; }
    const auto* collider = impl_->scene->TryGetComponent<Collider>(entity);
    if (collider && !collider->showDebug) { impl_->ClearPreview(); return true; }
    const auto* settings = impl_->scene->TryGetComponent<RigidBody>(entity);
    detail::PhysicsPose pose;
    Impl::ShapeInput input;
    if (!collider || !settings || !impl_->ReadShapeInput(entity, *settings, *collider, pose, input, error)) {
        impl_->ClearPreview();
        if (error.empty()) error = "entity has no RigidBody/Collider";
        return false;
    }
    if (!impl_->previewInput || *impl_->previewInput != input) {
        impl_->ClearPreview();
        impl_->previewInput = input;
        const auto result = CreateShape(input.collider, input.scale, input.geometry.get());
        if (result.HasError()) impl_->previewError = result.GetError().c_str();
        else {
            const auto* shape = result.Get().GetPtr();
            // The offset wrapper is not a leaf. Let Jolt collect transformed
            // leaves before requesting triangles, preserving the local mesh origin.
            JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
            const JPH::TransformedShape transformed(JPH::RVec3(shape->GetCenterOfMass()),
                JPH::Quat::sIdentity(), shape, JPH::BodyID{});
            transformed.CollectTransformedShapes(JPH::AABox::sBiggest(), leaves);
            constexpr int batch = 128;
            std::array<JPH::Float3, batch * 3> vertices;
            for (const auto& leaf : leaves.mHits) {
                JPH::Shape::GetTrianglesContext context;
                leaf.GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
                for (int count; (count = leaf.GetTrianglesNext(context, batch, vertices.data())) != 0;) {
                    for (int i = 0; i < count * 3; i += 3) {
                        for (int edge = 0; edge < 3; ++edge) {
                            const auto& a = vertices[i + edge];
                            const auto& b = vertices[i + (edge + 1) % 3];
                            impl_->previewLines.emplace_back(a.x, a.y, a.z);
                            impl_->previewLines.emplace_back(b.x, b.y, b.z);
                        }
                    }
                }
            }
        }
    }
    error = impl_->previewError;
    if (!error.empty()) return false;
    for (std::size_t i = 0; i + 1 < impl_->previewLines.size(); i += 2)
        draw.Line(Vector3::Transform(impl_->previewLines[i], pose.rotation) + pose.position,
                  Vector3::Transform(impl_->previewLines[i + 1], pose.rotation) + pose.position, color);
    return true;
}
std::size_t PhysicsSubsystem::GetBodyCount() const { return impl_ ? impl_->bodies.size() : 0; }
} // namespace engine
