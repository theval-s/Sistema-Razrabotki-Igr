#include "rig_dialog.hpp"
#include "rig_workflow.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QMainWindow>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <DockManager.h>
#include <editor/editor_context.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <editor/paths.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <gtest/gtest.h>

namespace {

QString fixture() {
    return editor::toQString(std::filesystem::path(__FILE__).parent_path().parent_path() /
                             "example_game/models/TestBot/model.glb");
}

TEST(ContentRigDialog, InspectionSelectsClipsAndScaleWithoutChangingSource) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QFileInfo::exists(fixture()));
    engine::AssetCatalog catalog;
    content::RigDialog dialog(catalog, fixture(), directory.path(), nullptr);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("rigClips"));
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->rowCount() == 2 && table->item(1, 2); }));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_TRUE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    table->item(0, 2)->setCheckState(Qt::Unchecked);
    dialog.findChild<QDoubleSpinBox*>(QStringLiteral("rigPlacementScale"))->setValue(0.022);
    const auto request = dialog.importRequest();
    EXPECT_EQ(request.clips.size(), 1u);
    EXPECT_EQ(request.clips.front(), table->item(1, 2)->data(Qt::UserRole).toString().toStdString());
    EXPECT_FLOAT_EQ(request.placementScale, 0.022f);
    EXPECT_EQ(request.sourceGlb, editor::toPath(fixture()));
    EXPECT_FALSE(request.expectedSourceRevision.empty());
    ASSERT_TRUE(QDir().mkdir(dialog.path()));
    // Revalidate after a destination collision appears while the dialog is open.
    const auto lines = dialog.findChildren<QLineEdit*>();
    ASSERT_FALSE(lines.isEmpty());
    const auto original = lines.front()->text();
    lines.front()->setText(original + "2");
    lines.front()->setText(original);
    EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
}

TEST(ContentRigDialog, MissingExistingClipRequiresExplicitRemapOrRemovalAndRetainsIdentity) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    engine::AssetCatalog catalog;
    engine::RigAssetGroup group;
    group.directory = editor::toPath(directory.filePath("Rig"));
    group.revision = "previous-group";
    const auto id = engine::NewUuid();
    group.clips.push_back({{id, "game/Rig/old.animation.json"}, "renamed-source-clip", false});
    content::RigDialog dialog(catalog, fixture(), directory.path(), nullptr, &group);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("rigClips"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->rowCount() == 3 && table->item(2, 2); }));
    auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    EXPECT_FALSE(ok->isEnabled());
    auto* mapping = qobject_cast<QComboBox*>(table->cellWidget(0, 2));
    ASSERT_NE(mapping, nullptr);
    mapping->setCurrentIndex(1);
    ASSERT_TRUE(ok->isEnabled());
    auto request = dialog.reimportRequest();
    ASSERT_EQ(request.clips.size(), 1u);
    EXPECT_EQ(request.clips.front().existingId, id);
    EXPECT_FALSE(request.clips.front().remove);
    EXPECT_EQ(request.expectedGroupRevision, group.revision);
    EXPECT_FALSE(request.refreshTexture);
    // Selecting the same source as a new clip would duplicate a mapping.
    for (int row = 1; row < table->rowCount(); ++row) {
        auto* item = table->item(row, 2);
        if (item->data(Qt::UserRole) == mapping->currentData()) item->setCheckState(Qt::Checked);
    }
    EXPECT_FALSE(ok->isEnabled());
    mapping->setCurrentIndex(0);
    ASSERT_TRUE(ok->isEnabled());
    request = dialog.reimportRequest();
    EXPECT_EQ(request.clips.front().existingId, id);
    EXPECT_TRUE(request.clips.front().remove);
    EXPECT_EQ(request.clips.size(), 2u);
}

TEST(ContentRigDialog, TombstoneRequiresExplicitReactivationEvenWhenSourceNameMatches) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    engine::AssetCatalog catalog;
    engine::RigAssetGroup group;
    group.directory = editor::toPath(directory.filePath("MovedRig"));
    group.revision = "moved-group";
    const auto id = engine::NewUuid();
    group.clips.push_back({{id, "game/MovedRig/idle.animation.json"}, "idle", true});
    content::RigDialog dialog(catalog, fixture(), directory.path(), nullptr, &group);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("rigClips"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return table->rowCount() == 3 && table->item(2, 2); }));
    auto* mapping = qobject_cast<QComboBox*>(table->cellWidget(0, 2));
    ASSERT_NE(mapping, nullptr);
    EXPECT_EQ(mapping->currentIndex(), 0);
    EXPECT_TRUE(dialog.reimportRequest().clips.empty());
    QTableWidgetItem* new_idle = nullptr;
    for (int row = 1; row < table->rowCount(); ++row)
        if (table->item(row, 2)->data(Qt::UserRole).toString() == QStringLiteral("idle")) new_idle = table->item(row, 2);
    ASSERT_NE(new_idle, nullptr);
    new_idle->setCheckState(Qt::Checked);
    auto request = dialog.reimportRequest();
    ASSERT_EQ(request.clips.size(), 1u);
    EXPECT_FALSE(request.clips.front().existingId.IsValid());
    new_idle->setCheckState(Qt::Unchecked);
    mapping->setCurrentIndex(mapping->findData(QStringLiteral("idle")));
    request = dialog.reimportRequest();
    ASSERT_EQ(request.clips.size(), 1u);
    EXPECT_EQ(request.clips.front().existingId, id);
    EXPECT_EQ(request.clips.front().sourceName, "idle");
    EXPECT_FALSE(request.clips.front().remove);
    mapping->setCurrentIndex(0);
    EXPECT_TRUE(dialog.reimportRequest().clips.empty());
}

TEST(ContentRigDialog, CommittedResultDisplaysRetainedTransactionPath) {
    QTemporaryDir directory;
    engine::Engine host;
    engine::Project project;
    project.file = editor::toPath(directory.filePath("test.sriproject"));
    QMainWindow window;
    ads::CDockManager docks(&window);
    QWidget viewport;
    editor::ViewportToolBar toolbar;
    editor::EditorContext context(host, project, window, docks, viewport, toolbar);
    engine::RigPublishResult result;
    result.state = engine::RigPublishState::Committed;
    result.group.directory = project.ContentDir() / "Rig";
    result.recoveryDirectory = project.Root() / ".sri/rig-transactions/retained";
    result.warnings.push_back("Could not remove backup");
    bool saw_path = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* message = window.findChild<QMessageBox*>()) {
            saw_path = message->text().contains(editor::toQString(result.recoveryDirectory));
            message->accept();
        }
    });
    EXPECT_TRUE(content::showRigResult(context, &window, result));
    EXPECT_TRUE(saw_path);
}

TEST(ContentRigDialog, DestructionJoinsInspectionBeforeModuleCanUnload) {
    QTemporaryDir directory;
    engine::AssetCatalog catalog;
    // Destruction joins even without an event-loop turn to consume the worker result.
    { content::RigDialog dialog(catalog, fixture(), directory.path(), nullptr); }
}

} // namespace
