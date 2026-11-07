
#include <engine/scripting/LuaEntityHandle.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <engine/scene/Scene.hpp>

namespace engine {

    namespace {
        Transform DecomposeChecked(const DirectX::SimpleMath::Matrix& matrix);
        DirectX::SimpleMath::Matrix ParentInverse(Scene& scene, entt::entity parent);

        void ThrowIfInvalid(const LuaEntityHandle& handle) {
            if (!handle.IsValid()) {
                throw std::runtime_error("Invalid Entity handle");
            }
        }

        bool IsFinite(const DirectX::SimpleMath::Vector3& value) {
            return std::isfinite(value.x) &&
                std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        bool IsFinite(const DirectX::SimpleMath::Quaternion& value) {
            return std::isfinite(value.x) &&
                std::isfinite(value.y) &&
                std::isfinite(value.z) &&
                std::isfinite(value.w);
        }

    } // namespace

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    LuaEntityHandle LuaEntityHandle::FromScene(
        Scene& scene,
        entt::entity entity
    ) {
        LuaEntityHandle handle;

        if (!scene.IsValid(entity))
            return handle;

        handle.scene_ = &scene;
        handle.entity_ = entity;
        handle.id_ = scene.GetId(entity);
        handle.sceneLifetime_ = scene.GetLifetimeToken();

        return handle;
    }

    LuaEntityHandle LuaEntityHandle::FromScriptOwner(
        Scene& scene,
        entt::entity entity,
        std::weak_ptr<void> scriptLifetime
    ) {
        auto handle = FromScene(scene, entity);

        if (handle.IsValid()) {
            handle.scriptLifetime_ = std::move(scriptLifetime);
            handle.hasScriptLifetime_ = true;
        }

        return handle;
    }

    // -------------------------------------------------------------------------
    // Lifetime and validity
    // -------------------------------------------------------------------------

    bool LuaEntityHandle::IsValid() const {

        // The Scene no longer exists.
        if (!scene_ || sceneLifetime_.expired())
            return false;

        // The script owning this handle was destroyed.
        if (hasScriptLifetime_ && scriptLifetime_.expired())
            return false;

        // Check EnTT entity existence and generation.
        if (!scene_->IsValid(entity_))
            return false;

        // Check persistent entity identity.
        return scene_->GetId(entity_) == id_;
    }

    entt::entity LuaEntityHandle::Resolve() const {
        if (!IsValid())
            return entt::null;

        return entity_;
    }

    Scene* LuaEntityHandle::GetScene() const {
        return IsValid() ? scene_ : nullptr;
    }

    bool LuaEntityHandle::BelongsTo(const Scene& scene) const {
        return IsValid() && scene_ == &scene;
    }

    // -------------------------------------------------------------------------
    // Identity and name
    // -------------------------------------------------------------------------

    std::string LuaEntityHandle::GetId() const {
        ThrowIfInvalid(*this);

        return id_.ToString();
    }

    std::string LuaEntityHandle::GetName() const {
        ThrowIfInvalid(*this);

        return scene_->GetName(entity_);
    }

    void LuaEntityHandle::SetName(const std::string& name) const {
        ThrowIfInvalid(*this);

        scene_->SetName(entity_, name);
    }

    // -------------------------------------------------------------------------
    // Transform: Position
    // -------------------------------------------------------------------------

    using Vector3 = DirectX::SimpleMath::Vector3;
    using Quaternion = DirectX::SimpleMath::Quaternion;

    Vector3 LuaEntityHandle::GetPosition() const {
        ThrowIfInvalid(*this);

        return scene_->GetTransform(entity_).position;
    }

    void LuaEntityHandle::SetPosition(Vector3 position) const {
        ThrowIfInvalid(*this);

        if (!IsFinite(position)) {
            throw std::runtime_error(
                "Position components must be finite"
            );
        }

        Transform transform = scene_->GetTransform(entity_);

        transform.position = position;

        scene_->SetTransform(entity_, transform);
    }

    // -------------------------------------------------------------------------
    // Transform: Rotation
    // -------------------------------------------------------------------------

    Quaternion LuaEntityHandle::GetRotation() const {
        ThrowIfInvalid(*this);

        return scene_->GetTransform(entity_).rotation;
    }

    void LuaEntityHandle::SetRotation(Quaternion rotation) const {
        ThrowIfInvalid(*this);

        if (!IsFinite(rotation)) {
            throw std::runtime_error(
                "Rotation components must be finite"
            );
        }

        const double lengthSquared =
            double(rotation.x) * rotation.x +
            double(rotation.y) * rotation.y +
            double(rotation.z) * rotation.z +
            double(rotation.w) * rotation.w;

        if (lengthSquared <= 1e-12f) {
            throw std::runtime_error(
                "Rotation quaternion must have non-zero length"
            );
        }

        // Normalize the quaternion before storing it.
        const double inverseLength = 1.0 / std::sqrt(lengthSquared);

        rotation.x = static_cast<float>(rotation.x * inverseLength);
        rotation.y = static_cast<float>(rotation.y * inverseLength);
        rotation.z = static_cast<float>(rotation.z * inverseLength);
        rotation.w = static_cast<float>(rotation.w * inverseLength);

        Transform transform = scene_->GetTransform(entity_);

        transform.rotation = rotation;

        scene_->SetTransform(entity_, transform);
    }

    // -------------------------------------------------------------------------
    // Transform: Scale
    // -------------------------------------------------------------------------

    Vector3 LuaEntityHandle::GetScale() const {
        ThrowIfInvalid(*this);

        return scene_->GetTransform(entity_).scale;
    }

    void LuaEntityHandle::SetScale(Vector3 scale) const {
        ThrowIfInvalid(*this);

        if (!IsFinite(scale)) {
            throw std::runtime_error(
                "Scale components must be finite"
            );
        }

        Transform transform = scene_->GetTransform(entity_);

        transform.scale = scale;

        scene_->SetTransform(entity_, transform);
    }

    // -------------------------------------------------------------------------
    // Tags
    // -------------------------------------------------------------------------

    std::vector<std::string> LuaEntityHandle::GetTags() const {
        ThrowIfInvalid(*this);

        const auto tags = scene_->GetTags(entity_);

        return {
            tags.begin(),
            tags.end()
        };
    }

    bool LuaEntityHandle::HasTag(const std::string& tag) const {
        ThrowIfInvalid(*this);

        const auto tags = scene_->GetTags(entity_);

        return std::find(
            tags.begin(),
            tags.end(),
            tag
        ) != tags.end();
    }

    void LuaEntityHandle::AddTag(const std::string& tag) const {
        ThrowIfInvalid(*this);

        scene_->AddTag(entity_, tag);
    }

    void LuaEntityHandle::RemoveTag(const std::string& tag) const {
        ThrowIfInvalid(*this);

        scene_->RemoveTag(entity_, tag);
    }

    // -------------------------------------------------------------------------
    // Hierarchy: Parent
    // -------------------------------------------------------------------------

    LuaEntityHandle LuaEntityHandle::GetParent() const {
        ThrowIfInvalid(*this);

        const auto parent = scene_->GetParent(entity_);

        if (parent == entt::null)
            return {};

        return FromScene(*scene_, parent);
    }

    // -------------------------------------------------------------------------
    // Hierarchy: Children
    // -------------------------------------------------------------------------

    std::vector<LuaEntityHandle> LuaEntityHandle::GetChildren() const {
        ThrowIfInvalid(*this);

        const auto children = scene_->GetChildren(entity_);

        std::vector<LuaEntityHandle> result;
        result.reserve(children.size());

        for (const auto child : children) {
            if (!scene_->IsValid(child))
                continue;

            result.push_back(
                FromScene(*scene_, child)
            );
        }

        return result;
    }

    // -------------------------------------------------------------------------
    // Hierarchy: SetParent
    // -------------------------------------------------------------------------

    bool LuaEntityHandle::SetParent(
        const LuaEntityHandle& parent
    ) const {

        if (!IsValid())
            return false;

        if (!parent.IsValid())
            return false;

        if (!parent.BelongsTo(*scene_))
            return false;

        return scene_->SetParent(
            entity_,
            parent.Resolve()
        );
    }

    // -------------------------------------------------------------------------
    // Hierarchy: SetParentKeepingWorld
    // -------------------------------------------------------------------------

    bool LuaEntityHandle::SetParentKeepingWorld(
        const LuaEntityHandle& parent
    ) const {

        if (!IsValid())
            return false;

        if (!parent.IsValid())
            return false;

        if (!parent.BelongsTo(*scene_))
            return false;

        const auto local = DecomposeChecked(scene_->GetWorldMatrix(entity_) * ParentInverse(*scene_, parent.Resolve()));
        if (!scene_->SetParent(entity_, parent.Resolve())) return false;
        scene_->SetTransform(entity_, local);
        return true;
    }

    namespace {
        using Matrix = DirectX::SimpleMath::Matrix;

        Transform DecomposeChecked(const Matrix& matrix) {
            Transform result;
            Matrix decomposable = matrix;
            if (!decomposable.Decompose(result.scale, result.rotation, result.position) ||
                !IsFinite(result.scale) || !IsFinite(result.rotation) || !IsFinite(result.position))
                throw std::runtime_error("World transform cannot be decomposed");
            const Matrix rebuilt = result.ToMatrix();
            for (int i = 0; i < 16; ++i) {
                const float a = matrix.m[i / 4][i % 4], b = rebuilt.m[i / 4][i % 4];
                if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > 1e-4f * std::max(1.0f, std::abs(a)))
                    throw std::runtime_error("Sheared transforms are unsupported");
            }
            return result;
        }

        Matrix ParentInverse(Scene& scene, entt::entity parent) {
            if (parent == entt::null) return Matrix::Identity;
            const Matrix matrix = scene.GetWorldMatrix(parent);
            const double determinant = matrix.Determinant();
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
                throw std::runtime_error("Parent transform is singular");
            DecomposeChecked(matrix);
            return matrix.Invert();
        }

        Transform World(const LuaEntityHandle& entity) {
            ThrowIfInvalid(entity);
            return DecomposeChecked(entity.GetScene()->GetWorldMatrix(entity.Resolve()));
        }

        void SetWorld(const LuaEntityHandle& entity, const Transform& value) {
            ThrowIfInvalid(entity);
            if (!IsFinite(value.position) || !IsFinite(value.scale) || !IsFinite(value.rotation))
                throw std::runtime_error("World transform must be finite");
            auto& scene = *entity.GetScene();
            const auto local = DecomposeChecked(value.ToMatrix() * ParentInverse(scene, scene.GetParent(entity.Resolve())));
            scene.SetTransform(entity.Resolve(), local);
        }
    }

    Vector3 LuaEntityHandle::GetWorldPosition() const { return World(*this).position; }
    Quaternion LuaEntityHandle::GetWorldRotation() const { return World(*this).rotation; }
    Vector3 LuaEntityHandle::GetWorldScale() const { return World(*this).scale; }
    void LuaEntityHandle::SetWorldPosition(Vector3 value) const { auto t = World(*this); t.position = value; SetWorld(*this, t); }
    void LuaEntityHandle::SetWorldScale(Vector3 value) const { auto t = World(*this); t.scale = value; SetWorld(*this, t); }
    void LuaEntityHandle::SetWorldRotation(Quaternion value) const {
        if (!IsFinite(value)) throw std::runtime_error("Rotation must be finite");
        const double length = std::sqrt(double(value.x)*value.x + double(value.y)*value.y + double(value.z)*value.z + double(value.w)*value.w);
        if (length <= 1e-6) throw std::runtime_error("Rotation must be nonzero");
        value = Quaternion(float(value.x/length), float(value.y/length), float(value.z/length), float(value.w/length));
        auto t = World(*this); t.rotation = value; SetWorld(*this, t);
    }
    Vector3 LuaEntityHandle::Forward() const { return Vector3::Transform(Vector3::Forward, GetWorldRotation()); }
    Vector3 LuaEntityHandle::Right() const { return Vector3::Transform(Vector3::Right, GetWorldRotation()); }
    Vector3 LuaEntityHandle::Up() const { return Vector3::Transform(Vector3::Up, GetWorldRotation()); }
    bool LuaEntityHandle::Unparent(bool keepingWorld) const {
        ThrowIfInvalid(*this);
        if (!keepingWorld) return scene_->SetParent(entity_, entt::null);
        const auto world = World(*this);
        if (!scene_->SetParent(entity_, entt::null)) return false;
        scene_->SetTransform(entity_, world);
        return true;
    }
    bool LuaEntityHandle::Equals(const LuaEntityHandle& other) const {
        return IsValid() && other.IsValid() && scene_ == other.scene_ && entity_ == other.entity_ && id_ == other.id_;
    }

} // namespace engine
