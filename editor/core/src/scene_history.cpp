#include <editor/scene_history.hpp>

#include <QApplication>
#include <QUndoCommand>

#include <algorithm>
#include <string>
#include <utility>

#include <engine/engine.hpp>
#include <engine/scene/Scene.hpp>

#include <editor/editor_log.hpp>
#include <editor/scene_events.hpp>
#include <editor/selection.hpp>

namespace editor {

class SceneHistory::Command final : public QUndoCommand {
public:
    struct Change {
        engine::EntityUuid id;
        std::optional<engine::SceneEntityDefinition> before;
        std::optional<engine::SceneEntityDefinition> after;
    };

    /// What differs between `before` and `after`; empty() if nothing.
    Command(SceneHistory& history, const QString& text, QString key, const Snapshot& before, const Snapshot& after)
        : QUndoCommand(text), history_(history), key_(std::move(key)), settings_before_(before.settings),
          settings_after_(after.settings), selection_before_(before.selection), selection_after_(after.selection) {
        // Scene order, so recreated entities come back in it.
        for (const auto& definition : after.entities) {
            const auto found = before.index.find(definition.id);
            if (found == before.index.end()) {
                changes_.push_back({definition.id, std::nullopt, definition});
            } else if (const auto& old = before.entities[found->second]; !(old == definition)) {
                changes_.push_back({definition.id, old, definition});
            }
        }
        for (const auto& definition : before.entities) {
            if (!after.index.contains(definition.id)) changes_.push_back({definition.id, definition, std::nullopt});
        }
    }

    [[nodiscard]] bool empty() const { return changes_.empty() && settings_before_ == settings_after_; }

    void undo() override { history_.apply(*this, false); }
    void redo() override {
        if (std::exchange(pushed_, false)) return;  // push() redoes what was already done
        history_.apply(*this, true);
    }
    [[nodiscard]] int id() const override { return key_.isEmpty() ? -1 : 1; }
    bool mergeWith(const QUndoCommand* other) override {
        const auto* next = static_cast<const Command*>(other);  // only Commands have this id
        if (sealed_ || next->key_ != key_) return false;
        for (const Change& change : next->changes_) {
            const auto it = std::ranges::find(changes_, change.id, &Change::id);
            if (it != changes_.end()) {
                it->after = change.after;
            } else {
                changes_.push_back(change);
            }
        }
        std::erase_if(changes_, [](const Change& change) { return change.before == change.after; });
        settings_after_ = next->settings_after_;
        selection_after_ = next->selection_after_;
        setObsolete(empty());
        return true;
    }

    mutable bool sealed_ = false;

private:
    friend class SceneHistory;

    SceneHistory& history_;
    QString key_;
    std::vector<Change> changes_;
    engine::SceneSettings settings_before_;
    engine::SceneSettings settings_after_;
    std::vector<engine::EntityUuid> selection_before_;
    std::vector<engine::EntityUuid> selection_after_;
    bool pushed_ = true;
};

SceneHistory::SceneHistory(engine::Engine& engine, Selection& selection, SceneEvents& events, QObject* parent)
    : QObject(parent), engine_(engine), selection_(selection), events_(events), recorded_revision_(events.revision()) {
    connect(&stack_, &QUndoStack::indexChanged, this, [this] {
        if (!pushing_) seal();  // after an undo, edits don't merge into what is now on top
        emit cleanChanged();
    });
    connect(&stack_, &QUndoStack::cleanChanged, this, &SceneHistory::cleanChanged);
    connect(&events_, &SceneEvents::changed, this, &SceneHistory::cleanChanged);
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        connect(application, &QApplication::focusChanged, this, &SceneHistory::seal);
    }
}

void SceneHistory::edit(const QString& text, const std::function<void()>& change, const QString& merge_key) {
    begin(text, merge_key);
    change();
    end();
}

void SceneHistory::begin(const QString& text, const QString& merge_key) {
    if (depth_++ > 0) return;
    text_ = text;
    merge_key_ = merge_key;
    before_.reset();
    if (!enabled_) return;
    noteUnrecorded();
    if (Snapshot snapshot; capture(snapshot)) before_ = std::move(snapshot);
}

void SceneHistory::end() {
    if (depth_ == 0 || --depth_ > 0) return;
    const std::optional<Snapshot> before = std::exchange(before_, std::nullopt);
    if (!enabled_) return;
    Snapshot after;
    if (!before || !capture(after)) {
        noteUnrecorded();
        return;
    }
    auto command = std::make_unique<Command>(*this, text_, merge_key_, *before, after);
    recorded_revision_ = events_.revision();
    if (command->empty()) return;
    pushing_ = true;
    stack_.push(command.release());
    pushing_ = false;
    emit cleanChanged();
}

void SceneHistory::cancel() {
    if (depth_ == 0) return;
    depth_ = 0;
    const std::optional<Snapshot> before = std::exchange(before_, std::nullopt);
    if (!enabled_) return;
    Snapshot now;
    if (!before || !capture(now)) {
        noteUnrecorded();
        return;
    }
    recorded_revision_ = events_.revision();  // the transaction's own changes, about to be undone
    apply(Command(*this, text_, {}, *before, now), false);
}

void SceneHistory::seal() {
    if (stack_.index() == 0) return;
    if (const auto* top = dynamic_cast<const Command*>(stack_.command(stack_.index() - 1))) top->sealed_ = true;
}

bool SceneHistory::isClean() const {
    return stack_.isClean() && !unrecorded_ && events_.revision() == recorded_revision_;
}

void SceneHistory::setClean() {
    stack_.setClean();
    unrecorded_ = false;
    recorded_revision_ = events_.revision();
    emit cleanChanged();
}

void SceneHistory::clear() {
    depth_ = 0;
    before_.reset();
    stack_.clear();
    setClean();
}

void SceneHistory::setEnabled(const bool enabled) {
    if (enabled == enabled_) return;
    if (!enabled) {
        cancel();
        noteUnrecorded();
    } else {
        recorded_revision_ = events_.revision();  // what Play did to its running copy doesn't count
    }
    enabled_ = enabled;
}

bool SceneHistory::capture(Snapshot& snapshot) const {
    const engine::Scene& scene = engine_.GetScene();
    std::string error;
    const auto visit = [&](const auto& self, const entt::entity entity) -> bool {
        engine::SceneEntityDefinition definition;
        if (!engine::CaptureEntity(scene, engine_.GetSceneAssets(), entity, definition, error)) return false;
        snapshot.index.emplace(definition.id, snapshot.entities.size());
        snapshot.entities.push_back(std::move(definition));
        return std::ranges::all_of(scene.GetChildren(entity), [&](const entt::entity child) { return self(self, child); });
    };
    if (!std::ranges::all_of(scene.GetRootEntities(), [&](const entt::entity root) { return visit(visit, root); })) {
        editorLog().warn("Can't undo this edit: {}", error);
        return false;
    }
    snapshot.settings = engine::CaptureSceneSettings(scene);
    for (const entt::entity entity : selection_.entities()) {
        if (scene.IsValid(entity)) snapshot.selection.push_back(scene.GetId(entity));
    }
    return true;
}

void SceneHistory::apply(const Command& command, const bool after) {
    noteUnrecorded();
    engine::Scene& scene = engine_.GetScene();
    std::vector<engine::SceneEntityDefinition> present;
    std::vector<engine::EntityUuid> gone;
    for (const Command::Change& change : command.changes_) {
        if (const auto& side = after ? change.after : change.before) {
            present.push_back(*side);
        } else {
            gone.push_back(change.id);
        }
    }
    // Reparented before anything is destroyed, so what stays isn't taken along.
    engine::ApplyEntityDefinitions(present, engine_.GetSceneAssets(), scene);
    for (const engine::EntityUuid& id : gone) {
        if (const entt::entity entity = scene.FindByUuid(id); entity != entt::null) scene.DestroyEntity(entity);
    }
    engine::ApplySceneSettings(after ? command.settings_after_ : command.settings_before_, scene);
    std::vector<entt::entity> selected;
    for (const engine::EntityUuid& id : after ? command.selection_after_ : command.selection_before_) {
        if (const entt::entity entity = scene.FindByUuid(id); entity != entt::null) selected.push_back(entity);
    }
    selection_.setEntities(selected);
    recorded_revision_ = events_.revision();
}

void SceneHistory::noteUnrecorded() {
    if (!enabled_ || events_.revision() == recorded_revision_) return;
#ifndef NDEBUG
    if (!unrecorded_) editorLog().warn("The scene changed outside an undoable edit; it can't be undone");
#endif
    unrecorded_ = true;
    recorded_revision_ = events_.revision();
}

}  // namespace editor
