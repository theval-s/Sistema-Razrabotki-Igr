#pragma once

#include <QObject>
#include <QString>
#include <QUndoStack>

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <engine/scene/SceneDocument.hpp>

#include <editor/editor_core_export.hpp>

namespace engine {
class Engine;
}

namespace editor {

class SceneEvents;
class Selection;

/// Undo/redo: what an edit changes between begin() and end(), by entity UUID, becomes one step.
class EDITOR_CORE_API SceneHistory : public QObject {
    Q_OBJECT

public:
    SceneHistory(engine::Engine& engine, Selection& selection, SceneEvents& events, QObject* parent = nullptr);

    [[nodiscard]] QUndoStack& stack() noexcept { return stack_; }

    /// One step for whatever `change` does. Edits with the same nonempty `merge_key` merge until seal().
    void edit(const QString& text, const std::function<void()>& change, const QString& merge_key = {});
    /// Nested calls join the outermost transaction.
    void begin(const QString& text, const QString& merge_key = {});
    /// Records the transaction, if it changed anything.
    void end();
    /// Puts the scene back as it was at begin().
    void cancel();
    [[nodiscard]] bool isRecording() const noexcept { return depth_ > 0; }
    /// The next edit starts a new step even with the same merge key. Focus changes do this too.
    void seal();

    /// No unsaved changes: neither undone/redone steps nor changes made outside a transaction.
    [[nodiscard]] bool isClean() const;
    void setClean();
    /// Forgets every step, e.g. after opening a scene.
    void clear();
    /// Off during Play: nothing is recorded, and changes to the running copy don't count.
    void setEnabled(bool enabled);

signals:
    /// isClean() may have changed.
    void cleanChanged();

private:
    struct Snapshot {
        /// In scene order, parents first.
        std::vector<engine::SceneEntityDefinition> entities;
        std::unordered_map<engine::EntityUuid, std::size_t, engine::EntityUuidHash> index;
        engine::SceneSettings settings;
        std::vector<engine::EntityUuid> selection;
    };
    class Command;
    friend class Command;

    [[nodiscard]] bool capture(Snapshot& snapshot) const;
    /// Puts the entities, settings and selection of one side of `command` into the scene.
    void apply(const Command& command, bool after);
    /// Counts changes made since the last step outside a transaction.
    void noteUnrecorded();

    engine::Engine& engine_;
    Selection& selection_;
    SceneEvents& events_;
    QUndoStack stack_;
    int depth_ = 0;
    QString text_;
    QString merge_key_;
    std::optional<Snapshot> before_;
    std::uint64_t recorded_revision_ = 0;
    bool unrecorded_ = false;
    bool enabled_ = true;
    bool pushing_ = false;
};

}  // namespace editor
