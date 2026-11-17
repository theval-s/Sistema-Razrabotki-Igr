#include "content_browser.hpp"
#include "asset_dialogs.hpp"
#include "asset_filter_model.hpp"
#include "asset_operations.hpp"
#include "gltf_fixture.hpp"

#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <engine/ai/BehaviorTreeSource.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <DockManager.h>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QListView>
#include <QMainWindow>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <string>

namespace {

class ContentBrowserDrop : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        project.file = editor::toPath(directory.filePath("project/test.sriproject"));
        ASSERT_TRUE(QDir().mkpath(editor::toQString(project.ContentDir()) + "/audio"));
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());
    }
    void TearDown() override { engine::AssetPathResolver::SetGameRoot({}); }

    void dropAudio(const bool source_in_content, const bool read_only = false, const bool list_mode = false) {
        const QString source = source_in_content
            ? editor::toQString(project.ContentDir()) + "/tone.mp3"
            : directory.filePath("tone.mp3");
        const QByteArray bytes("ID3-test-audio-payload");
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
        file.close();

        QSettings().setValue("content/listMode", false);
        content::ContentBrowser browser(context);
        for (auto* button : browser.findChildren<QToolButton*>()) {
            if (button->toolTip() == "Show as a list") {
                // Reapplying modes used to disable the viewport's drop handling.
                button->click();
                button->click();
                button->setChecked(list_mode);
            }
        }
        browser.resize(900, 600);
        browser.show();
        QListView* view = nullptr;
        for (auto* candidate : browser.findChildren<QListView*>()) {
            if (qobject_cast<content::AssetFilterModel*>(candidate->model())) view = candidate;
        }
        ASSERT_NE(view, nullptr);
        EXPECT_EQ(view->viewMode(), list_mode ? QListView::ListMode : QListView::IconMode);
        EXPECT_TRUE(view->viewport()->acceptDrops());
        if (read_only) {
            for (auto* button : browser.findChildren<QToolButton*>()) {
                if (button->text() == "Engine") button->click();
            }
        } else {
            // Exercise navigation and the current-folder root, not just the project root.
            auto* model = qobject_cast<content::AssetFilterModel*>(view->model());
            ASSERT_NE(model, nullptr);
            ASSERT_TRUE(QTest::qWaitFor([&] { return model->rowCount(view->rootIndex()) > 0; }));
            QModelIndex audio;
            for (int row = 0; row < model->rowCount(view->rootIndex()); ++row) {
                const auto index = model->index(row, 0, view->rootIndex());
                if (index.data().toString() == "audio") audio = index;
            }
            ASSERT_TRUE(audio.isValid());
            view->activated(audio);
        }
        QCoreApplication::processEvents();
        auto* model = qobject_cast<content::AssetFilterModel*>(view->model());
        ASSERT_NE(model, nullptr);
        QSignalSpy requests(model, &content::AssetFilterModel::dropRequested);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(source)});
        const QPoint point = view->viewport()->rect().center();
        ASSERT_FALSE(view->indexAt(point).isValid());
        const auto actions = Qt::CopyAction | Qt::MoveAction;
        QDragEnterEvent enter(point, actions, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(view->viewport(), &enter);
        ASSERT_TRUE(enter.isAccepted());
        QDragMoveEvent move(point, actions, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(view->viewport(), &move);
        if (read_only) {
            EXPECT_FALSE(move.isAccepted());
            QDropEvent drop(point, actions, &mime, Qt::LeftButton, Qt::ShiftModifier);
            QApplication::sendEvent(view->viewport(), &drop);
            EXPECT_FALSE(drop.isAccepted());
            EXPECT_TRUE(requests.isEmpty());
            EXPECT_FALSE(QFile::exists(editor::toQString(engine::AssetPathResolver::GetRoot() / "engine/tone.mp3")));
            EXPECT_TRUE(QFile::exists(source));
            return;
        }
        ASSERT_TRUE(move.isAccepted());
        EXPECT_EQ(move.dropAction(), Qt::CopyAction);
        QDropEvent drop(point, actions, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(view->viewport(), &drop);
        ASSERT_TRUE(drop.isAccepted());
        EXPECT_EQ(drop.dropAction(), Qt::CopyAction);
        ASSERT_EQ(requests.size(), 1);
        EXPECT_EQ(requests[0][2].value<Qt::DropAction>(), Qt::CopyAction);
        const QString target = editor::toQString(project.ContentDir()) + "/audio/tone.mp3";
        ASSERT_TRUE(QTest::qWaitFor([&] { return QFile::exists(target); }));
        EXPECT_TRUE(QFile::exists(source));
        QFile copied(target);
        ASSERT_TRUE(copied.open(QIODevice::ReadOnly));
        EXPECT_EQ(copied.readAll(), bytes);
        engine::AssetMetadata metadata;
        std::string error;
        EXPECT_TRUE(engine::ReadAssetMetadata(editor::toPath(target), metadata, error)) << error;
        EXPECT_EQ(metadata.kind, engine::CatalogAssetKind::AudioClip);
    }

    QTemporaryDir directory;
    engine::Engine host;
    engine::Project project;
    QMainWindow window;
    ads::CDockManager docks{&window};
    QWidget viewport;
    editor::ViewportToolBar toolbar;
    editor::EditorContext context{host, project, window, docks, viewport, toolbar};
};

TEST_F(ContentBrowserDrop, ExplorerAudioCopiesIntoCurrentFolder) { dropAudio(false); }
TEST_F(ContentBrowserDrop, ExplorerAudioCopiesInListMode) { dropAudio(false, false, true); }
TEST_F(ContentBrowserDrop, ExplorerDragOfProjectFileStillCopies) { dropAudio(true); }
TEST_F(ContentBrowserDrop, EngineFolderRejectsExternalAudio) { dropAudio(false, true); }

TEST_F(ContentBrowserDrop, NewBehaviorTreeComesWithAnActionToRun) {
    const std::filesystem::path ai = project.ContentDir() / "ai";
    std::filesystem::create_directories(ai);
    std::string error;
    ASSERT_TRUE(engine::CreateNewFile(ai / "Guard.action.lua", "return {}", error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(ai / "Guard.action.lua", {engine::NewUuid(), engine::CatalogAssetKind::Script},
                                           error)) << error;
    content::AssetOperations operations(context, &window);
    ASSERT_TRUE(operations.createBehaviorTree(editor::toQString(ai / "Guard.bt.lua")));

    // Guard.action.lua was taken, so the starter is the next name.
    std::string text;
    ASSERT_TRUE(engine::ReadTextFile(ai / "Guard.bt.lua", text, error)) << error;
    engine::ai::TreeSource tree;
    ASSERT_TRUE(engine::ai::ReadTreeSource(text, tree, error)) << error;
    ASSERT_EQ(tree.nodes.size(), 1u);
    EXPECT_EQ(tree.nodes[0].script, "game/ai/guard2.action.lua");  // logical names are lowercase
    EXPECT_EQ(engine::ai::WriteTreeSource(tree), text) << "the graph editor saves it without a warning";

    const std::filesystem::path engine_root = editor::toPath(directory.filePath("engine"));
    std::filesystem::create_directories(engine_root);
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(project.ContentDir(), engine_root, error)) << error;
    EXPECT_TRUE(engine::ai::ValidateTreeSource(tree, catalog, error)) << error;
}

TEST_F(ContentBrowserDrop, ImportedModelGetsAMaterialPerSlot) {
    test::GltfFixture model;
    const int gold = model.AddImageBytes("Gold", "image/png", std::string("\x89PNG\r\n\x1a\n", 8));
    model.AddMaterial("Gold", std::format(R"("pbrMetallicRoughness":{{"baseColorTexture":{{"index":{}}}}})", gold));
    model.AddMaterial("Gem");
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 0, 1, 0, 0, 1}}, 0});
    model.AddPrimitive({{0, 0, 1, 1, 0, 1, 0, 1, 1}, {{0, 0, 1, 0, 0, 1}}, 1});
    const QString source = directory.filePath("token.glb");
    model.WriteGlb(editor::toPath(source));
    const QString content = content::cleanPath(editor::toQString(project.ContentDir()));
    std::string error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(project.ContentDir(), engine::AssetPathResolver::GetRoot() / "engine", error)) << error;

    content::ImportModelDialog dialog(catalog, source, content, &window);
    auto* slots_list = dialog.findChild<QTreeWidget*>();
    ASSERT_NE(slots_list, nullptr);
    ASSERT_EQ(slots_list->topLevelItemCount(), 2);
    EXPECT_EQ(slots_list->topLevelItem(0)->text(0), "Gold");
    EXPECT_EQ(slots_list->topLevelItem(0)->text(1), "Gold.png");
    EXPECT_EQ(slots_list->topLevelItem(1)->text(1), "None");
    const content::ModelImport import = dialog.result();
    EXPECT_EQ(import.name, "token");
    ASSERT_TRUE(import.materials.baseMaterial);
    EXPECT_EQ(import.materials.baseMaterial->path, "engine/primitives/default.material.json");
    EXPECT_TRUE(import.materials.fallbackTexture.empty()) << "the model has a texture of its own";

    content::AssetOperations operations(context, &window);
    const QString folder = operations.importModel(import, content);
    ASSERT_EQ(folder, content + "/token");
    engine::AssetMetadata mesh;
    ASSERT_TRUE(engine::ReadAssetMetadata(editor::toPath(folder + "/model.glb"), mesh, error)) << error;
    ASSERT_EQ(mesh.materials.size(), 2u);
    EXPECT_EQ(mesh.materials[0]->path, "game/token/materials/Gold.material-instance.json");
    EXPECT_TRUE(QFile::exists(folder + "/textures/Gold.png"));
    EXPECT_NE(context.engine().GetSceneAssets().GetCatalog().Find(mesh.materials[1]->id,
                                                                  engine::CatalogAssetKind::MaterialInstance),
              nullptr) << "rescanned";
}

TEST_F(ContentBrowserDrop, ExtractMaterialsGivesALooseModelItsOwn) {
    test::GltfFixture model;
    model.AddMaterial("Stone");
    model.AddPrimitive({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {{0, 0, 1, 0, 0, 1}}, 0});
    const std::filesystem::path statues = project.ContentDir() / "statues";
    std::filesystem::create_directories(statues);
    model.WriteGlb(statues / "Lion.glb");
    const QByteArray paint("not decoded");
    QFile image(editor::toQString(statues / "Lion.png"));
    ASSERT_TRUE(image.open(QIODevice::WriteOnly));
    ASSERT_EQ(image.write(paint), paint.size());
    image.close();
    std::string error;
    ASSERT_TRUE(engine::WriteAssetMetadata(statues / "Lion.glb", {engine::NewUuid(), engine::CatalogAssetKind::Mesh},
                                           error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(statues / "Lion.png",
                                           {engine::NewUuid(), engine::CatalogAssetKind::Texture}, error)) << error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(project.ContentDir(), engine::AssetPathResolver::GetRoot() / "engine", error)) << error;

    const QString file = editor::toQString(statues / "Lion.glb");
    content::ExtractMaterialsDialog dialog(catalog, file, &window);
    const engine::ModelMaterialSettings settings = dialog.result();
    // No texture of its own, so the image beside it is offered.
    EXPECT_EQ(settings.fallbackTexture, statues / "Lion.png");
    content::AssetOperations operations(context, &window);
    ASSERT_TRUE(operations.extractMaterials(file, settings));

    engine::MaterialInstanceDefinition stone;
    std::string text;
    ASSERT_TRUE(engine::ReadTextFile(statues / "Lion/materials/Stone.material-instance.json", text, error)) << error;
    ASSERT_TRUE(engine::ParseMaterialInstanceDefinition(text, stone, error)) << error;
    EXPECT_EQ(stone.parameters.albedoTexture, "game/statues/Lion.png") << "used where it is";
    EXPECT_FALSE(std::filesystem::exists(statues / "Lion/textures"));
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("SRI-tests");
    QCoreApplication::setApplicationName("content-browser-drop");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
