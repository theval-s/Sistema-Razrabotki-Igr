#pragma once

#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <optional>

#include <engine/core/Surface.hpp>
#include <engine/input/InputTypes.hpp>

namespace engine {
class InputSubsystem;
}

namespace editor {
class EditorCameraSubsystem;
class ShortcutRegistry;

/// The area the engine renders into. Owns a native window (HWND) that is
/// handed to the engine as its NativeSurface; Qt never paints it.
/// Also forwards its keyboard and mouse input to the engine's InputSubsystem.
/// While the right button is held the cursor is hidden and pinned, so mouse-look
/// can turn without hitting the screen edge, and every key goes to the camera rather than
/// to shortcuts, so W/E/Q... keep flying.
///
/// During Play the game has the input while the viewport captures it: from Play, Resume or
/// a click into the viewport (which the game never sees) until the release shortcut, Pause,
/// Stop or focus loss. Meanwhile only the play shortcuts work, no mouse signals go out, and
/// the game's cursor mode applies: Locked pins the cursor at the centre like the right drag.
class ViewportWidget : public QWidget {
    Q_OBJECT

public:
    /// Shortcut id of the command that gives the keyboard and mouse back to the editor.
    static inline const QString ReleaseInputId = QStringLiteral("play.release-input");

    explicit ViewportWidget(QWidget* parent = nullptr);

    [[nodiscard]] engine::NativeSurface surface();
    /// nullptr = input goes nowhere.
    void setInput(engine::InputSubsystem* input);
    void setCamera(EditorCameraSubsystem* camera);
    /// Registers the release command and looks up the play shortcuts in `shortcuts`.
    void setShortcuts(ShortcutRegistry& shortcuts);
    /// Starting captures the input; stopping gives it back.
    void setPlaying(bool playing);
    /// Pausing gives the input back, resuming captures it again.
    void setPaused(bool paused);

    /// Whether the game has the keyboard and mouse.
    [[nodiscard]] bool capturesInput() const noexcept { return captured_; }
    /// Only while playing and not paused.
    void captureInput();
    void releaseInput();
    /// Follows the game's cursor mode while it has the input; call every frame.
    void applyCursorMode();

    /// No Qt painting at all, so it can't draw over the swap chain.
    [[nodiscard]] QPaintEngine* paintEngine() const override;

signals:
    /// Size in physical pixels.
    void surfaceResized(quint32 width, quint32 height);
    void inputCaptureChanged(bool captured);
    // Positions are viewport-local, in logical pixels. Never during Play, nor while the right
    // button turns the view: the cursor is hidden then, and pinned.
    /// A button went down, before the engine's input sees it.
    void mousePressed(QPointF position, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    /// The cursor moved over the viewport, or anywhere while a button is held.
    void mouseMoved(QPointF position);
    void mouseReleased(QPointF position, Qt::MouseButton button);

protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    [[nodiscard]] QSize pixelSize() const;
    /// Editing: always, the camera reads it. Playing: only while captured.
    [[nodiscard]] bool feedsEngine() const noexcept { return input_ != nullptr && (!playing_ || captured_); }
    void feedMousePosition(QPointF position);
    /// A play shortcut, which works even while the game has the keyboard.
    [[nodiscard]] bool isPlayShortcut(const QKeyEvent& event) const;
    void releaseCursor();

    engine::InputSubsystem* input_ = nullptr;
    EditorCameraSubsystem* camera_ = nullptr;
    const ShortcutRegistry* shortcuts_ = nullptr;
    /// Global position of the previous mouse move; none after the cursor left the widget,
    /// so coming back in elsewhere isn't reported as a jump.
    std::optional<QPoint> last_cursor_{};
    /// Right button held, or the game locked it: cursor hidden and warped back to last_cursor_.
    bool cursor_pinned_ = false;
    /// The game's mode as applied; Normal while it doesn't have the input.
    engine::CursorMode cursor_mode_ = engine::CursorMode::Normal;
    bool playing_ = false;
    bool paused_ = false;
    bool captured_ = false;
    /// Pressed to capture the input: their release isn't the game's either.
    Qt::MouseButtons swallowed_buttons_{};
};

}  // namespace editor
