#include <editor/scene_document.hpp>

#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QWidget>

#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <engine/scene/SceneDocument.hpp>

#include <editor/paths.hpp>
#include <editor/selection.hpp>

namespace editor {

namespace {

constexpr std::string_view SceneSuffix = ".scene.json";
constexpr const char* BuiltinPlaneFile = "engine/primitives/plane/model.obj";

QString fromUtf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString sceneFilter() {
    return QObject::tr("Scenes (*.scene.json)");
}

/// Adds .scene.json unless it's there; the catalog only recognizes scenes by it.
std::filesystem::path withSceneSuffix(std::filesystem::path file) {
    QString name = toQString(file.filename());
    if (name.endsWith(QLatin1String(SceneSuffix.data()), Qt::CaseInsensitive)) {
        return file;
    }
    if (name.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        name.chop(5);
    }
    return file.replace_filename(toPath(name + QLatin1String(SceneSuffix.data())));
}

/// A light and a ground plane, so a new scene isn't black. No camera: the editor has its own.
engine::SceneDocument defaultScene(const engine::AssetCatalog& catalog) {
    using DirectX::SimpleMath::Quaternion;
    using DirectX::SimpleMath::Vector3;
    engine::SceneDocument document;

    engine::SceneEntityDefinition sun;
    sun.id = engine::NewUuid();
    sun.name = "Sun";
    Vector3 direction{-0.45f, -1.0f, -0.3f};
    direction.Normalize();
    sun.transform.rotation = Quaternion::FromToRotation(Vector3::Forward, direction);
    sun.directionalLight = engine::DirectionalLight{Vector3{1.0f, 0.95f, 0.85f}, 1.25f};
    document.directionalLight = sun.id;
    document.entities.push_back(std::move(sun));

    const auto* plane = catalog.FindPath(BuiltinPlaneFile);
    const auto* material = catalog.FindPath(engine::BuiltinAssets::DefaultMaterialInstance);
    if (plane != nullptr && material != nullptr) {
        engine::SceneEntityDefinition ground;
        ground.id = engine::NewUuid();
        ground.name = "Ground";
        ground.transform.scale = Vector3{20.0f, 1.0f, 20.0f};
        ground.renderer = engine::SceneEntityDefinition::Mesh{{plane->metadata.id, plane->path},
                                                             {material->metadata.id, material->path}};
        document.entities.push_back(std::move(ground));
    } else {
        spdlog::warn("New scene: built-in plane or default material missing from the catalog, no ground");
    }
    return document;
}

}  // namespace

SceneDocument::SceneDocument(engine::Engine& engine, const engine::Project& project, Selection& selection,
                             QObject* parent)
    : QObject(parent), engine_(engine), project_(project), selection_(selection),
      clean_revision_(engine.GetScene().GetRevision()) {
    // TODO: add delegates to scene
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &SceneDocument::checkState);
    timer->start(200);
}

const std::filesystem::path& SceneDocument::filePath() const noexcept {
    return file_;
}

QString SceneDocument::displayName() const {
    if (file_.empty()) {
        return tr("Untitled");
    }
    QString name = toQString(file_.filename());
    if (name.endsWith(QLatin1String(SceneSuffix.data()), Qt::CaseInsensitive)) {
        name.chop(static_cast<qsizetype>(SceneSuffix.size()));
    }
    return name;
}

bool SceneDocument::isDirty() const noexcept {
    if (runtime_) return runtime_dirty_;
    return marked_dirty_ || engine_.GetScene().GetRevision() != clean_revision_;
}

void SceneDocument::markDirty() {
    marked_dirty_ = true;
    checkState();
}

void SceneDocument::markClean() {
    if (runtime_) return;
    setClean(file_);
}

void SceneDocument::beginRuntime() {
    runtime_dirty_ = isDirty();
    runtime_ = true;
}

void SceneDocument::endRuntime() {
    runtime_ = false;
    clean_revision_ = engine_.GetScene().GetRevision();
    marked_dirty_ = runtime_dirty_;
    checkState();
}

void SceneDocument::relocate(const std::filesystem::path& file) {
    file_ = file;
    checkState();
}

bool SceneDocument::open(const std::filesystem::path& file, std::string& error) {
    if (runtime_) {
        error = "Stop Play before opening an editor scene";
        return false;
    }
    const std::filesystem::path target = engine::AssetPathResolver::ToAbsolute(file);
    std::string diagnostics;
    if (!engine_.LoadScene(target, diagnostics)) {
        error = std::move(diagnostics);
        return false;
    }
    if (!diagnostics.empty()) {
        spdlog::warn("Opened {} with problems:\n{}", engine::ToUtf8(file), diagnostics);
    }
    selection_.clear();
    setClean(file);
    emit opened();
    error.clear();
    return true;
}

bool SceneDocument::create(const std::filesystem::path& file, std::string& error) {
    if (runtime_) {
        error = "Stop Play before creating a scene";
        return false;
    }
    const std::filesystem::path target = withSceneSuffix(engine::AssetPathResolver::ToAbsolute(file));
    std::string text;
    if (!engine::SerializeSceneDocument(defaultScene(engine_.GetSceneAssets().GetCatalog()), text, error) ||
        !engine::CreateNewFile(target, text + '\n', error)) {
        return false;
    }
    if (!engine::WriteAssetMetadata(target, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error)) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        return false;
    }
    if (std::string rescan; !engine_.RescanAssetCatalogs(rescan)) {
        spdlog::warn("Asset catalog: {}", rescan);
    }
    return open(target, error);
}

bool SceneDocument::save(std::string& error) {
    if (file_.empty()) {
        error = "the scene has no file yet";
        return false;
    }
    return saveAs(file_, error);
}

bool SceneDocument::saveAs(const std::filesystem::path& file, std::string& error) {
    if (runtime_) {
        error = "Stop Play before saving the editor scene";
        return false;
    }
    const std::filesystem::path target = withSceneSuffix(engine::AssetPathResolver::ToAbsolute(file));
    if (!engine_.SaveScene(target, error)) {
        return false;
    }
    if (std::error_code ec; !std::filesystem::exists(engine::AssetMetadataPath(target), ec)) {
        // A new file: give it an identity so other assets can refer to it.
        if (engine::AssetPathResolver::ToName(target).empty()) {
            spdlog::warn("{} is outside the project content: it is saved, but no asset can refer to it",
                         engine::ToUtf8(target));
        } else {
            if (!engine::WriteAssetMetadata(target, {engine::NewUuid(), engine::CatalogAssetKind::Scene}, error)) {
                return false;
            }
            if (std::string rescan; !engine_.RescanAssetCatalogs(rescan)) {
                spdlog::warn("Asset catalog: {}", rescan);
            }
        }
    }
    setClean(target);
    error.clear();
    return true;
}

bool SceneDocument::maybeSave(QWidget* parent) {
    if (!isDirty()) {
        return true;
    }
    const auto answer = QMessageBox::question(parent, tr("Unsaved Changes"),
                                              tr("Save changes to %1?").arg(displayName()),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save) {
        return saveInteractive(parent, false);
    }
    return answer == QMessageBox::Discard;
}

bool SceneDocument::saveInteractive(QWidget* parent, const bool choose_file) {
    std::filesystem::path target = file_;
    if (choose_file || target.empty()) {
        const QString chosen = QFileDialog::getSaveFileName(parent, tr("Save Scene As"), dialogDirectory(),
                                                            sceneFilter());
        if (chosen.isEmpty()) {
            return false;
        }
        target = toPath(chosen);
    }
    std::string error;
    if (!saveAs(target, error)) {
        QMessageBox::warning(parent, tr("Save Scene"), fromUtf8(error));
        return false;
    }
    return true;
}

bool SceneDocument::openInteractive(QWidget* parent, const std::filesystem::path& file) {
    if (!maybeSave(parent)) {
        return false;
    }
    std::filesystem::path target = file;
    if (target.empty()) {
        const QString chosen = QFileDialog::getOpenFileName(parent, tr("Open Scene"), dialogDirectory(),
                                                            sceneFilter());
        if (chosen.isEmpty()) {
            return false;
        }
        target = toPath(chosen);
    }
    std::string error;
    if (!open(target, error)) {
        QMessageBox::warning(parent, tr("Open Scene"), fromUtf8(error));
        return false;
    }
    return true;
}

bool SceneDocument::createInteractive(QWidget* parent, const std::filesystem::path& file) {
    if (!maybeSave(parent)) {
        return false;
    }
    std::filesystem::path target = file;
    if (target.empty()) {
        // Not overwriting: create() refuses existing files and says so.
        const QString chosen = QFileDialog::getSaveFileName(parent, tr("New Scene"), dialogDirectory(),
                                                            sceneFilter(), nullptr,
                                                            QFileDialog::DontConfirmOverwrite);
        if (chosen.isEmpty()) {
            return false;
        }
        target = toPath(chosen);
    }
    std::string error;
    if (!create(target, error)) {
        QMessageBox::warning(parent, tr("New Scene"), fromUtf8(error));
        return false;
    }
    return true;
}

void SceneDocument::setClean(const std::filesystem::path& file) {
    file_ = file;
    clean_revision_ = engine_.GetScene().GetRevision();
    marked_dirty_ = false;
    checkState();
}

void SceneDocument::checkState() {
    const bool dirty = isDirty();
    if (dirty != reported_dirty_ || file_ != reported_file_) {
        reported_dirty_ = dirty;
        reported_file_ = file_;
        emit stateChanged();
    }
}

QString SceneDocument::dialogDirectory() const {
    if (!file_.empty()) {
        return toQString(file_.parent_path());
    }
    std::error_code ec;
    const std::filesystem::path scenes = project_.ContentDir() / "scenes";
    return toQString(std::filesystem::is_directory(scenes, ec) ? scenes : project_.ContentDir());
}

}  // namespace editor
