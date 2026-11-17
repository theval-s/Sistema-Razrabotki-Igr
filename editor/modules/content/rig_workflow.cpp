#include "rig_workflow.hpp"
#include "asset_operations.hpp"
#include <QFileInfo>
#include <QMessageBox>
#include <QStringList>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/project/Project.hpp>

namespace content {

engine::RigAuthoringContext rigContext(editor::EditorContext& context) {
    return {context.project().ContentDir(), engine::AssetPathResolver::GetRoot() / "engine",
            context.project().Root() / ".sri" / "rig-transactions"};
}

bool selectedRig(editor::EditorContext& context, const QString& path, engine::RigAssetGroup& group, std::string& error) {
    const QFileInfo info(path);
    if (!engine::ReadRigAssetGroup(rigContext(context), editor::toPath(info.isDir() ? path : info.absolutePath()), group, error))
        return false;
    if (info.isDir() || info.fileName() == QLatin1String("rig.import.json")) return true;
    const auto name = assetName(path).toStdString();
    const auto matches = [&name](const engine::AssetReference& ref) { return ref.path == name; };
    if (matches(group.source) || matches(group.mesh) || matches(group.skeleton) ||
        (group.texture && matches(*group.texture)) || (group.materialInstance && matches(*group.materialInstance))) return true;
    for (const auto& clip : group.clips) if (!clip.removed && matches(clip.asset)) return true;
    error = "The selected asset is not owned by this animated rig";
    return false;
}

bool showRigResult(editor::EditorContext& context, QWidget* parent, const engine::RigPublishResult& result) {
    QStringList messages;
    if (!result.error.empty()) messages << QString::fromStdString(result.error);
    if (result.state != engine::RigPublishState::Committed && result.error.empty())
        messages << QObject::tr("The animated rig was not published.");
    for (const auto& warning : result.warnings) messages << QString::fromStdString(warning);
    if (result.state == engine::RigPublishState::RecoveryRequired)
        messages << QObject::tr("Recovery required. Retained transaction data: %1").arg(editor::toQString(result.recoveryDirectory));
    else if (!result.recoveryDirectory.empty())
        messages << QObject::tr("Retained transaction data: %1").arg(editor::toQString(result.recoveryDirectory));
    if (!messages.isEmpty()) QMessageBox::warning(parent, QObject::tr("Animated Rig"), messages.join(QLatin1Char('\n')));
    if (result.state != engine::RigPublishState::Committed) return false;
    context.showStatus(QObject::tr("Animated rig published: %1").arg(editor::toQString(result.group.directory)), 6000);
    return true;
}

bool prepareRigReload(editor::EditorContext& context, QWidget* parent) {
    if (context.playSession().isPlaying()) return false;
    auto& document = context.sceneDocument();
    if (QMessageBox::question(parent, QObject::tr("Reimport Animated Rig"),
            QObject::tr("Reimport refreshes loaded assets by reloading the current scene. Save the current scene and continue?"),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save) != QMessageBox::Save) return false;
    return context.maybeSaveFiles(parent) && document.saveInteractive(parent, false);
}

void reloadRigScene(editor::EditorContext& context, QWidget* parent) {
    // Engine::LoadScene drives scene/component lifecycle observers, stopping previews before replacement.
    const auto path = context.sceneDocument().filePath();
    std::string error;
    if (!context.sceneDocument().open(path, error))
        QMessageBox::warning(parent, QObject::tr("Reload Scene"),
            QObject::tr("Rig committed, but scene reload failed. The saved scene is at %1.\n%2")
                .arg(editor::toQString(path), QString::fromStdString(error)));
}

} // namespace content
