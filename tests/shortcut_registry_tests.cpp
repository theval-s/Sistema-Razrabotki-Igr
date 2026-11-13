#include <editor/shortcut_registry.hpp>

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

using editor::ShortcutRegistry;

const QKeySequence CtrlK(Qt::CTRL | Qt::Key_K);
const QKeySequence F7(Qt::Key_F7);

class ShortcutRegistryTest : public testing::Test {
protected:
    ShortcutRegistryTest() { QSettings().clear(); }

    /// An action whose shortcut works while `scope` or its children have focus.
    QAction* scoped(QWidget& scope, const QString& id, const QKeySequence& shortcut = {}) {
        auto* action = new QAction(id, &scope);
        registry.bind(action, id, QStringLiteral("Test"), shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        scope.addAction(action);
        return action;
    }

    ShortcutRegistry registry;
};

TEST_F(ShortcutRegistryTest, BindGivesTheDefaultAndListsTheCommand) {
    QAction action(QStringLiteral("&Save Scene..."));
    registry.bind(&action, QStringLiteral("file.save-scene"), QStringLiteral("File"), CtrlK);
    EXPECT_EQ(action.shortcuts(), QList<QKeySequence>{CtrlK});

    const auto commands = registry.commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands[0].id, QStringLiteral("file.save-scene"));
    EXPECT_EQ(commands[0].category, QStringLiteral("File"));
    EXPECT_EQ(commands[0].title, QStringLiteral("Save Scene"));
    EXPECT_EQ(commands[0].defaults, QList<QKeySequence>{CtrlK});
    EXPECT_EQ(registry.shortcuts(QStringLiteral("file.save-scene")), QList<QKeySequence>{CtrlK});
}

TEST_F(ShortcutRegistryTest, ChangesReachEveryBoundActionAndOutliveARestart) {
    QAction first, second;
    registry.bind(&first, QStringLiteral("play.play"), QStringLiteral("Play"), CtrlK);
    registry.bind(&second, QStringLiteral("play.play"), QStringLiteral("Play"));
    EXPECT_EQ(registry.commands().size(), 1);
    EXPECT_EQ(second.shortcuts(), QList<QKeySequence>{CtrlK});

    int changes = 0;
    QObject::connect(&registry, &ShortcutRegistry::changed, [&](const QString& id) {
        EXPECT_EQ(id, QStringLiteral("play.play"));
        ++changes;
    });
    ASSERT_TRUE(registry.setShortcuts(QStringLiteral("play.play"), {F7, QKeySequence(), F7, CtrlK}));
    EXPECT_EQ(changes, 1);
    const QList<QKeySequence> changed{F7, CtrlK};
    EXPECT_EQ(first.shortcuts(), changed);
    EXPECT_EQ(second.shortcuts(), changed);

    ShortcutRegistry restarted;
    QAction later;
    restarted.bind(&later, QStringLiteral("play.play"), QStringLiteral("Play"), CtrlK);
    EXPECT_EQ(later.shortcuts(), changed);
    EXPECT_EQ(restarted.commands()[0].defaults, QList<QKeySequence>{CtrlK});
}

TEST_F(ShortcutRegistryTest, ClearingIsSavedAndResetForgetsIt) {
    QAction action;
    registry.bind(&action, QStringLiteral("edit.focus"), QStringLiteral("Edit"), F7);
    ASSERT_TRUE(registry.setShortcuts(QStringLiteral("edit.focus"), {}));
    EXPECT_TRUE(action.shortcuts().isEmpty());
    {
        ShortcutRegistry restarted;
        QAction later;
        restarted.bind(&later, QStringLiteral("edit.focus"), QStringLiteral("Edit"), F7);
        EXPECT_TRUE(later.shortcuts().isEmpty());
    }

    ASSERT_TRUE(registry.reset(QStringLiteral("edit.focus")));
    EXPECT_EQ(action.shortcuts(), QList<QKeySequence>{F7});
    EXPECT_FALSE(QSettings().contains(QStringLiteral("shortcuts/edit.focus")));
}

TEST_F(ShortcutRegistryTest, UnknownIdsAreRefused) {
    EXPECT_FALSE(registry.setShortcuts(QStringLiteral("nope"), {F7}));
    EXPECT_FALSE(registry.reset(QStringLiteral("nope")));
    EXPECT_TRUE(registry.shortcuts(QStringLiteral("nope")).isEmpty());
    EXPECT_TRUE(registry.conflicts(QStringLiteral("nope"), F7).isEmpty());
}

TEST_F(ShortcutRegistryTest, MatchesKeyPressesAgainstTheCurrentShortcuts) {
    QAction action;
    registry.bind(&action, QStringLiteral("play.stop"), QStringLiteral("Play"), QKeySequence(Qt::SHIFT | Qt::Key_F5));
    const auto press = [](const Qt::Key key, const Qt::KeyboardModifiers modifiers) {
        return QKeyEvent(QEvent::KeyPress, key, modifiers);
    };
    EXPECT_TRUE(registry.matches(QStringLiteral("play.stop"), press(Qt::Key_F5, Qt::ShiftModifier)));
    EXPECT_FALSE(registry.matches(QStringLiteral("play.stop"), press(Qt::Key_F5, Qt::NoModifier)));
    EXPECT_FALSE(registry.matches(QStringLiteral("nope"), press(Qt::Key_F5, Qt::ShiftModifier)));

    ASSERT_TRUE(registry.setShortcuts(QStringLiteral("play.stop"), {QKeySequence(Qt::Key_Enter)}));
    EXPECT_FALSE(registry.matches(QStringLiteral("play.stop"), press(Qt::Key_F5, Qt::ShiftModifier)));
    // The numpad's Enter is the same shortcut.
    EXPECT_TRUE(registry.matches(QStringLiteral("play.stop"), press(Qt::Key_Enter, Qt::KeypadModifier)));
}

TEST_F(ShortcutRegistryTest, ConflictsOnlyWhereBothShortcutsWork) {
    QWidget window;
    auto* hierarchy = new QWidget(&window);
    auto* content = new QWidget(&window);
    auto* tree = new QWidget(hierarchy);
    scoped(*hierarchy, QStringLiteral("edit.delete"), F7);
    scoped(*content, QStringLiteral("content.delete"), F7);
    scoped(*tree, QStringLiteral("tree.rename"));

    // Sibling panels don't clash; a panel's shortcut reaches its children.
    EXPECT_TRUE(registry.conflicts(QStringLiteral("content.delete"), F7).isEmpty());
    EXPECT_EQ(registry.conflicts(QStringLiteral("tree.rename"), F7), QStringList{QStringLiteral("edit.delete")});

    // A widget-only shortcut doesn't reach the children.
    auto* list = new QWidget(content);
    auto* copy = new QAction(&window);
    registry.bind(copy, QStringLiteral("content.copy"), QStringLiteral("Test"), CtrlK);
    copy->setShortcutContext(Qt::WidgetShortcut);
    content->addAction(copy);
    scoped(*list, QStringLiteral("list.copy"));
    EXPECT_TRUE(registry.conflicts(QStringLiteral("list.copy"), CtrlK).isEmpty());

    // Window-wide shortcuts clash with everything.
    auto* play = new QAction(&window);
    registry.bind(play, QStringLiteral("play.play"), QStringLiteral("Play"));
    window.addAction(play);
    const QStringList both{QStringLiteral("edit.delete"), QStringLiteral("content.delete")};
    EXPECT_EQ(registry.conflicts(QStringLiteral("play.play"), F7), both);
    EXPECT_TRUE(registry.conflicts(QStringLiteral("play.play"), QKeySequence()).isEmpty());
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    // Saved shortcuts go to a scratch ini file, not the editor's settings.
    const QTemporaryDir settings_dir;
    QCoreApplication::setOrganizationName(QStringLiteral("SRI Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("shortcut_registry_tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
