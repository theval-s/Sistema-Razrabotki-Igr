#pragma once

#include <QObject>

#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include <engine/scene/Scene.hpp>

#include <editor/editor_core_export.hpp>

namespace editor {

/// in engine we batch changes, here we unbatch them...
struct SceneChanges {
    /// The hierarchy changed
    bool structure = false;
    /// Every entity that changed, destroyed ones included (with stale handles).
    std::unordered_set<entt::entity> entities;
};

/// Engine event -> Qt event
class EDITOR_CORE_API SceneEvents : public QObject {
    Q_OBJECT

public:
    explicit SceneEvents(engine::Scene& scene, QObject* parent = nullptr);
    ~SceneEvents() override;

    SceneEvents(const SceneEvents&) = delete;
    SceneEvents& operator=(const SceneEvents&) = delete;

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

signals:
    /// Everything that changed since the last one.
    void changed(const editor::SceneChanges& changes);

private:
    void flush();

    engine::Scene& scene_;
    std::size_t observer_ = 0;
    SceneChanges pending_;
    std::uint64_t revision_ = 0;
    bool scheduled_ = false;
};

}  // namespace editor
