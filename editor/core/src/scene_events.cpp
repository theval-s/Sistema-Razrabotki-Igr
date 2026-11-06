#include <editor/scene_events.hpp>

#include <utility>

namespace editor {

SceneEvents::SceneEvents(engine::Scene& scene, QObject* parent) : QObject(parent), scene_(scene) {
    observer_ = scene_.AddChangeObserver([this](const engine::Scene::ChangeKind kind, const entt::entity entity) {
        ++revision_;
        if (kind != engine::Scene::ChangeKind::Modified) {
            pending_.structure = true;
        }
        if (entity != entt::null) {
            pending_.entities.insert(entity);
        }
        if (!scheduled_) {
            scheduled_ = true;
            QMetaObject::invokeMethod(this, &SceneEvents::flush, Qt::QueuedConnection);
        }
    });
}

SceneEvents::~SceneEvents() {
    scene_.RemoveChangeObserver(observer_);
}

void SceneEvents::flush() {
    scheduled_ = false;
    const SceneChanges changes = std::exchange(pending_, {});
    emit changed(changes);
}

}  // namespace editor
