#include <engine/input/Win32InputHost.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>
#include <windowsx.h>
#include <imm.h>
#include <engine/engine.hpp>
#include <engine/core/Utf.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32PhysicalKey.hpp>
#include <engine/window.hpp>

#pragma comment(lib, "imm32.lib")

namespace engine {
namespace {
Key Logical(WPARAM key) {
    if (key >= 'A' && key <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + key - 'A');
    if (key >= '0' && key <= '9') return static_cast<Key>(static_cast<int>(Key::Num0) + key - '0');
    if (key >= VK_F1 && key <= VK_F12) return static_cast<Key>(static_cast<int>(Key::F1) + key - VK_F1);
    switch (key) {
    case VK_SPACE: return Key::Space; case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape; case VK_TAB: return Key::Tab;
    case VK_BACK: return Key::Backspace; case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left; case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up; case VK_DOWN: return Key::Down;
    case VK_SHIFT: return Key::Shift; case VK_CONTROL: return Key::Control;
    case VK_MENU: return Key::Alt; default: return Key::Unknown;
    }
}
InputUiNavigationKey Navigation(WPARAM key) {
    switch (key) {
    case VK_HOME: return InputUiNavigationKey::Home; case VK_END: return InputUiNavigationKey::End;
    case VK_PRIOR: return InputUiNavigationKey::PageUp; case VK_NEXT: return InputUiNavigationKey::PageDown;
    case VK_INSERT: return InputUiNavigationKey::Insert; default: return InputUiNavigationKey::None;
    }
}
std::wstring CompositionString(HIMC context, DWORD type) {
    const LONG bytes = ImmGetCompositionStringW(context, type, nullptr, 0);
    if (bytes <= 0 || bytes % sizeof(wchar_t)) return {};
    std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
    ImmGetCompositionStringW(context, type, text.data(), static_cast<DWORD>(bytes));
    return text;
}
}

class Win32InputHost::HostBridge final : public UiHost {
public:
    Win32InputHost* owner = nullptr; // revoked synchronously before the host dies
    UiCaptureState capture;
    UiTextInputState text;
    HCURSOR cursor = LoadCursorW(nullptr, IDC_ARROW);
    HWND Window() const { return owner && IsWindow(owner->window_) ? owner->window_ : nullptr; }
    std::optional<UiSurfaceMetrics> GetSurfaceMetrics() const override {
        return Window() ? owner->GetSurfaceMetrics() : UiSurfaceMetrics{};
    }
    bool ReadClipboard(std::string& utf8, std::string& error) override {
        error.clear();
        const HWND window = Window();
        if (!window || !OpenClipboard(window)) { error = "Cannot open Win32 clipboard"; return false; }
        const HANDLE data = GetClipboardData(CF_UNICODETEXT);
        const auto* value = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
        bool success = value != nullptr;
        if (value) {
            const auto capacity = GlobalSize(data) / sizeof(wchar_t);
            std::size_t size = 0;
            while (size < capacity && value[size]) ++size;
            success = size < capacity;
            if (success) {
                auto converted = utf::ToUtf8(std::wstring_view(value, size));
                success = converted.has_value();
                if (converted) utf8 = std::move(*converted);
            }
            GlobalUnlock(data);
        }
        CloseClipboard();
        if (!success) error = "Clipboard has no valid Unicode text";
        return success;
    }
    bool WriteClipboard(std::string_view utf8, std::string& error) override {
        error.clear();
        const auto converted = utf::ToWide(utf8);
        if (!converted) { error = "Invalid UTF-8 clipboard text"; return false; }
        const auto& value = *converted;
        if (value.size() >= (std::numeric_limits<SIZE_T>::max)() / sizeof(wchar_t)) {
            error = "Clipboard text is too large"; return false;
        }
        const HWND window = Window();
        if (!window || !OpenClipboard(window)) { error = "Cannot open Win32 clipboard"; return false; }
        const auto bytes = (value.size() + 1) * sizeof(wchar_t);
        HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
        auto* target = data ? GlobalLock(data) : nullptr;
        bool success = false;
        if (target) {
            std::memcpy(target, value.c_str(), bytes);
            GlobalUnlock(data);
            success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data);
        }
        if (!success && data) GlobalFree(data);
        CloseClipboard();
        if (!success) error = "Cannot write Win32 clipboard";
        return success;
    }
    void SetCursor(std::string_view name) override {
        LPCWSTR id = IDC_ARROW;
        if (name == "text") id = IDC_IBEAM;
        else if (name == "pointer" || name == "hand") id = IDC_HAND;
        else if (name == "cross") id = IDC_CROSS;
        else if (name == "move") id = IDC_SIZEALL;
        else if (name == "resize" || name == "resize-nwse") id = IDC_SIZENWSE;
        else if (name == "resize-ew") id = IDC_SIZEWE;
        else if (name == "resize-ns") id = IDC_SIZENS;
        else if (name == "unavailable") id = IDC_NO;
        cursor = name == "none" ? nullptr : LoadCursorW(nullptr, id);
        if (Window() && !owner->relativeMouse_) ::SetCursor(cursor);
    }
    void SetTextInput(const UiTextInputState& state) override {
        if (text.active && !state.active) CancelComposition();
        text = state;
        const HWND window = Window();
        if (!window || !state.active) return;
        if (const HIMC context = ImmGetContext(window)) {
            const auto& r = state.caret;
            const LONG x = static_cast<LONG>(std::lround(r.x));
            const LONG y = static_cast<LONG>(std::lround(r.y));
            COMPOSITIONFORM form{};
            form.dwStyle = CFS_POINT;
            form.ptCurrentPos = {x, y + static_cast<LONG>(std::lround(r.height))};
            ImmSetCompositionWindow(context, &form);
            CANDIDATEFORM candidate{};
            candidate.dwStyle = CFS_EXCLUDE;
            candidate.ptCurrentPos = form.ptCurrentPos;
            candidate.rcArea = {x, y, x + static_cast<LONG>(std::lround(r.width)), form.ptCurrentPos.y};
            ImmSetCandidateWindow(context, &candidate);
            ImmReleaseContext(window, context);
        }
    }
    void CancelComposition() override {
        const HWND window = Window();
        if (window) {
            if (const HIMC context = ImmGetContext(window)) {
                ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                ImmReleaseContext(window, context);
            }
        }
        if (owner) { owner->composing_ = false; owner->highSurrogate_ = 0; owner->imeEcho_.clear(); }
    }
    void SetCapture(const UiCaptureState& state) override {
        capture = state;
        if (!Window()) return;
        if (state.pointer || state.wantsVisibleCursor || state.textInput) owner->SetRelativeMouseMode(false);
        if (state.pointer && GetFocus() == Window()) ::SetCapture(Window());
        else if (!owner->relativeMouse_ && GetCapture() == Window()) ReleaseCapture();
    }
};

Win32InputHost::~Win32InputHost() { Shutdown(); }
std::string_view Win32InputHost::GetName() const { return "Win32InputHost"; }
std::shared_ptr<UiHost> Win32InputHost::GetUiHost() {
    if (!uiHost_) { uiHost_ = std::make_shared<HostBridge>(); uiHost_->owner = this; }
    return uiHost_;
}
UiSurfaceMetrics Win32InputHost::GetSurfaceMetrics() const {
    RECT rect{};
    if (!IsWindow(window_) || !GetClientRect(window_, &rect)) return {};
    if (IsIconic(window_)) rect = {};
    const UINT dpi = GetDpiForWindow(window_);
    return {static_cast<std::uint32_t>((std::max)(0L, rect.right)),
            static_cast<std::uint32_t>((std::max)(0L, rect.bottom)), dpi ? dpi / 96.0f : 1.0f};
}
bool Win32InputHost::Initialize() {
    input_ = GetEngine().GetSubsystem<InputSubsystem>();
    if (!IsWindow(window_) || !input_) return false;
    (void)GetUiHost();
    uiHost_->owner = this;
    focused_ = GetFocus() == window_;
    SetInputMessageHandler(window_, [this](HWND w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r) {
        return HandleMessage(w, m, wp, lp, r);
    });
    InputUiEvent event;
    event.payload = InputUiFocus{focused_};
    Queue(std::move(event));
    return true;
}
void Win32InputHost::SetRelativeMouseMode(bool enabled) {
    if (enabled && uiHost_ && (uiHost_->capture.pointer || uiHost_->capture.wantsVisibleCursor || uiHost_->capture.textInput)) return;
    if (enabled == relativeMouse_) return;
    if (enabled) {
        if (GetForegroundWindow() != window_) return;
        ::SetCapture(window_);
        while (ShowCursor(FALSE) >= 0) {}
        relativeMouse_ = true;
        RECT client{};
        GetClientRect(window_, &client);
        POINT center{client.right / 2, client.bottom / 2};
        ClientToScreen(window_, &center);
        SetCursorPos(center.x, center.y);
    } else {
        relativeMouse_ = false;
        if (GetCapture() == window_) ReleaseCapture();
        while (ShowCursor(TRUE) < 0) {}
    }
    hasCursorPosition_ = false;
}
void Win32InputHost::Shutdown() {
    SetInputMessageHandler(window_, {});
    if (uiHost_ && uiHost_->owner) { uiHost_->CancelComposition(); uiHost_->owner = nullptr; }
    if (GetCapture() == window_) ReleaseCapture();
    SetRelativeMouseMode(false);
    input_ = nullptr;
    focused_ = false;
    highSurrogate_ = 0;
    imeEcho_.clear();
}
void Win32InputHost::Queue(InputUiEvent event) {
    if (!input_) return;
    event.realTimeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto down = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
    event.modifiers = {down(VK_SHIFT), down(VK_CONTROL), down(VK_MENU), down(VK_LWIN) || down(VK_RWIN),
                      (GetKeyState(VK_CAPITAL) & 1) != 0, (GetKeyState(VK_NUMLOCK) & 1) != 0};
    if (!event.hasPointerPosition) {
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(window_, &p)) {
            event.pointerX = static_cast<float>(p.x); event.pointerY = static_cast<float>(p.y);
            event.hasPointerPosition = true;
        }
    }
    input_->OnUiEvent(std::move(event));
}
void Win32InputHost::TextUnit(wchar_t unit) {
    if (!imeEcho_.empty()) {
        if (imeEcho_.front() == unit) { imeEcho_.erase(0, 1); return; }
        imeEcho_.clear();
    }
    if (unit >= 0xd800 && unit <= 0xdbff) { highSurrogate_ = unit; return; }
    std::wstring text;
    if (unit >= 0xdc00 && unit <= 0xdfff) {
        if (!highSurrogate_) return;
        text.push_back(highSurrogate_);
    }
    highSurrogate_ = 0;
    if (unit < 0x20 || unit == 0x7f) return;
    text.push_back(unit);
    InputUiEvent event;
    auto converted = utf::ToUtf8(text);
    if (!converted) return;
    event.payload = InputUiText{std::move(*converted)};
    Queue(std::move(event));
}

bool Win32InputHost::HandleMessage(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!input_) return false;
    InputUiEvent event;
    switch (message) {
    case WM_NCDESTROY:
        if (focused_) input_->OnFocusLost();
        focused_ = false;
        SetRelativeMouseMode(false);
        uiHost_->CancelComposition();
        uiHost_->owner = nullptr;
        window_ = nullptr;
        break;
    case WM_SETFOCUS:
        focused_ = true; hasCursorPosition_ = false;
        event.payload = InputUiFocus{true}; Queue(std::move(event)); break;
    case WM_KILLFOCUS:
        focused_ = false; hasCursorPosition_ = false;
        SetRelativeMouseMode(false);
        if (GetCapture() == window) ReleaseCapture();
        uiHost_->CancelComposition();
        input_->OnFocusLost();
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        if (!focused_) break;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (down && wp != VK_PROCESSKEY) imeEcho_.clear();
        const auto bits = static_cast<unsigned long long>(lp);
        const auto physical = PhysicalKeyFromWin32Scan((bits >> 16) & 0xff, (bits & 0x01000000) != 0);
        const auto logical = Logical(wp);
        input_->OnPhysicalKey(physical, down); input_->OnKey(logical, down);
        const unsigned count = down ? (std::max)(1u, static_cast<unsigned>(LOWORD(lp))) : 1u;
        for (unsigned i = 0; i < count; ++i) {
            event.payload = InputUiKey{logical, physical, Navigation(wp), down, down && ((bits & 0x40000000) || i > 0)};
            Queue(event);
        }
        break;
    }
    case WM_CHAR: case WM_SYSCHAR:
        if (focused_) for (unsigned i = 0; i < (std::max)(1u, static_cast<unsigned>(LOWORD(lp))); ++i) TextUnit(static_cast<wchar_t>(wp));
        result = 0; return true;
    case WM_UNICHAR:
        if (wp == UNICODE_NOCHAR) { result = TRUE; return true; }
        if (focused_ && wp <= 0x10ffff && !(wp >= 0xd800 && wp <= 0xdfff)) {
            if (wp > 0xffff) { TextUnit(static_cast<wchar_t>(0xd800 + ((wp - 0x10000) >> 10))); TextUnit(static_cast<wchar_t>(0xdc00 + ((wp - 0x10000) & 0x3ff))); }
            else TextUnit(static_cast<wchar_t>(wp));
        }
        result = 0; return true;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        if (!focused_) break;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ScreenToClient(window, &point);
        event.pointerX = static_cast<float>(point.x); event.pointerY = static_cast<float>(point.y);
        event.hasPointerPosition = true;
        input_->OnMousePosition(event.pointerX, event.pointerY);
        if (message == WM_MOUSEMOVE) event.payload = InputUiPointerMove{};
        else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            event.payload = InputUiWheel{message == WM_MOUSEHWHEEL ? delta : 0, message == WM_MOUSEWHEEL ? delta : 0};
            if (message == WM_MOUSEWHEEL) input_->OnMouseWheel(delta);
        } else {
            const auto button = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ? MouseButton::Left :
                message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ? MouseButton::Right : MouseButton::Middle;
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
            input_->OnMouseButton(button, down); event.payload = InputUiPointerButton{button, down};
        }
        Queue(std::move(event)); break;
    }
    case WM_IME_STARTCOMPOSITION:
        if (!focused_) break;
        composing_ = true; highSurrogate_ = 0; imeEcho_.clear();
        event.payload = InputUiComposition{}; Queue(std::move(event));
        result = 0; return true;
    case WM_IME_COMPOSITION: {
        if (!focused_) break;
        if (!lp && composing_) {
            InputUiComposition composition;
            composition.phase = InputUiCompositionPhase::Cancel;
            event.payload = std::move(composition); Queue(std::move(event));
            composing_ = false;
            result = 0; return true;
        }
        const HIMC context = ImmGetContext(window);
        if (!context) break;
        if (lp & GCS_RESULTSTR) {
            const auto text = CompositionString(context, GCS_RESULTSTR);
            auto converted = utf::ToUtf8(text);
            if (!converted) { ImmReleaseContext(window, context); result = 0; return true; }
            InputUiComposition composition;
            composition.phase = InputUiCompositionPhase::Commit; composition.utf8 = std::move(*converted);
            event.payload = std::move(composition); Queue(event);
            // Returning handled prevents DefWindowProc from generating WM_IME_CHAR;
            // also discard legacy IMEs' echoed WM_CHAR units, if supplied.
            imeEcho_ = text;
            composing_ = false;
        }
        if (lp & GCS_COMPSTR) {
            const auto text = CompositionString(context, GCS_COMPSTR);
            auto converted = utf::ToUtf8(text);
            if (!converted) { ImmReleaseContext(window, context); result = 0; return true; }
            InputUiComposition composition;
            composition.phase = InputUiCompositionPhase::Update; composition.utf8 = std::move(*converted);
            const LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
            composition.cursor = static_cast<std::uint32_t>(utf::Utf16ScalarOffset(text, cursor < 0 ? 0 : static_cast<std::size_t>(cursor)));
            const LONG attrBytes = ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0);
            std::vector<BYTE> attributes(attrBytes > 0 ? static_cast<std::size_t>(attrBytes) : 0);
            if (!attributes.empty()) ImmGetCompositionStringW(context, GCS_COMPATTR, attributes.data(), static_cast<DWORD>(attributes.size()));
            std::size_t start = 0;
            while (start < attributes.size() && attributes[start] != ATTR_TARGET_CONVERTED && attributes[start] != ATTR_TARGET_NOTCONVERTED) ++start;
            std::size_t end = start;
            while (end < attributes.size() && (attributes[end] == ATTR_TARGET_CONVERTED || attributes[end] == ATTR_TARGET_NOTCONVERTED)) ++end;
            composition.selectionStart = static_cast<std::uint32_t>(utf::Utf16ScalarOffset(text, start));
            composition.selectionLength = static_cast<std::uint32_t>(utf::Utf16ScalarOffset(text, end)) - composition.selectionStart;
            const LONG clauseBytes = ImmGetCompositionStringW(context, GCS_COMPCLAUSE, nullptr, 0);
            std::vector<DWORD> clauses(clauseBytes > 0 ? static_cast<std::size_t>(clauseBytes) / sizeof(DWORD) : 0);
            if (!clauses.empty()) ImmGetCompositionStringW(context, GCS_COMPCLAUSE, clauses.data(), static_cast<DWORD>(clauses.size() * sizeof(DWORD)));
            for (std::size_t i = 1; i < clauses.size(); ++i) {
                const auto a = (std::min)(static_cast<std::size_t>(clauses[i - 1]), text.size());
                const auto b = (std::min)(static_cast<std::size_t>(clauses[i]), text.size());
                if (b < a) continue;
                const auto scalarStart = static_cast<std::uint32_t>(utf::Utf16ScalarOffset(text, a));
                const auto scalarEnd = static_cast<std::uint32_t>(utf::Utf16ScalarOffset(text, b));
                composition.clauses.push_back({scalarStart, scalarEnd - scalarStart, a < end && b > start});
            }
            event.payload = std::move(composition); Queue(std::move(event)); composing_ = true;
        }
        ImmReleaseContext(window, context);
        result = 0; return true;
    }
    case WM_IME_ENDCOMPOSITION:
        if (composing_) { InputUiComposition c; c.phase = InputUiCompositionPhase::Cancel; event.payload = std::move(c); Queue(std::move(event)); }
        composing_ = false;
        result = 0; return true;
    case WM_IME_CHAR: result = 0; return true;
    case WM_IME_REQUEST:
        if (uiHost_->text.active && wp == IMR_QUERYCHARPOSITION && lp) {
            auto* position = reinterpret_cast<IMECHARPOSITION*>(lp);
            if (position->dwSize < sizeof(IMECHARPOSITION) || position->dwCharPos != 0) {
                result = FALSE; return true;
            }
            const auto& caret = uiHost_->text.caret;
            position->pt = {static_cast<LONG>(std::lround(caret.x)), static_cast<LONG>(std::lround(caret.y))};
            ClientToScreen(window, &position->pt);
            position->cLineHeight = static_cast<UINT>((std::max)(1L, static_cast<LONG>(std::lround(caret.height))));
            GetClientRect(window, &position->rcDocument);
            MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&position->rcDocument), 2);
            result = TRUE; return true;
        }
        if (uiHost_->text.active && wp == IMR_DOCUMENTFEED) {
            const auto& snapshot = uiHost_->text;
            const auto converted = utf::ToWide(snapshot.surroundingText);
            if (!converted) { result = 0; return true; }
            const auto& surrounding = *converted;
            // RECONVERTSTRING sizes and byte offsets are DWORDs.
            if (surrounding.size() > ((std::numeric_limits<DWORD>::max)() - sizeof(RECONVERTSTRING)) / sizeof(wchar_t) - 1) {
                result = 0; return true;
            }
            const auto bytes = sizeof(RECONVERTSTRING) + (surrounding.size() + 1) * sizeof(wchar_t);
            result = static_cast<LRESULT>(bytes);
            if (!lp) return true;
            auto* reconvert = reinterpret_cast<RECONVERTSTRING*>(lp);
            if (reconvert->dwSize < bytes) { result = 0; return true; }
            const auto start = utf::Utf16UnitOffset(surrounding, snapshot.selectionLength ? snapshot.selectionStart : snapshot.cursor);
            const auto available = utf::Utf16ScalarOffset(surrounding, surrounding.size());
            const auto selectionStart = (std::min)(available, static_cast<std::size_t>(snapshot.selectionStart));
            const auto selectionEnd = selectionStart + (std::min)(static_cast<std::size_t>(snapshot.selectionLength), available - selectionStart);
            const auto end = snapshot.selectionLength ? utf::Utf16UnitOffset(surrounding, selectionEnd) : start;
            reconvert->dwVersion = 0;
            reconvert->dwStrLen = static_cast<DWORD>(surrounding.size());
            reconvert->dwStrOffset = sizeof(RECONVERTSTRING);
            reconvert->dwCompStrLen = reconvert->dwTargetStrLen = static_cast<DWORD>(end - start);
            reconvert->dwCompStrOffset = reconvert->dwTargetStrOffset = static_cast<DWORD>(start * sizeof(wchar_t));
            std::memcpy(reinterpret_cast<BYTE*>(reconvert) + sizeof(RECONVERTSTRING), surrounding.c_str(),
                        (surrounding.size() + 1) * sizeof(wchar_t));
            return true;
        }
        break;
    case WM_IME_SETCONTEXT:
        result = DefWindowProcW(window, message, wp, lp & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
        return true;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && !relativeMouse_) { ::SetCursor(uiHost_->cursor); result = TRUE; return true; }
        break;
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    }
    }
    return false;
}

void Win32InputHost::Update(const FrameContext&) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { GetEngine().RequestExit(); return; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!IsWindow(window_) || GetForegroundWindow() != window_) {
        if (focused_) { focused_ = false; input_->OnFocusLost(); uiHost_->CancelComposition(); }
        SetRelativeMouseMode(false); hasCursorPosition_ = false; return;
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    if (relativeMouse_) {
        RECT client{}; GetClientRect(window_, &client);
        POINT center{client.right / 2, client.bottom / 2}; ClientToScreen(window_, &center);
        input_->OnMouseMove(static_cast<float>(cursor.x - center.x), static_cast<float>(cursor.y - center.y));
        SetCursorPos(center.x, center.y);
    } else {
        if (hasCursorPosition_) input_->OnMouseMove(static_cast<float>(cursor.x - lastCursor_.x), static_cast<float>(cursor.y - lastCursor_.y));
        lastCursor_ = cursor; hasCursorPosition_ = true;
    }
}
} // namespace engine
