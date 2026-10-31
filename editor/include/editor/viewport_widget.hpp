#pragma once

#include <QPoint>
#include <QPointF>
#include <QWidget>

#include <optional>

#include <engine/core/Surface.hpp>

namespace engine {
class InputSubsystem;
}

namespace editor {
class EditorCameraSubsystem;

/// The area the engine renders into. Owns a native window (HWND) that is
/// handed to the engine as its NativeSurface; Qt never paints it.
/// Also forwards its keyboard and mouse input to the engine's InputSubsystem.
/// While the right button is held the cursor is hidden and pinned, so mouse-look
/// can turn without hitting the screen edge.
class ViewportWidget : public QWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);

    [[nodiscard]] engine::NativeSurface surface();
    /// nullptr = input goes nowhere.
    void setInput(engine::InputSubsystem* input);
    void setCamera(EditorCameraSubsystem* camera);

    /// No Qt painting at all, so it can't draw over the swap chain.
    [[nodiscard]] QPaintEngine* paintEngine() const override;

signals:
    /// Size in physical pixels.
    void surfaceResized(quint32 width, quint32 height);
    /// Left click in viewport-local logical pixels, before forwarding the mouse button.
    void pickRequested(QPointF position, bool toggle);

protected:
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
    void releaseCursor();

    engine::InputSubsystem* input_ = nullptr;
    EditorCameraSubsystem* camera_ = nullptr;
    /// Global position of the previous mouse move; none after the cursor left the widget,
    /// so coming back in elsewhere isn't reported as a jump.
    std::optional<QPoint> last_cursor_{};
    /// Right button held: cursor hidden and warped back to last_cursor_.
    bool cursor_pinned_ = false;
};

}  // namespace editor
