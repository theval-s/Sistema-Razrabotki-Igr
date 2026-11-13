#include <editor/edit_actions.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>
#include <editor/scene_events.hpp>
#include <editor/scene_history.hpp>
#include <editor/selection.hpp>
#include <editor/shortcut_registry.hpp>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QSettings>
#include <QTemporaryDir>
#include <QUndoStack>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <engine/scene/Scene.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace {

class EditActionsTest : public testing::Test {
protected:
    EditActionsTest() {
        QGuiApplication::clipboard()->clear();
        rig = scene().CreateEntity("Rig");
        arm = scene().CreateEntity("Arm", rig);
        hand = scene().CreateEntity("Hand", arm);
        history.stack().clear();
    }

    engine::Scene& scene() { return host.GetScene(); }

    /// The entities under `parent` (the roots for entt::null) named `name`.
    std::vector<entt::entity> named(const entt::entity parent, const std::string& name) {
        std::vector<entt::entity> result;
        const auto entities = parent != entt::null ? scene().GetChildren(parent) : scene().GetRootEntities();
        for (const entt::entity entity : entities) {
            if (scene().GetName(entity) == name) result.push_back(entity);
        }
        return result;
    }

    engine::Engine host;
    engine::Project project;
    editor::SceneEvents events{host.GetScene()};
    editor::Selection selection{host.GetScene(), events};
    editor::SceneHistory history{host, selection, events};
    editor::SceneDocument document{host, project, selection, history};
    editor::PlaySession play{host, project, selection, document};
    editor::ShortcutRegistry shortcuts;
    editor::EditActions edit{host, selection, history, play, shortcuts};
    entt::entity rig = entt::null;
    entt::entity arm = entt::null;
    entt::entity hand = entt::null;
};

TEST_F(EditActionsTest, PasteMakesASelectedCopyBesideTheOriginal) {
    EXPECT_FALSE(edit.copyAction()->isEnabled());
    EXPECT_FALSE(edit.pasteAction()->isEnabled());
    selection.select(arm);
    ASSERT_TRUE(edit.copyAction()->isEnabled());
    edit.copyAction()->trigger();
    // Not text, so it can't land in a text field.
    EXPECT_TRUE(QGuiApplication::clipboard()->text().isEmpty());
    EXPECT_EQ(history.stack().count(), 0);
    ASSERT_TRUE(edit.pasteAction()->isEnabled());

    edit.pasteAction()->trigger();
    const auto copies = named(rig, "Arm (1)");
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(named(copies[0], "Hand").size(), 1u);
    EXPECT_NE(scene().GetId(copies[0]), scene().GetId(arm));
    EXPECT_EQ(std::vector<entt::entity>(selection.entities().begin(), selection.entities().end()), copies);

    // A second paste numbers on.
    edit.pasteAction()->trigger();
    EXPECT_EQ(named(rig, "Arm (2)").size(), 1u);
    EXPECT_EQ(history.stack().count(), 2);
    history.stack().undo();
    history.stack().undo();
    EXPECT_EQ(scene().GetChildren(rig).size(), 1u);
}

TEST_F(EditActionsTest, CutIsOneStepAndPastesBackUnderTheSameName) {
    selection.select(arm);
    const auto arm_id = scene().GetId(arm);
    edit.cutAction()->trigger();
    EXPECT_FALSE(scene().IsValid(arm));
    EXPECT_FALSE(scene().IsValid(hand));
    EXPECT_TRUE(selection.isEmpty());
    EXPECT_EQ(history.stack().count(), 1);

    edit.pasteAction()->trigger();
    const auto pasted = named(rig, "Arm");
    ASSERT_EQ(pasted.size(), 1u);
    EXPECT_NE(scene().GetId(pasted[0]), arm_id);
    EXPECT_EQ(named(pasted[0], "Hand").size(), 1u);

    // Undoing both brings the original back.
    history.stack().undo();
    history.stack().undo();
    EXPECT_TRUE(scene().FindByUuid(arm_id) != entt::null);
}

TEST_F(EditActionsTest, AMissingParentPastesAsARoot) {
    selection.select(arm);
    edit.copyAction()->trigger();
    // As in another scene, which doesn't have the rig.
    scene().DestroyEntity(rig);
    edit.pasteAction()->trigger();
    const auto pasted = named(entt::null, "Arm");
    ASSERT_EQ(pasted.size(), 1u);
    EXPECT_EQ(named(pasted[0], "Hand").size(), 1u);
}

TEST_F(EditActionsTest, OtherClipboardContentsDontPaste) {
    QGuiApplication::clipboard()->setText(QStringLiteral(R"({"version": 1})"));
    EXPECT_FALSE(edit.pasteAction()->isEnabled());
    edit.pasteAction()->trigger();
    EXPECT_EQ(history.stack().count(), 0);
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    // The shortcut registry reads saved shortcuts: keep them in a scratch ini file.
    const QTemporaryDir settings_dir;
    QCoreApplication::setOrganizationName(QStringLiteral("SRI Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("edit_actions_tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
