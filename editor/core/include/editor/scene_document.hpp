#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <string>

#include <editor/editor_core_export.hpp>

class QWidget;

namespace engine {
class Engine;
struct Project;
}  // namespace engine

namespace editor {

class Selection;

/// The scene file being edited: which one it is, whether it has unsaved changes, and
/// opening, creating and saving it. Shared by every module that opens or saves scenes.
/// Paths are absolute; errors are UTF-8.
class EDITOR_CORE_API SceneDocument : public QObject {
    Q_OBJECT

public:
    SceneDocument(engine::Engine& engine, const engine::Project& project, Selection& selection,
                  QObject* parent = nullptr);

    /// Empty while the scene is untitled (never saved).
    [[nodiscard]] const std::filesystem::path& filePath() const noexcept;
    /// File name without .scene.json, or "Untitled".
    [[nodiscard]] QString displayName() const;
    /// Structural changes are noticed on their own (Scene::GetRevision); anything else that
    /// edits the scene, such as moving an entity, has to call markDirty().
    [[nodiscard]] bool isDirty() const noexcept;
    void markDirty();
    /// Forgets unsaved changes, e.g. after building a scene in code.
    void markClean();

    /// Loads `file` into the engine's scene. Missing assets are only logged.
    /// On failure the current scene stays.
    bool open(const std::filesystem::path& file, std::string& error);
    /// Writes a new scene with a light and a ground plane to `file`, then opens it.
    /// Fails if `file` already exists.
    bool create(const std::filesystem::path& file, std::string& error);
    /// False while untitled: use saveAs().
    bool save(std::string& error);
    /// Saves to `file` (a .scene.json; created with its .meta when new) and edits it from then on.
    bool saveAs(const std::filesystem::path& file, std::string& error);

    // Interactive versions for menus and panels; they show their own dialogs.

    /// Asks whether to save unsaved changes. False if the user cancelled.
    bool maybeSave(QWidget* parent);
    /// Saves, asking for a file if untitled or `choose_file` is set. False if cancelled or failed.
    bool saveInteractive(QWidget* parent, bool choose_file);
    /// maybeSave(), then opens `file`, or a file the user picks when it's empty.
    bool openInteractive(QWidget* parent, const std::filesystem::path& file = {});
    /// maybeSave(), then creates `file`, or a file the user picks when it's empty.
    bool createInteractive(QWidget* parent, const std::filesystem::path& file = {});

signals:
    /// A scene was loaded into the engine (open() or create()).
    void opened();
    /// filePath() or isDirty() changed.
    void stateChanged();

private:
    /// Keeps the file as the one being edited, now unmodified.
    void setClean(const std::filesystem::path& file);
    /// Emits stateChanged() if the path or dirty flag differ from what it last reported.
    void checkState();
    /// Directory the file dialogs start in.
    [[nodiscard]] QString dialogDirectory() const;

    engine::Engine& engine_;
    const engine::Project& project_;
    Selection& selection_;
    std::filesystem::path file_;
    std::uint64_t clean_revision_ = 0;
    bool marked_dirty_ = false;
    // What stateChanged() last reported.
    std::filesystem::path reported_file_;
    bool reported_dirty_ = false;
};

}  // namespace editor
