#pragma once
#include <engine/assets/RigAuthoring.hpp>
#include <QString>

namespace editor { class EditorContext; }
class QWidget;

namespace content {
engine::RigAuthoringContext rigContext(editor::EditorContext& context);
/// Only owned assets qualify; unrelated neighbours do not select a rig.
bool selectedRig(editor::EditorContext& context, const QString& path, engine::RigAssetGroup& group, std::string& error);
bool showRigResult(editor::EditorContext& context, QWidget* parent, const engine::RigPublishResult& result);
/// Saves the scene (including untitled scenes) before a destructive asset refresh.
bool prepareRigReload(editor::EditorContext& context, QWidget* parent);
void reloadRigScene(editor::EditorContext& context, QWidget* parent);
} // namespace content
