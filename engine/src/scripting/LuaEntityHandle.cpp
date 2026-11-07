
#include <engine/scripting/LuaEntityHandle.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <engine/scene/Scene.hpp>

namespace engine {

    namespace {

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

        const float lengthSquared =
            rotation.x * rotation.x +
            rotation.y * rotation.y +
            rotation.z * rotation.z +
            rotation.w * rotation.w;

        if (lengthSquared <= 1e-12f) {
            throw std::runtime_error(
                "Rotation quaternion must have non-zero length"
            );
        }

        // Normalize the quaternion before storing it.
        const float inverseLength =
            1.0f / std::sqrt(lengthSquared);

        rotation.x *= inverseLength;
        rotation.y *= inverseLength;
        rotation.z *= inverseLength;
        rotation.w *= inverseLength;

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

        return scene_->SetParentKeepingWorld(
            entity_,
            parent.Resolve()
        );
    }

} // namespace engine
