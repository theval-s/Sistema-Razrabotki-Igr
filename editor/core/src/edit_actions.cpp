#include <editor/edit_actions.hpp>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMimeData>
#include <QUndoStack>
#include <QWidget>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <DirectXCollision.h>
#include <SimpleMath.h>
#include <editor/editor_log.hpp>
#include <nlohmann/json.hpp>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/scene/SceneDocument.hpp>
#include <engine/scene/SceneQueries.hpp>

#include <editor/editor_camera.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>
#include <editor/shortcut_registry.hpp>

namespace editor {

namespace {

using DirectX::SimpleMath::Vector3;

/// Half the size of what lights, cameras and empty entities count as when framed.
constexpr float PointExtent = 0.5f;

/// World bounds of `entity` and everything under it. Entities without a mesh or Bounds
/// count as a small box around their position.
void growBounds(const engine::Scene& scene, engine::AssetSystem& assets, const entt::entity entity,
                std::optional<DirectX::BoundingBox>& total) {
    const engine::Aabb* local = nullptr;
    if (const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(entity)) {
        if (const auto mesh = assets.Peek(renderer->mesh)) local = &mesh->bounds;
    }
    if (local == nullptr) {
        if (const auto* bounds = scene.TryGetComponent<engine::Bounds>(entity)) local = &bounds->local;
    }
    const auto matrix = scene.GetWorldMatrix(entity);
    DirectX::BoundingBox box;
    if (local != nullptr) {
        DirectX::BoundingBox local_box;
        DirectX::BoundingBox::CreateFromPoints(local_box, local->min, local->max);
        local_box.Transform(box, matrix);
    } else {
        box = DirectX::BoundingBox(matrix.Translation(), Vector3{PointExtent});
    }
    if (total) {
        DirectX::BoundingBox::CreateMerged(*total, *total, box);
    } else {
        total = box;
    }
    for (const entt::entity child : scene.GetChildren(entity)) {
        growBounds(scene, assets, child, total);
    }
}

/// Entities on the clipboard, as {"version", "parents", "scene"}.
const QString EntitiesMime = QStringLiteral("application/x-sri-entities");

bool writeEntities(std::vector<engine::SceneEntityDefinition> definitions, QByteArray& result, std::string& error) {
    nlohmann::json parents = nlohmann::json::object();
    std::unordered_set<engine::EntityUuid, engine::EntityUuidHash> inside;
    for (engine::SceneEntityDefinition& definition : definitions) {
        // Parents come before their children, so one not seen yet is outside.
        if (definition.parent && !inside.contains(*definition.parent)) {
            parents[definition.id.ToString()] = definition.parent->ToString();
            definition.parent.reset();
        }
        inside.insert(definition.id);
    }
    engine::SceneDocument document;
    document.entities = std::move(definitions);
    std::string scene;
    if (!engine::SerializeSceneDocument(document, scene, error)) return false;
    const nlohmann::json clip{{"version", 1}, {"parents", std::move(parents)}, {"scene", nlohmann::json::parse(scene)}};
    result = QByteArray::fromStdString(clip.dump());
    return true;
}

bool readEntities(const QByteArray& data, std::vector<engine::SceneEntityDefinition>& result, std::string& error) {
    try {
        const auto clip = nlohmann::json::parse(data.constBegin(), data.constEnd());
        if (clip.at("version").get<int>() != 1) throw std::invalid_argument("unsupported clipboard version");
        engine::SceneDocument document;
        if (!engine::ParseSceneDocument(clip.at("scene").dump(), document, error)) return false;
        const nlohmann::json& parents = clip.at("parents");
        for (engine::SceneEntityDefinition& definition : document.entities) {
            if (const auto it = parents.find(definition.id.ToString()); it != parents.end()) {
                definition.parent = engine::EntityUuid::Parse(it->get<std::string>());
            }
        }
        result = std::move(document.entities);
        return true;
    } catch (const std::exception& exception) {
        //do we really need exception here...?
        error = exception.what();
        return false;
    }
}

bool hasEntities(const QMimeData* data) {
    return data != nullptr && data->hasFormat(EntitiesMime);
}

/// Whether a sibling of `entity` has its name.
bool nameTaken(const engine::Scene& scene, const entt::entity entity) {
    const entt::entity parent = scene.GetParent(entity);
    const auto siblings = parent != entt::null ? scene.GetChildren(parent) : scene.GetRootEntities();
    return std::ranges::any_of(siblings, [&](const entt::entity sibling) {
        return sibling != entity && scene.GetName(sibling) == scene.GetName(entity);
    });
}

/// "Cube (3)" -> {"Cube", 3}; "Cube" -> {"Cube", 0}.
std::pair<std::string_view, int> splitCopyNumber(const std::string_view name) {
    const auto open = name.rfind(" (");
    if (open == std::string_view::npos || !name.ends_with(')')) return {name, 0};
    const std::string_view digits = name.substr(open + 2, name.size() - open - 3);
    int number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size() || number <= 0) return {name, 0};
    return {name.substr(0, open), number};
}

/// To make a copy with Copy (1), Copy (2) etc
void numberCopy(engine::Scene& scene, const entt::entity copy) {
    const std::string name = scene.GetName(copy);
    const std::string_view base = splitCopyNumber(name).first;
    const entt::entity parent = scene.GetParent(copy);
    const auto siblings = parent != entt::null ? scene.GetChildren(parent) : scene.GetRootEntities();
    int highest = 0;
    for (const entt::entity sibling : siblings) {
        const auto [sibling_base, number] = splitCopyNumber(scene.GetName(sibling));
        if (sibling_base == base) highest = std::max(highest, number);
    }
    scene.SetName(copy, std::string(base) + " (" + std::to_string(highest + 1) + ")");
}

void collectSubtree(const engine::Scene& scene, const entt::entity entity, std::vector<entt::entity>& out) {
    out.push_back(entity);
    for (const entt::entity child : scene.GetChildren(entity)) {
        collectSubtree(scene, child, out);
    }
}

}  // namespace

EditActions::EditActions(engine::Engine& engine, Selection& selection, SceneHistory& history,
                         PlaySession& play_session, ShortcutRegistry& shortcuts, QObject* parent)
    : QObject(parent), engine_(engine), selection_(selection), history_(history), play_session_(play_session) {
    const auto make = [this, &shortcuts](const QString& id, const QString& text, const QKeySequence& shortcut,
                                         auto&& slot) {
        auto* action = new QAction(text, this);
        shortcuts.bind(action, id, tr("Edit"), shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    undo_ = make(QStringLiteral("edit.undo"), tr("&Undo"), QKeySequence::Undo, [this] { history_.stack().undo(); });
    redo_ = make(QStringLiteral("edit.redo"), tr("&Redo"), QKeySequence::Redo, [this] { history_.stack().redo(); });
    cut_ = make(QStringLiteral("edit.cut"), tr("Cu&t"), QKeySequence::Cut, [this] { cutSelected(); });
    copy_ = make(QStringLiteral("edit.copy"), tr("&Copy"), QKeySequence::Copy, [this] { copySelected(); });
    paste_ = make(QStringLiteral("edit.paste"), tr("&Paste"), QKeySequence::Paste, [this] { paste(); });
    duplicate_ = make(QStringLiteral("edit.duplicate"), tr("D&uplicate"), QKeySequence(Qt::CTRL | Qt::Key_D),
                      [this] { duplicateSelected(); });
    delete_ = make(QStringLiteral("edit.delete"), tr("&Delete"), QKeySequence::Delete, [this] { deleteSelected(); });
    select_all_ = make(QStringLiteral("edit.select-all"), tr("Select &All"), QKeySequence::SelectAll,
                       [this] { selectAll(); });
    deselect_ = make(QStringLiteral("edit.deselect"), tr("D&eselect"), QKeySequence(Qt::Key_Escape),
                     [this] { selection_.clear(); });
    focus_ = make(QStringLiteral("edit.focus"), tr("&Focus Selection"), QKeySequence(Qt::Key_F),
                  [this] { focusSelection(); });
    //TODO: more?

    connect(&selection_, &Selection::changed, this, &EditActions::refresh);
    connect(&history_.stack(), &QUndoStack::indexChanged, this, &EditActions::refresh);
    connect(&play_session_, &PlaySession::playingChanged, this, &EditActions::refresh);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &EditActions::refresh);
    refresh();
}

void EditActions::attach(QWidget* scope) const {
    scope->addActions(actions());
}

QList<QAction*> EditActions::actions() const {
    return {undo_, redo_, cut_, copy_, paste_, duplicate_, delete_, select_all_, deselect_, focus_};
}

bool EditActions::copySelected() {
    std::vector<engine::SceneEntityDefinition> definitions;
    std::string error;
    QByteArray data;
    if (!engine::CaptureEntities(engine_.GetScene(), engine_.GetSceneAssets(), selection_.entities(), definitions,
                                 error) ||
        !writeEntities(std::move(definitions), data, error)) {
        editorLog().warn("Could not copy: {}", error);
        return false;
    }
    auto* mime = new QMimeData;
    mime->setData(EntitiesMime, data);
    QGuiApplication::clipboard()->setMimeData(mime);
    return true;
}

void EditActions::cutSelected() {
    // Nothing goes if it couldn't be copied.
    if (copySelected()) history_.edit(tr("Cut"), [this] { remove(); });
}

void EditActions::paste() {
    const QMimeData* clipboard = QGuiApplication::clipboard()->mimeData();
    if (!hasEntities(clipboard)) return;
    std::vector<engine::SceneEntityDefinition> definitions;
    if (std::string error; !readEntities(clipboard->data(EntitiesMime), definitions, error)) {
        editorLog().warn("Could not paste: {}", error);
        return;
    }
    history_.edit(tr("Paste"), [this, &definitions] { place(definitions); });
}

void EditActions::duplicateSelected() {
    history_.edit(tr("Duplicate"), [this] { duplicate(); });
}

void EditActions::duplicate() {
    std::vector<engine::SceneEntityDefinition> definitions;
    if (std::string error; !engine::CaptureEntities(engine_.GetScene(), engine_.GetSceneAssets(),
                                                    selection_.entities(), definitions, error)) {
        editorLog().warn("Could not duplicate: {}", error);
        return;
    }
    place(definitions);
}

void EditActions::place(const std::span<const engine::SceneEntityDefinition> definitions) {
    engine::Scene& scene = engine_.GetScene();
    const auto copies = engine::InstantiateEntities(definitions, engine_.GetSceneAssets(), scene, true);
    // The copies of the top entities, not of their children; in selection order, so the
    // primary's copy is the new primary.
    std::unordered_set<engine::EntityUuid, engine::EntityUuidHash> copied;
    std::vector<entt::entity> tops;
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto& definition = definitions[i];
        if (!definition.parent || !copied.contains(*definition.parent)) tops.push_back(copies[i]);
        copied.insert(definition.id);
    }
    // Only the top ones: their children keep their names, being under a new parent.
    for (const entt::entity top : tops) {
        if (nameTaken(scene, top)) numberCopy(scene, top);
    }
    selection_.setEntities(tops);
}

void EditActions::deleteSelected() {
    history_.edit(tr("Delete"), [this] { remove(); });
}

void EditActions::remove() {
    engine::Scene& scene = engine_.GetScene();
    // A copy: the selection can change under a span while entities go.
    const std::vector<entt::entity> entities(selection_.entities().begin(), selection_.entities().end());
    for (const entt::entity entity : entities) {
        if (scene.IsValid(entity)) {  // not already gone with its parent
            scene.DestroyEntity(entity);
        }
    }
    selection_.clear();
}

void EditActions::selectAll() {
    const engine::Scene& scene = engine_.GetScene();
    std::vector<entt::entity> entities;
    for (const entt::entity root : scene.GetRootEntities()) {
        collectSubtree(scene, root, entities);
    }
    // The primary stays the same: it's what the inspector shows.
    const entt::entity primary = selection_.primary();
    if (const auto it = std::ranges::find(entities, primary); it != entities.end()) {
        std::rotate(it, it + 1, entities.end());
    }
    selection_.setEntities(entities);
}

void EditActions::focusSelection() {
    EditorCameraSubsystem* camera = engine_.GetSubsystem<EditorCameraSubsystem>();
    if (camera == nullptr) return;
    const engine::Scene& scene = engine_.GetScene();
    std::optional<DirectX::BoundingBox> bounds;
    for (const entt::entity entity : selection_.entities()) {
        if (scene.IsValid(entity)) growBounds(scene, engine_.GetSceneAssets(), entity, bounds);
    }
    if (!bounds) return;
    const Vector3 extents = bounds->Extents;
    camera->Focus(bounds->Center, std::max(extents.Length(), PointExtent));
}

void EditActions::refresh() {
    const bool editing = !play_session_.isPlaying();
    const bool selected = editing && !selection_.isEmpty();
    const QUndoStack& stack = history_.stack();
    undo_->setEnabled(editing && stack.canUndo());
    undo_->setText(stack.canUndo() ? tr("&Undo %1").arg(stack.undoText()) : tr("&Undo"));
    redo_->setEnabled(editing && stack.canRedo());
    redo_->setText(stack.canRedo() ? tr("&Redo %1").arg(stack.redoText()) : tr("&Redo"));
    cut_->setEnabled(selected);
    copy_->setEnabled(selected);
    paste_->setEnabled(editing && hasEntities(QGuiApplication::clipboard()->mimeData()));
    duplicate_->setEnabled(selected);
    delete_->setEnabled(selected);
    select_all_->setEnabled(editing);
    deselect_->setEnabled(selected);
    focus_->setEnabled(selected);
}

}  // namespace editor
