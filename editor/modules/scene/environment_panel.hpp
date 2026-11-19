#pragma once

class QWidget;
namespace editor { class EditorContext; }

/// Scene-level authoring, independent of the entity inspector.
QWidget* createEnvironmentPanel(editor::EditorContext& context);
