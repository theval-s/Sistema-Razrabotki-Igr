#include <editor/shortcut_registry.hpp>
#include <editor/viewport_widget.hpp>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

#include <gtest/gtest.h>

#include <engine/core/FrameContext.hpp>
#include <engine/input/InputSubsystem.hpp>

namespace {

using editor::ShortcutRegistry;
using editor::ViewportWidget;

const QPoint Inside(40, 40);

class ViewportInputTest : public testing::Test {
protected:
    ViewportInputTest() {
        QSettings().clear();
        // A window-wide editor shortcut, which the game must get the key of, and a play control, which it mustn't.
        window.addAction(&save);
        registry.bind(&save, QStringLiteral("file.save-scene"), QStringLiteral("File"), QKeySequence(Qt::CTRL | Qt::Key_S));
        window.addAction(&stop);
        registry.bind(&stop, QStringLiteral("play.stop"), QStringLiteral("Play"), QKeySequence(Qt::SHIFT | Qt::Key_F5));
        QObject::connect(&save, &QAction::triggered, [this] { ++saves; });
        QObject::connect(&stop, &QAction::triggered, [this] { ++stops; });

        auto* layout = new QVBoxLayout(&window);
        field = new QLineEdit;
        viewport = new ViewportWidget;
        layout->addWidget(field);
        layout->addWidget(viewport, 1);
        viewport->setInput(&input);
        viewport->setShortcuts(registry);
        QObject::connect(viewport, &ViewportWidget::mousePressed, [this] { ++presses; });
        QObject::connect(viewport, &ViewportWidget::mouseMoved, [this] { ++moves; });
        QObject::connect(viewport, &ViewportWidget::mouseReleased, [this] { ++releases; });

        window.resize(200, 200);
        window.show();
        window.activateWindow();
        EXPECT_TRUE(QTest::qWaitForWindowActive(&window));
    }

    /// The engine's next frame.
    void frame() { input.Update(engine::FrameContext{}); }

    /// QTest::mouseMove only moves the cursor, which the offscreen platform doesn't report.
    void move(const QPoint position, const Qt::MouseButtons held = {}) const {
        QMouseEvent event(QEvent::MouseMove, position, viewport->mapToGlobal(position), Qt::NoButton, held, {});
        QApplication::sendEvent(viewport, &event);
    }

    engine::InputSubsystem input;
    ShortcutRegistry registry;
    QWidget window;
    QAction save{QStringLiteral("Save Scene")};
    QAction stop{QStringLiteral("Stop")};
    QLineEdit* field = nullptr;
    ViewportWidget* viewport = nullptr;
    int saves = 0, stops = 0;
    int presses = 0, moves = 0, releases = 0;
};

TEST_F(ViewportInputTest, EditingReportsTheMouseToTheEditor) {
    QTest::mouseClick(viewport, Qt::LeftButton, {}, Inside);
    move(Inside + QPoint(5, 5));
    EXPECT_EQ(presses, 1);
    EXPECT_EQ(releases, 1);
    EXPECT_EQ(moves, 1);
    EXPECT_FALSE(viewport->capturesInput());
}

TEST_F(ViewportInputTest, PlayGivesTheMouseToTheGameOnly) {
    viewport->setPlaying(true);
    EXPECT_TRUE(viewport->capturesInput());
    EXPECT_TRUE(viewport->hasFocus());
    presses = moves = releases = 0;

    QTest::mousePress(viewport, Qt::LeftButton, {}, Inside);
    frame();
    EXPECT_TRUE(input.WasPressed(engine::MouseButton::Left));
    move(Inside + QPoint(5, 5), Qt::LeftButton);
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, Inside + QPoint(5, 5));
    frame();
    EXPECT_TRUE(input.WasReleased(engine::MouseButton::Left));
    EXPECT_GT(input.GetMouseDelta().x, 0.0f);
    EXPECT_EQ(presses + moves + releases, 0);
}

TEST_F(ViewportInputTest, OnlyPlayControlsTakeKeysFromTheGame) {
    QTest::keyClick(viewport, Qt::Key_S, Qt::ControlModifier);
    EXPECT_EQ(saves, 1);

    viewport->setPlaying(true);
    QTest::keyClick(viewport, Qt::Key_S, Qt::ControlModifier);
    frame();
    EXPECT_EQ(saves, 1);
    EXPECT_TRUE(input.WasPressed(engine::Key::S));

    QTest::keyClick(viewport, Qt::Key_F5, Qt::ShiftModifier);
    EXPECT_EQ(stops, 1);
}

TEST_F(ViewportInputTest, ReleaseShortcutHandsEverythingBack) {
    int changes = 0;
    QObject::connect(viewport, &ViewportWidget::inputCaptureChanged, [&] { ++changes; });
    viewport->setPlaying(true);
    QTest::keyPress(viewport, Qt::Key_W);
    frame();
    ASSERT_TRUE(input.IsDown(engine::Key::W));

    QTest::keyClick(viewport, Qt::Key_Escape);
    frame();
    EXPECT_FALSE(viewport->capturesInput());
    EXPECT_EQ(changes, 2);
    // Held keys are let go, and the release key itself isn't the game's.
    EXPECT_FALSE(input.IsDown(engine::Key::W));
    EXPECT_FALSE(input.WasPressed(engine::Key::Escape));
    QTest::keyRelease(viewport, Qt::Key_W);
    QTest::keyClick(viewport, Qt::Key_S, Qt::ControlModifier);
    frame();
    EXPECT_EQ(saves, 1);
    EXPECT_FALSE(input.WasPressed(engine::Key::S));
}

TEST_F(ViewportInputTest, ReleaseShortcutCanBeRebound) {
    ASSERT_TRUE(registry.setShortcuts(ViewportWidget::ReleaseInputId, {QKeySequence(Qt::SHIFT | Qt::Key_F1)}));
    viewport->setPlaying(true);
    QTest::keyClick(viewport, Qt::Key_Escape);
    frame();
    EXPECT_TRUE(viewport->capturesInput());
    EXPECT_TRUE(input.WasPressed(engine::Key::Escape));

    QTest::keyClick(viewport, Qt::Key_F1, Qt::ShiftModifier);
    EXPECT_FALSE(viewport->capturesInput());
}

TEST_F(ViewportInputTest, TheClickThatRecapturesIsSwallowed) {
    viewport->setPlaying(true);
    viewport->releaseInput();
    QTest::mouseClick(viewport, Qt::LeftButton, {}, Inside);
    frame();
    EXPECT_TRUE(viewport->capturesInput());
    EXPECT_FALSE(input.WasPressed(engine::MouseButton::Left));
    EXPECT_FALSE(input.WasReleased(engine::MouseButton::Left));
    EXPECT_EQ(presses + releases, 0);

    QTest::mouseClick(viewport, Qt::LeftButton, {}, Inside);
    frame();
    EXPECT_TRUE(input.WasPressed(engine::MouseButton::Left));
}

TEST_F(ViewportInputTest, FocusLossReleases) {
    viewport->setPlaying(true);
    field->setFocus();
    EXPECT_FALSE(viewport->capturesInput());
    // Nor does getting the focus back capture: only a click does.
    viewport->setFocus();
    EXPECT_FALSE(viewport->capturesInput());
}

TEST_F(ViewportInputTest, PauseReleasesUntilResumed) {
    viewport->setPlaying(true);
    viewport->setPaused(true);
    EXPECT_FALSE(viewport->capturesInput());
    QTest::mouseClick(viewport, Qt::LeftButton, {}, Inside);
    EXPECT_FALSE(viewport->capturesInput());

    viewport->setPaused(false);
    EXPECT_TRUE(viewport->capturesInput());
}

TEST_F(ViewportInputTest, GameCursorModeAppliesWhileTheGameHasInput) {
    viewport->setPlaying(true);
    input.SetCursorMode(engine::CursorMode::Locked);
    viewport->applyCursorMode();
    const QPoint center = viewport->rect().center();
    EXPECT_EQ(viewport->cursor().shape(), Qt::BlankCursor);
    EXPECT_EQ(QWidget::mouseGrabber(), viewport);
    EXPECT_EQ(QCursor::pos(), viewport->mapToGlobal(center));

    // Only the motion is the game's: the cursor goes back to the centre.
    move(center + QPoint(7, 3));
    frame();
    EXPECT_GT(input.GetMouseDelta().x, 0.0f);
    EXPECT_GT(input.GetMouseDelta().y, 0.0f);
    EXPECT_EQ(QCursor::pos(), viewport->mapToGlobal(center));

    QTest::keyClick(viewport, Qt::Key_Escape);
    EXPECT_EQ(viewport->cursor().shape(), Qt::ArrowCursor);
    EXPECT_EQ(QWidget::mouseGrabber(), nullptr);
    // The request outlives the release, so the recapturing click locks again.
    QTest::mouseClick(viewport, Qt::LeftButton, {}, Inside);
    EXPECT_EQ(QWidget::mouseGrabber(), viewport);

    input.SetCursorMode(engine::CursorMode::Hidden);
    viewport->applyCursorMode();
    EXPECT_EQ(viewport->cursor().shape(), Qt::BlankCursor);
    EXPECT_EQ(QWidget::mouseGrabber(), nullptr);
    viewport->setPlaying(false);
    EXPECT_EQ(viewport->cursor().shape(), Qt::ArrowCursor);
}

TEST_F(ViewportInputTest, StopGivesTheViewportBackToTheEditor) {
    viewport->setPlaying(true);
    viewport->setPlaying(false);
    EXPECT_FALSE(viewport->capturesInput());
    QTest::mouseClick(viewport, Qt::LeftButton, {}, Inside);
    EXPECT_EQ(presses, 1);
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    // Saved shortcuts go to a scratch ini file, not the editor's settings.
    const QTemporaryDir settings_dir;
    QCoreApplication::setOrganizationName(QStringLiteral("SRI Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("viewport_input_tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
