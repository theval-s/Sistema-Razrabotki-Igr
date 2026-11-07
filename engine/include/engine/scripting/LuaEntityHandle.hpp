#pragma once

#include <memory>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <SimpleMath.h>

#include <engine/core/Uuid.hpp>
#include <engine/engine_export.hpp>
#include <engine/scene/Transform.hpp>

namespace engine {

    class Scene;

    class ENGINE_API LuaEntityHandle {
    public:
        LuaEntityHandle() = default;

        /// Creates a general-purpose handle for an existing entity.
        [[nodiscard]] static LuaEntityHandle FromScene(Scene& scene, entt::entity entity);

        /// Creates an entity handle tied to the lifetime of a script instance.
        [[nodiscard]] static LuaEntityHandle FromScriptOwner(Scene& scene, entt::entity entity, std::weak_ptr<void> scriptLifetime);

        [[nodiscard]] bool IsValid() const;

        [[nodiscard]] bool BelongsTo(const Scene& scene) const;

        [[nodiscard]] std::string GetId() const;
        [[nodiscard]] std::string GetName() const;
        void SetName(const std::string& name) const;

        [[nodiscard]] DirectX::SimpleMath::Vector3 GetPosition() const;
        void SetPosition(DirectX::SimpleMath::Vector3 position) const;
        [[nodiscard]] DirectX::SimpleMath::Quaternion GetRotation() const;
        void SetRotation(DirectX::SimpleMath::Quaternion rotation) const;
        [[nodiscard]] DirectX::SimpleMath::Vector3 GetScale() const;
        void SetScale(DirectX::SimpleMath::Vector3 scale) const;
        [[nodiscard]] DirectX::SimpleMath::Vector3 GetWorldPosition() const;
        [[nodiscard]] DirectX::SimpleMath::Quaternion GetWorldRotation() const;
        [[nodiscard]] DirectX::SimpleMath::Vector3 GetWorldScale() const;
        void SetWorldPosition(DirectX::SimpleMath::Vector3 value) const;
        void SetWorldRotation(DirectX::SimpleMath::Quaternion value) const;
        void SetWorldScale(DirectX::SimpleMath::Vector3 value) const;
        [[nodiscard]] DirectX::SimpleMath::Vector3 Forward() const;
        [[nodiscard]] DirectX::SimpleMath::Vector3 Right() const;
        [[nodiscard]] DirectX::SimpleMath::Vector3 Up() const;
        [[nodiscard]] bool Unparent(bool keepingWorld = false) const;
        [[nodiscard]] bool Equals(const LuaEntityHandle& other) const;

        [[nodiscard]] std::vector<std::string> GetTags() const;
        [[nodiscard]] bool HasTag(const std::string& tag) const;
        void AddTag(const std::string& tag) const;
        void RemoveTag(const std::string& tag) const;

        [[nodiscard]] LuaEntityHandle GetParent() const;
        [[nodiscard]] std::vector<LuaEntityHandle>GetChildren() const;
        [[nodiscard]] bool SetParent(const LuaEntityHandle& parent) const;

        [[nodiscard]] bool SetParentKeepingWorld(const LuaEntityHandle& parent) const;
        [[nodiscard]] entt::entity Resolve() const;
        [[nodiscard]] Scene* GetScene() const;

    private:
        Scene* scene_ = nullptr;
        entt::entity entity_ = entt::null;

        EntityUuid id_{};

        std::weak_ptr<void> sceneLifetime_;
        std::weak_ptr<void> scriptLifetime_;

        bool hasScriptLifetime_ = false;
    };

    /// Backward-compatible name for existing scripting code.
    using ScriptEntityHandle = LuaEntityHandle;

} // namespace engine
