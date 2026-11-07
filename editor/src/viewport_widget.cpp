#include <editor/viewport_widget.hpp>

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <cmath>

#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32PhysicalKey.hpp>
#include <editor/editor_camera.hpp>

namespace editor {

namespace {

void feedMousePosition(ViewportWidget& viewport, engine::InputSubsystem* input, QPointF position) {
    if (input && viewport.hasFocus()) {
        const qreal ratio = viewport.devicePixelRatioF();
        input->OnMousePosition(static_cast<float>(position.x() * ratio),
                               static_cast<float>(position.y() * ratio));
    }
}

engine::Key toEngineKey(const int key) {
    using engine::Key;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (key - Qt::Key_A));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return static_cast<Key>(static_cast<int>(Key::Num0) + (key - Qt::Key_0));
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (key - Qt::Key_F1));
    }
    switch (key) {
        case Qt::Key_Space: return Key::Space;
        case Qt::Key_Return:
        case Qt::Key_Enter: return Key::Enter;
        case Qt::Key_Escape: return Key::Escape;
        case Qt::Key_Tab: return Key::Tab;
        case Qt::Key_Backspace: return Key::Backspace;
        case Qt::Key_Delete: return Key::Delete;
        case Qt::Key_Left: return Key::Left;
        case Qt::Key_Right: return Key::Right;
        case Qt::Key_Up: return Key::Up;
        case Qt::Key_Down: return Key::Down;
        case Qt::Key_Shift: return Key::Shift;
        case Qt::Key_Control: return Key::Control;
        case Qt::Key_Alt: return Key::Alt;
        default: return Key::Unknown;
    }
}

std::optional<engine::MouseButton> toEngineButton(const Qt::MouseButton button) {
    switch (button) {
        case Qt::LeftButton: return engine::MouseButton::Left;
        case Qt::RightButton: return engine::MouseButton::Right;
        case Qt::MiddleButton: return engine::MouseButton::Middle;
        default: return std::nullopt;
    }
}

}  // namespace

ViewportWidget::ViewportWidget(QWidget* parent) : QWidget(parent) {
    // A real native child window, so the renderer can create a swap chain on it.
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_PaintOnScreen);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(64, 64);
}

engine::NativeSurface ViewportWidget::surface() {
    const QSize size = pixelSize();
    return engine::NativeSurface{
        .windowHandle = reinterpret_cast<void*>(winId()),
        .width = static_cast<std::uint32_t>(size.width()),
        .height = static_cast<std::uint32_t>(size.height()),
    };
}

void ViewportWidget::setInput(engine::InputSubsystem* input) {
    input_ = input;
}

void ViewportWidget::setCamera(EditorCameraSubsystem* camera) {
    camera_ = camera;
    if (camera_ && hasFocus()) camera_->SetInputActive(true);
}

void ViewportWidget::setPlaying(bool playing) {
    releaseCursor();
    last_cursor_.reset();
    playing_ = playing;
    if (input_) {
        input_->Reset();
        feedMousePosition(*this, input_, mapFromGlobal(QCursor::pos()));
    }
}

QPaintEngine* ViewportWidget::paintEngine() const {
    return nullptr;
}

bool ViewportWidget::event(QEvent* event) {
    // Accepting the override keeps a shortcut (F, Ctrl+A...) from taking the key: it arrives
    // in keyPressEvent instead, and from there at the camera.
    if (event->type() == QEvent::ShortcutOverride && cursor_pinned_) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const QSize size = pixelSize();
    emit surfaceResized(static_cast<quint32>(size.width()), static_cast<quint32>(size.height()));
}

void ViewportWidget::keyPressEvent(QKeyEvent* event) {
    // During Play, Escape hands the keyboard back to the editor instead of going to the game.
    if (playing_ && event->key() == Qt::Key_Escape) {
        releaseCursor();
        clearFocus();
        event->accept();
        return;
    }
    if (input_ != nullptr && !event->isAutoRepeat()) {
        input_->OnKey(toEngineKey(event->key()), true);
        const auto scan = event->nativeScanCode();
        input_->OnPhysicalKey(engine::PhysicalKeyFromWin32Scan(scan & 0xff,
            (scan & 0xe000) == 0xe000 || (scan & 0x100) != 0), true);
    }
    QWidget::keyPressEvent(event);
}

void ViewportWidget::keyReleaseEvent(QKeyEvent* event) {
    if (input_ != nullptr && !event->isAutoRepeat()) {
        input_->OnKey(toEngineKey(event->key()), false);
        const auto scan = event->nativeScanCode();
        input_->OnPhysicalKey(engine::PhysicalKeyFromWin32Scan(scan & 0xff,
            (scan & 0xe000) == 0xe000 || (scan & 0x100) != 0), false);
    }
    QWidget::keyReleaseEvent(event);
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    // Clicking the viewport is how it gets the keyboard, so WASD works right away.
    setFocus(Qt::MouseFocusReason);
    feedMousePosition(*this, input_, event->position());
    last_cursor_ = event->globalPosition().toPoint();
    if (!cursor_pinned_) {
        emit mousePressed(event->position(), event->button(), event->modifiers());
    }
    if (!playing_ && event->button() == Qt::RightButton) {
        cursor_pinned_ = true;
        setCursor(Qt::BlankCursor);
    }
    if (const auto button = toEngineButton(event->button()); input_ != nullptr && button) {
        input_->OnMouseButton(*button, true);
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
    feedMousePosition(*this, input_, event->position());
    if (event->button() == Qt::RightButton) {
        releaseCursor();
    } else if (!cursor_pinned_) {
        emit mouseReleased(event->position(), event->button());
    }
    if (const auto button = toEngineButton(event->button()); input_ != nullptr && button) {
        input_->OnMouseButton(*button, false);
    }
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    feedMousePosition(*this, input_, event->position());
    if (!cursor_pinned_) {
        emit mouseMoved(event->position());
    }
    const QPoint cursor = event->globalPosition().toPoint();
    if (!last_cursor_) {
        last_cursor_ = cursor;
        return;
    }
    const QPoint delta = cursor - *last_cursor_;
    if (input_ != nullptr && !delta.isNull()) {
        const qreal ratio = devicePixelRatioF();
        input_->OnMouseMove(static_cast<float>(delta.x() * ratio), static_cast<float>(delta.y() * ratio));
    }
    if (cursor_pinned_) {
        // The warp produces a move to last_cursor_ itself, i.e. a zero delta.
        QCursor::setPos(*last_cursor_);
    } else {
        last_cursor_ = cursor;
    }
}

void ViewportWidget::wheelEvent(QWheelEvent* event) {
    feedMousePosition(*this, input_, event->position());
    if (input_ != nullptr) {
        input_->OnMouseWheel(static_cast<float>(event->angleDelta().y()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep));
    }
    event->accept();
}

void ViewportWidget::focusOutEvent(QFocusEvent* event) {
    // The release events go to whoever has focus now: drop everything held.
    releaseCursor();
    if (input_ != nullptr) {
        input_->OnFocusLost();
    }
    if (camera_) camera_->SetInputActive(false);
    QWidget::focusOutEvent(event);
}

void ViewportWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    feedMousePosition(*this, input_, mapFromGlobal(QCursor::pos()));
    if (camera_) camera_->SetInputActive(true);
}

void ViewportWidget::leaveEvent(QEvent* event) {
    if (!cursor_pinned_) {
        last_cursor_.reset();
    }
    QWidget::leaveEvent(event);
}

QSize ViewportWidget::pixelSize() const {
    const qreal ratio = devicePixelRatioF();
    return {static_cast<int>(std::lround(width() * ratio)), static_cast<int>(std::lround(height() * ratio))};
}

void ViewportWidget::releaseCursor() {
    if (cursor_pinned_) {
        cursor_pinned_ = false;
        unsetCursor();
    }
}

}  // namespace editor
