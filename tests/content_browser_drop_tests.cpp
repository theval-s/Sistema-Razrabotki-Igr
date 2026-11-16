#include "content_browser.hpp"
#include "asset_filter_model.hpp"

#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
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
#include <QUrl>
#include <gtest/gtest.h>

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

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("SRI-tests");
    QCoreApplication::setApplicationName("content-browser-drop");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
