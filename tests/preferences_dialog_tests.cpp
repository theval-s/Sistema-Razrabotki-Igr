#include <editor/preferences_dialog.hpp>
#include <editor/shortcut_registry.hpp>

#include <QAction>
#include <QApplication>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <memory>

#include <gtest/gtest.h>

namespace {

using editor::PreferencesDialog;
using editor::ShortcutRegistry;

const QKeySequence CtrlS(Qt::CTRL | Qt::Key_S);
const QKeySequence CtrlY(Qt::CTRL | Qt::Key_Y);
const QKeySequence CtrlZ(Qt::CTRL | Qt::Key_Z);
const QKeySequence CtrlK(Qt::CTRL | Qt::Key_K);

class PreferencesDialogTest : public testing::Test {
protected:
    PreferencesDialogTest() {
        QSettings().clear();
        // Unscoped actions are window-wide, so any shared key conflicts.
        registry.bind(&save, QStringLiteral("file.save-scene"), QStringLiteral("File"), CtrlS);
        registry.bind(&undo, QStringLiteral("edit.undo"), QStringLiteral("Edit"), CtrlZ);
        registry.bind(&redo, QStringLiteral("edit.redo"), QStringLiteral("Edit"), CtrlY);
        dialog = std::make_unique<PreferencesDialog>(registry);
        dialog->show();
        tree = dialog->findChild<QTreeWidget*>(QStringLiteral("ShortcutTree"));
        primary = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("PrimaryShortcut"));
        alternate = dialog->findChild<QKeySequenceEdit*>(QStringLiteral("AlternateShortcut"));
        conflict = dialog->findChild<QLabel*>(QStringLiteral("ShortcutConflict"));
    }

    QTreeWidgetItem* row(const QString& title) const {
        const auto found = tree->findItems(title, Qt::MatchExactly | Qt::MatchRecursive);
        return found.size() == 1 ? found[0] : nullptr;
    }

    void select(const QString& title) const { tree->setCurrentItem(row(title)); }

    /// Answers the next message box with `button`.
    static void answer(const QMessageBox::StandardButton button) {
        QTimer::singleShot(0, [button] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->button(button)->click();
        });
    }

    ShortcutRegistry registry;
    QAction save{QStringLiteral("&Save Scene...")};
    QAction undo{QStringLiteral("&Undo")};
    QAction redo{QStringLiteral("&Redo")};
    std::unique_ptr<PreferencesDialog> dialog;
    QTreeWidget* tree = nullptr;
    QKeySequenceEdit* primary = nullptr;
    QKeySequenceEdit* alternate = nullptr;
    QLabel* conflict = nullptr;
};

TEST_F(PreferencesDialogTest, ListsCommandsUnderTheirCategories) {
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    // By name, not by when they were registered.
    EXPECT_EQ(tree->topLevelItem(0)->text(0), QStringLiteral("Edit"));
    EXPECT_EQ(tree->topLevelItem(1)->text(0), QStringLiteral("File"));
    ASSERT_EQ(tree->topLevelItem(0)->childCount(), 2);
    ASSERT_NE(row(QStringLiteral("Redo")), nullptr);
    EXPECT_EQ(row(QStringLiteral("Redo"))->text(1), CtrlY.toString(QKeySequence::NativeText));
    EXPECT_EQ(row(QStringLiteral("Redo"))->text(2), CtrlY.toString(QKeySequence::NativeText));

    // Nothing to edit until a command is picked.
    EXPECT_FALSE(primary->isEnabled());
    select(QStringLiteral("Redo"));
    EXPECT_TRUE(primary->isEnabled());
    EXPECT_EQ(primary->keySequence(), CtrlY);
    EXPECT_TRUE(alternate->keySequence().isEmpty());
}

TEST_F(PreferencesDialogTest, RecordingAKeyRebindsTheCommand) {
    select(QStringLiteral("Redo"));
    QTest::keyClick(alternate, Qt::Key_K, Qt::ControlModifier);
    const QList<QKeySequence> both{CtrlY, CtrlK};
    EXPECT_EQ(registry.shortcuts(QStringLiteral("edit.redo")), both);
    EXPECT_EQ(redo.shortcuts(), both);
    EXPECT_TRUE(row(QStringLiteral("Redo"))->font(1).bold());
    EXPECT_FALSE(row(QStringLiteral("Undo"))->font(1).bold());

    QTest::keyClick(primary, Qt::Key_F7);
    EXPECT_EQ(redo.shortcuts(), (QList<QKeySequence>{QKeySequence(Qt::Key_F7), CtrlK}));
}

TEST_F(PreferencesDialogTest, WarnsAboutConflictsAndResets) {
    select(QStringLiteral("Undo"));
    EXPECT_TRUE(conflict->text().isEmpty());
    QTest::keyClick(primary, Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(undo.shortcuts(), QList<QKeySequence>{CtrlY});
    EXPECT_TRUE(conflict->text().contains(QStringLiteral("Redo (Edit)")));
    // Both rows show it.
    EXPECT_TRUE(row(QStringLiteral("Redo"))->toolTip(1).contains(QStringLiteral("Undo (Edit)")));

    primary->clear();
    EXPECT_TRUE(undo.shortcuts().isEmpty());
    EXPECT_TRUE(conflict->text().isEmpty());
    EXPECT_TRUE(row(QStringLiteral("Redo"))->toolTip(1).isEmpty());

    auto* reset = dialog->findChild<QPushButton*>(QStringLiteral("ResetShortcut"));
    ASSERT_TRUE(reset->isEnabled());
    reset->click();
    EXPECT_EQ(undo.shortcuts(), QList<QKeySequence>{CtrlZ});
    EXPECT_EQ(primary->keySequence(), CtrlZ);
    EXPECT_FALSE(reset->isEnabled());
}

TEST_F(PreferencesDialogTest, FilterMatchesCategoriesTitlesAndKeys) {
    auto* filter = dialog->findChild<QLineEdit*>(QStringLiteral("ShortcutFilter"));
    EXPECT_EQ(dialog->focusWidget(), filter);
    filter->setText(QStringLiteral("redo"));
    EXPECT_FALSE(row(QStringLiteral("Redo"))->isHidden());
    EXPECT_TRUE(row(QStringLiteral("Undo"))->isHidden());
    EXPECT_TRUE(tree->topLevelItem(1)->isHidden());

    filter->setText(CtrlS.toString(QKeySequence::NativeText).toLower());
    EXPECT_FALSE(row(QStringLiteral("Save Scene"))->isHidden());
    EXPECT_TRUE(tree->topLevelItem(0)->isHidden());

    filter->setText(QStringLiteral("edit"));
    EXPECT_FALSE(row(QStringLiteral("Undo"))->isHidden());
    EXPECT_FALSE(row(QStringLiteral("Redo"))->isHidden());
    EXPECT_TRUE(row(QStringLiteral("Save Scene"))->isHidden());

    filter->clear();
    EXPECT_FALSE(row(QStringLiteral("Save Scene"))->isHidden());
}

TEST_F(PreferencesDialogTest, ResetAllAsksFirst) {
    auto* reset_all = dialog->findChild<QPushButton*>(QStringLiteral("ResetAllShortcuts"));
    EXPECT_FALSE(reset_all->isEnabled());
    registry.setShortcuts(QStringLiteral("file.save-scene"), {QKeySequence(Qt::Key_F7)});
    registry.setShortcuts(QStringLiteral("edit.undo"), {});
    ASSERT_TRUE(reset_all->isEnabled());

    answer(QMessageBox::No);
    reset_all->click();
    EXPECT_TRUE(undo.shortcuts().isEmpty());

    answer(QMessageBox::Yes);
    reset_all->click();
    EXPECT_EQ(save.shortcuts(), QList<QKeySequence>{CtrlS});
    EXPECT_EQ(undo.shortcuts(), QList<QKeySequence>{CtrlZ});
    EXPECT_FALSE(QSettings().contains(QStringLiteral("shortcuts/file.save-scene")));
    EXPECT_FALSE(reset_all->isEnabled());
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    // Saved shortcuts go to a scratch ini file, not the editor's settings.
    const QTemporaryDir settings_dir;
    QCoreApplication::setOrganizationName(QStringLiteral("SRI Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("preferences_dialog_tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
