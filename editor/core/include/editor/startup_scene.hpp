#pragma once

#include <QObject>

#include <editor/editor_core_export.hpp>

namespace editor {

class EditorContext;

/// The project's startup scene, either opened or created
class EDITOR_CORE_API StartupScene : public QObject {
    Q_OBJECT

public:
    explicit StartupScene(EditorContext& context, QObject* parent = nullptr);

    /// Creates main.scene.json if no startup scene specified
    void open();

private:
    /// On every change of the open scene's file or saved state.
    void adopt();

    EditorContext& context_;
};

}  // namespace editor
