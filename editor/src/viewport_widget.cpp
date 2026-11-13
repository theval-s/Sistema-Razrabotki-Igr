#include <editor/viewport_widget.hpp>

#include <QAction>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QPointer>
#include <QTextCharFormat>
#include <chrono>
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <engine/input/InputSubsystem.hpp>
#include <engine/core/Utf.hpp>
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
        case Qt::Key_Tab:
        case Qt::Key_Backtab: return Key::Tab;
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

// The engine owns this adapter; the native widget may disappear first.
class ViewportWidget::HostBridge final : public engine::UiHost {
public:
    explicit HostBridge(ViewportWidget* widget) : widget_(widget) {}
    bool ReadClipboard(std::string& text, std::string& error) override {
        if (!widget_) { error = "Viewport is unavailable"; return false; }
        text = QGuiApplication::clipboard()->text().toUtf8().toStdString();
        error.clear(); return true;
    }
    bool WriteClipboard(std::string_view text, std::string& error) override {
        if (!widget_) { error = "Viewport is unavailable"; return false; }
        QGuiApplication::clipboard()->setText(QString::fromUtf8(text.data(), text.size()));
        error.clear(); return true;
    }
    void SetCursor(std::string_view name) override {
        if (!widget_) return;
        auto shape = Qt::ArrowCursor;
        if (name == "text") shape = Qt::IBeamCursor;
        else if (name == "pointer" || name == "hand") shape = Qt::PointingHandCursor;
        else if (name == "cross") shape = Qt::CrossCursor;
        else if (name == "move") shape = Qt::SizeAllCursor;
        else if (name == "resize" || name == "resize-nwse") shape = Qt::SizeFDiagCursor;
        else if (name == "resize-nesw") shape = Qt::SizeBDiagCursor;
        else if (name == "resize-ew") shape = Qt::SizeHorCursor;
        else if (name == "resize-ns") shape = Qt::SizeVerCursor;
        else if (name == "unavailable") shape = Qt::ForbiddenCursor;
        else if (name == "none") shape = Qt::BlankCursor;
        widget_->ui_cursor_ = shape;
        widget_->applyUiCapture();
    }
    void SetTextInput(const engine::UiTextInputState& state) override {
        if (!widget_) return;
        if (widget_->text_input_.active && !state.active) widget_->cancelComposition();
        widget_->text_input_ = state;
        widget_->setAttribute(Qt::WA_InputMethodEnabled, state.active && widget_->captured_);
        if (widget_->hasFocus()) QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
    void CancelComposition() override { if (widget_) widget_->cancelComposition(); }
    void SetCapture(const engine::UiCaptureState& state) override {
        if (!widget_) return;
        widget_->ui_capture_ = state;
        widget_->applyUiCapture();
    }
private:
    QPointer<ViewportWidget> widget_;
};

std::shared_ptr<engine::UiHost> ViewportWidget::uiHost() {
    if (!ui_host_) ui_host_ = std::make_shared<HostBridge>(this);
    return ui_host_;
}

engine::UiSurfaceMetrics ViewportWidget::uiMetrics() const {
    const auto size = pixelSize();
    return {static_cast<std::uint32_t>(size.width()), static_cast<std::uint32_t>(size.height()),
            static_cast<float>(devicePixelRatioF())};
}

void ViewportWidget::applyUiCapture() {
    const bool active = playing_ && captured_ && hasFocus();
    if (active && (ui_capture_.pointer || ui_capture_.wantsVisibleCursor || ui_capture_.textInput)) releaseCursor();
    if (active && ui_capture_.pointer && !ui_mouse_grab_) {
        grabMouse(); ui_mouse_grab_ = true;
    } else if ((!active || !ui_capture_.pointer) && ui_mouse_grab_) {
        if (mouseGrabber() == this) releaseMouse();
        ui_mouse_grab_ = false;
    }
    if (active) setCursor(ui_capture_.wantsVisibleCursor && ui_cursor_ == Qt::BlankCursor ? Qt::ArrowCursor : ui_cursor_);
    else if (!cursor_pinned_) unsetCursor();
}

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
}

void ViewportWidget::captureInput() {
    if (!playing_ || captured_) return;
    captured_ = true;
    setFocus(Qt::OtherFocusReason);
    queueUi({.payload = engine::InputUiFocus{true}});
    setAttribute(Qt::WA_InputMethodEnabled, text_input_.active);
    applyUiCapture();
    feedMousePosition(mapFromGlobal(QCursor::pos()));
    emit inputCaptureChanged(true);
}

void ViewportWidget::releaseInput() {
    if (!captured_) return;
    captured_ = false;
    cancelComposition();
    setAttribute(Qt::WA_InputMethodEnabled, false);
    applyUiCapture();
    swallowed_buttons_ = {};
    releaseCursor();
    last_cursor_.reset();
    // The game would never see the release of what is held now.
    if (input_ != nullptr) input_->OnFocusLost();
    emit inputCaptureChanged(false);
}

QPaintEngine* ViewportWidget::paintEngine() const {
    return nullptr;
}

bool ViewportWidget::event(QEvent* event) {
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal) {
        const bool handled = QWidget::event(event);
        const auto size = pixelSize();
        emit surfaceResized(static_cast<quint32>(size.width()), static_cast<quint32>(size.height()));
        if (hasFocus() && text_input_.active) QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
        return handled;
    }
    // QWidget otherwise consumes Tab before keyPressEvent for dock focus traversal.
    if (captured_ && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) && !isPlayShortcut(*key)) {
            if (event->type() == QEvent::KeyPress) keyPressEvent(key); else keyReleaseEvent(key);
            return true;
        }
    }
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
    if (captured_ && isPlayShortcut(*event)) { event->ignore(); return; }
    feedKey(event, true);
    if (captured_) { event->accept(); return; }
    QWidget::keyPressEvent(event);
}

void ViewportWidget::keyReleaseEvent(QKeyEvent* event) {
    if (captured_ && isPlayShortcut(*event)) { event->ignore(); return; }
    feedKey(event, false);
    if (captured_) { event->accept(); return; }
    QWidget::keyReleaseEvent(event);
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    // Clicking the viewport is how it gets the keyboard, so WASD works right away.
    setFocus(Qt::MouseFocusReason);
    last_cursor_ = event->globalPosition().toPoint();
    if (playing_ && !captured_) {
        // Reacquisition is focus-only: neither UI nor gameplay gets this press/release.
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
    if (const auto button = toEngineButton(event->button()); feedsEngine() && button) {
        input_->OnMouseButton(*button, true);
        queueUi({.payload = engine::InputUiPointerButton{*button, true}}, event->modifiers(), event->position());
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (swallowed_buttons_.testFlag(event->button())) {
        swallowed_buttons_.setFlag(event->button(), false);
        return;
    }
    if (playing_ && !captured_) return;
    feedMousePosition(event->position());
    if (event->button() == Qt::RightButton) {
        releaseCursor();
    } else if (!playing_ && !cursor_pinned_) {
        emit mouseReleased(event->position(), event->button());
    }
    if (const auto button = toEngineButton(event->button()); input_ != nullptr && button) {
        input_->OnMouseButton(*button, false);
        queueUi({.payload = engine::InputUiPointerButton{*button, false}}, event->modifiers(), event->position());
    }
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    if (playing_ && !captured_) return;
    feedMousePosition(event->position());
    queueUi({.payload = engine::InputUiPointerMove{}}, event->modifiers(), event->position());
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
        queueUi({.payload = engine::InputUiWheel{event->angleDelta().x() / 120.0f,
            event->angleDelta().y() / 120.0f}}, event->modifiers(), event->position());
    }
    event->accept();
}

void ViewportWidget::focusOutEvent(QFocusEvent* event) {
    // The release events go to whoever has focus now: drop everything held.
    const bool wasCaptured = captured_;
    releaseInput();
    releaseCursor();
    if (input_ != nullptr && !wasCaptured) {
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

namespace {
// Qt offsets count UTF-16 units; the engine boundary counts Unicode scalars.
std::u16string_view utf16View(const QString& text) {
    return {QStringView(text).utf16(), static_cast<std::size_t>(text.size())};
}
int units(const QString& text, std::uint64_t scalars) {
    const auto offset = engine::utf::Utf16UnitOffset(utf16View(text),
        static_cast<std::size_t>((std::min)(scalars, static_cast<std::uint64_t>(text.size()))));
    return static_cast<int>((std::min)(offset, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}
std::uint32_t scalars(const QString& text, qsizetype offset) {
    const auto count = engine::utf::Utf16ScalarOffset(utf16View(text), static_cast<std::size_t>((std::max)(qsizetype{0}, offset)));
    return static_cast<std::uint32_t>((std::min)(count, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}
}

void ViewportWidget::queueUi(engine::InputUiEvent event, Qt::KeyboardModifiers modifiers,
                             std::optional<QPointF> position) {
    if (!input_ || !playing_ || !captured_) return;
    event.realTimeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    event.modifiers = {modifiers.testFlag(Qt::ShiftModifier), modifiers.testFlag(Qt::ControlModifier),
                      modifiers.testFlag(Qt::AltModifier), modifiers.testFlag(Qt::MetaModifier),
                      (GetKeyState(VK_CAPITAL) & 1) != 0, (GetKeyState(VK_NUMLOCK) & 1) != 0};
    if (position) {
        event.hasPointerPosition = true;
        event.pointerX = static_cast<float>(position->x() * devicePixelRatioF());
        event.pointerY = static_cast<float>(position->y() * devicePixelRatioF());
    }
    input_->OnUiEvent(std::move(event));
}

void ViewportWidget::feedKey(QKeyEvent* event, bool down) {
    if (!feedsEngine()) return;
    const auto scan = event->nativeScanCode();
    const auto physical = engine::PhysicalKeyFromWin32Scan(scan & 0xff,
        (scan & 0xe000) == 0xe000 || (scan & 0x100) != 0);
    const auto logical = toEngineKey(event->key());
    if (!event->isAutoRepeat()) {
        input_->OnKey(logical, down);
        input_->OnPhysicalKey(physical, down);
    }
    // Qt can produce synthetic repeat releases. They are not physical/UI key-up.
    if (!down && event->isAutoRepeat()) return;
    auto navigation = engine::InputUiNavigationKey::None;
    switch (event->key()) {
    case Qt::Key_Home: navigation = engine::InputUiNavigationKey::Home; break;
    case Qt::Key_End: navigation = engine::InputUiNavigationKey::End; break;
    case Qt::Key_PageUp: navigation = engine::InputUiNavigationKey::PageUp; break;
    case Qt::Key_PageDown: navigation = engine::InputUiNavigationKey::PageDown; break;
    case Qt::Key_Insert: navigation = engine::InputUiNavigationKey::Insert; break;
    }
    auto modifiers = event->modifiers();
    if (event->key() == Qt::Key_Backtab) modifiers |= Qt::ShiftModifier;
    queueUi({.payload = engine::InputUiKey{logical, physical, navigation, down, event->isAutoRepeat()}}, modifiers);
    // IME commits arrive exclusively through inputMethodEvent. AltGr text remains valid.
    if (down && !composing_ && !modifiers.testFlag(Qt::MetaModifier) &&
        (!modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::AltModifier))) {
        QString text;
        for (const auto cp : event->text().toUcs4())
            if (cp >= 0x20 && cp != 0x7f) text += QString::fromUcs4(&cp, 1);
        if (!text.isEmpty()) queueUi({.payload = engine::InputUiText{text.toUtf8().toStdString()}}, modifiers);
    }
}

void ViewportWidget::cancelComposition() {
    composing_ = false;
    if (resetting_ime_) return;
    resetting_ime_ = true;
    if (hasFocus()) QGuiApplication::inputMethod()->reset();
    resetting_ime_ = false;
}

QVariant ViewportWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    const auto& state = text_input_;
    const auto text = QString::fromUtf8(state.surroundingText.data(), state.surroundingText.size());
    const int cursor = units(text, state.cursor);
    const int start = units(text, state.selectionStart);
    const int end = units(text, std::uint64_t(state.selectionStart) + state.selectionLength);
    switch (query) {
    case Qt::ImEnabled: return captured_ && state.active;
    case Qt::ImCursorRectangle: {
        const auto& r = state.caret;
        const auto ratio = devicePixelRatioF();
        return QRectF(r.x / ratio, r.y / ratio, r.width / ratio, r.height / ratio);
    }
    case Qt::ImSurroundingText: return text;
    case Qt::ImCursorPosition:
    case Qt::ImAbsolutePosition: return cursor;
    case Qt::ImAnchorPosition: return state.selectionLength ? (cursor == start ? end : start) : cursor;
    case Qt::ImCurrentSelection: return text.mid(start, end - start);
    case Qt::ImTextBeforeCursor: return text.left(cursor);
    case Qt::ImTextAfterCursor: return text.mid(cursor);
    case Qt::ImHints: return int(Qt::ImhNone);
    default: return QWidget::inputMethodQuery(query);
    }
}

void ViewportWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (resetting_ime_ || !captured_ || !text_input_.active) { event->accept(); return; }
    const auto modifiers = QGuiApplication::keyboardModifiers();
    const auto& preedit = event->preeditString();
    const bool commit = !event->commitString().isEmpty() || event->replacementLength() != 0;
    if (commit) {
        engine::InputUiComposition c;
        c.phase = engine::InputUiCompositionPhase::Commit;
        c.utf8 = event->commitString().toUtf8().toStdString();
        const auto text = QString::fromUtf8(text_input_.surroundingText.data(), text_input_.surroundingText.size());
        const int cursor = units(text, text_input_.cursor);
        const auto start = std::clamp<qint64>(qint64(cursor) + event->replacementStart(), 0, text.size());
        const auto end = std::clamp<qint64>(start + event->replacementLength(), start, text.size());
        c.replacementStart = static_cast<std::int32_t>(scalars(text, start)) -
                             static_cast<std::int32_t>(scalars(text, cursor));
        c.replacementLength = scalars(text, end) - scalars(text, start);
        queueUi({.payload = std::move(c)}, modifiers);
        composing_ = false;
    }
    if (!preedit.isEmpty()) {
        if (!composing_) queueUi({.payload = engine::InputUiComposition{}}, modifiers);
        engine::InputUiComposition c;
        c.phase = engine::InputUiCompositionPhase::Update;
        c.utf8 = preedit.toUtf8().toStdString();
        c.cursor = scalars(preedit, preedit.size());
        for (const auto& attribute : event->attributes()) {
            const auto start = scalars(preedit, attribute.start);
            const auto end = scalars(preedit, std::clamp<qint64>(
                qint64(attribute.start) + attribute.length, 0, preedit.size()));
            if (attribute.type == QInputMethodEvent::Cursor) c.cursor = start;
            if (attribute.type == QInputMethodEvent::TextFormat && end >= start) {
                const auto format = qvariant_cast<QTextFormat>(attribute.value).toCharFormat();
                const bool selected = format.background().style() != Qt::NoBrush;
                c.clauses.push_back({start, end - start, selected});
                if (selected) { c.selectionStart = start; c.selectionLength = end - start; }
            }
        }
        queueUi({.payload = std::move(c)}, modifiers);
        composing_ = true;
    } else if (composing_) {
        engine::InputUiComposition c;
        c.phase = engine::InputUiCompositionPhase::Cancel;
        queueUi({.payload = std::move(c)}, modifiers);
        composing_ = false;
    }
    event->accept();
}

}  // namespace editor
