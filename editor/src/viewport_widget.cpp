#include <editor/viewport_widget.hpp>

#include <QAction>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32PhysicalKey.hpp>
#include <editor/editor_camera.hpp>
#include <editor/shortcut_registry.hpp>

namespace editor {

namespace {

/// Bound by the Scripting module's play controls.
const QString PlayShortcutIds[] = {QStringLiteral("play.stop"), QStringLiteral("play.pause")};

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
    if (camera_ && hasFocus() && !playing_) camera_->SetInputActive(true);
}

void ViewportWidget::setShortcuts(ShortcutRegistry& shortcuts) {
    shortcuts_ = &shortcuts;
    // Only a handle for the registry: Qt never triggers it, keyPressEvent matches it while the
    // game has the keyboard. So Esc can still deselect while editing.
    auto* release = new QAction(tr("Release Game Input"), this);
    release->setShortcutContext(Qt::WidgetShortcut);
    shortcuts.bind(release, ReleaseInputId, tr("Play"), QKeySequence(Qt::Key_Escape));
}

void ViewportWidget::setPlaying(bool playing) {
    releaseInput();
    releaseCursor();
    last_cursor_.reset();
    playing_ = playing;
    paused_ = false;
    // During Play the view is the game's.
    if (camera_) camera_->SetInputActive(!playing && hasFocus());
    if (input_) {
        input_->Reset();
        feedMousePosition(mapFromGlobal(QCursor::pos()));
    }
    if (playing) captureInput();
}

void ViewportWidget::setPaused(const bool paused) {
    paused_ = paused;
    if (paused) {
        releaseInput();
    } else {
        captureInput();
    }
}

void ViewportWidget::captureInput() {
    if (!playing_ || paused_ || captured_) return;
    captured_ = true;
    setFocus(Qt::OtherFocusReason);
    feedMousePosition(mapFromGlobal(QCursor::pos()));
    applyCursorMode();
    emit inputCaptureChanged(true);
}

void ViewportWidget::releaseInput() {
    if (!captured_) return;
    captured_ = false;
    swallowed_buttons_ = {};
    applyCursorMode();
    last_cursor_.reset();
    // The game would never see the release of what is held now.
    if (input_ != nullptr) input_->OnFocusLost();
    emit inputCaptureChanged(false);
}

void ViewportWidget::applyCursorMode() {
    using engine::CursorMode;
    const CursorMode mode = captured_ && input_ != nullptr ? input_->GetCursorMode() : CursorMode::Normal;
    if (mode == cursor_mode_) return;
    if (cursor_mode_ == CursorMode::Locked) releaseMouse();
    cursor_mode_ = mode;
    cursor_pinned_ = mode == CursorMode::Locked;
    if (mode == CursorMode::Normal) {
        unsetCursor();
        return;
    }
    setCursor(Qt::BlankCursor);
    if (cursor_pinned_) {
        last_cursor_ = mapToGlobal(rect().center());
        QCursor::setPos(*last_cursor_);
        // Grabbed, so a fast flick can't leave the viewport between two moves.
        grabMouse();
    }
}

QPaintEngine* ViewportWidget::paintEngine() const {
    return nullptr;
}

bool ViewportWidget::event(QEvent* event) {
    // Accepting the override keeps a shortcut (F, Ctrl+A...) from taking the key: it arrives
    // in keyPressEvent instead, and from there at the camera, or at the game while it has input.
    if (event->type() == QEvent::ShortcutOverride &&
        (cursor_pinned_ || (captured_ && !isPlayShortcut(*static_cast<QKeyEvent*>(event))))) {
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
    if (captured_ && shortcuts_ != nullptr && shortcuts_->matches(ReleaseInputId, *event)) {
        releaseInput();
        event->accept();
        return;
    }
    if (feedsEngine() && !event->isAutoRepeat()) {
        input_->OnKey(toEngineKey(event->key()), true);
        const auto scan = event->nativeScanCode();
        input_->OnPhysicalKey(engine::PhysicalKeyFromWin32Scan(scan & 0xff,
            (scan & 0xe000) == 0xe000 || (scan & 0x100) != 0), true);
    }
    QWidget::keyPressEvent(event);
}

void ViewportWidget::keyReleaseEvent(QKeyEvent* event) {
    if (feedsEngine() && !event->isAutoRepeat()) {
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
    last_cursor_ = event->globalPosition().toPoint();
    if (playing_ && !captured_) {
        // The click only hands the input back, so the game never sees a stray shot.
        swallowed_buttons_ |= event->button();
        captureInput();
        return;
    }
    feedMousePosition(event->position());
    if (!playing_ && !cursor_pinned_) {
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
    if (swallowed_buttons_.testFlag(event->button())) {
        swallowed_buttons_.setFlag(event->button(), false);
        return;
    }
    if (playing_ && !captured_) return;
    feedMousePosition(event->position());
    if (!playing_ && event->button() == Qt::RightButton) {
        releaseCursor();
    } else if (!playing_ && !cursor_pinned_) {
        emit mouseReleased(event->position(), event->button());
    }
    if (const auto button = toEngineButton(event->button()); input_ != nullptr && button) {
        input_->OnMouseButton(*button, false);
    }
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    if (playing_ && !captured_) return;
    feedMousePosition(event->position());
    if (!playing_ && !cursor_pinned_) {
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
    feedMousePosition(event->position());
    if (feedsEngine()) {
        input_->OnMouseWheel(static_cast<float>(event->angleDelta().y()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep));
    }
    event->accept();
}

void ViewportWidget::focusOutEvent(QFocusEvent* event) {
    // The release events go to whoever has focus now: drop everything held.
    releaseInput();
    releaseCursor();
    if (input_ != nullptr) {
        input_->OnFocusLost();
    }
    if (camera_) camera_->SetInputActive(false);
    QWidget::focusOutEvent(event);
}

void ViewportWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    feedMousePosition(mapFromGlobal(QCursor::pos()));
    if (camera_ && !playing_) camera_->SetInputActive(true);
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

void ViewportWidget::feedMousePosition(const QPointF position) {
    if (feedsEngine() && hasFocus()) {
        const qreal ratio = devicePixelRatioF();
        input_->OnMousePosition(static_cast<float>(position.x() * ratio), static_cast<float>(position.y() * ratio));
    }
}

bool ViewportWidget::isPlayShortcut(const QKeyEvent& event) const {
    return shortcuts_ != nullptr &&
           std::ranges::any_of(PlayShortcutIds, [&](const QString& id) { return shortcuts_->matches(id, event); });
}

void ViewportWidget::releaseCursor() {
    if (cursor_pinned_) {
        cursor_pinned_ = false;
        unsetCursor();
    }
}

}  // namespace editor
