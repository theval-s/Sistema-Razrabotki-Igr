#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/Uuid.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32InputHost.hpp>
#include <engine/window.hpp>
#include <gtest/gtest.h>

namespace {
using namespace engine;

struct RootScope {
    std::filesystem::path previous = AssetPathResolver::GetGameRoot();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-native-input-" + NewUuid());
    RootScope() { std::filesystem::create_directories(root); AssetPathResolver::SetGameRoot(root); }
    ~RootScope() {
        AssetPathResolver::SetGameRoot(previous);
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
    }
};

// ShowCursor exposes a thread-local counter, not just a visibility flag.
int CursorCount() { ShowCursor(TRUE); return ShowCursor(FALSE); }
void SetCursorCount(int target) {
    int current = CursorCount();
    while (current < target) current = ShowCursor(TRUE);
    while (current > target) current = ShowCursor(FALSE);
}

struct DesktopScope {
    int cursorCount = CursorCount();
    HCURSOR cursor = GetCursor();
    HWND foreground = GetForegroundWindow();
    HWND focus = GetFocus();
    HWND capture = GetCapture();
    POINT position{};
    bool hasPosition = GetCursorPos(&position) != FALSE;
    ~DesktopScope() {
        if (IsWindow(foreground)) SetForegroundWindow(foreground);
        if (IsWindow(focus)) SetFocus(focus);
        if (IsWindow(capture)) SetCapture(capture);
        if (hasPosition) SetCursorPos(position.x, position.y);
        SetCursor(cursor);
        SetCursorCount(cursorCount);
    }
};

struct WindowScope {
    HWND handle = nullptr;
    ~WindowScope() {
        if (IsWindow(handle)) DestroyWindow(handle);
        // The engine window procedure posts WM_QUIT on destruction. It must not
        // terminate the next test's host message pump.
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    }
};

HWND CreateHostWindow() {
    WNDCLASSEXW windowClass{.cbSize = sizeof(WNDCLASSEXW)};
    // makeWindow registers the engine message-routing procedure only once.
    if (!GetClassInfoExW(GetModuleHandleW(nullptr), L"ExampleGame", &windowClass))
        return makeWindow(320, 240, nullptr);
    const HWND window = CreateWindowExW(0, L"ExampleGame", L"Native input test", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window) ShowWindow(window, SW_SHOW);
    return window;
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::string ForegroundPrerequisite(HWND window) {
    const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return "OpenInputDesktop denied interactive desktop access (Win32 error " +
        std::to_string(GetLastError()) + ")";
    wchar_t inputName[256]{}, threadName[256]{};
    DWORD needed = 0;
    const bool named = GetUserObjectInformationW(desktop, UOI_NAME, inputName, sizeof(inputName), &needed) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            threadName, sizeof(threadName), &needed);
    const DWORD error = named ? ERROR_SUCCESS : GetLastError();
    CloseDesktop(desktop);
    if (!named) return "Cannot identify interactive/thread desktop (Win32 error " + std::to_string(error) + ")";
    if (std::wstring_view(inputName) != threadName) return "Test thread is not attached to the active input desktop";
    ShowWindow(window, SW_SHOW);
    const BOOL granted = SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    PumpMessages();
    if (GetForegroundWindow() != window || GetFocus() != window)
        return "OS did not grant foreground and keyboard focus: SetForegroundWindow=" +
            std::to_string(granted) + ", foregroundMatches=" +
            std::to_string(GetForegroundWindow() == window) + ", focusMatches=" +
            std::to_string(GetFocus() == window);
    return {};
}

TEST(Win32Input, SentMessagesUnicodeFocusAndRecaptureIngestion) {
    RootScope root;
    DesktopScope desktop;
    WindowScope window{CreateHostWindow()};
    ASSERT_NE(window.handle, nullptr);
    Engine engine{EngineConfig{.taskThreads = 1}};
    engine.AddSubsystem<Win32InputHost>(window.handle);
    auto* input = engine.AddSubsystem<InputSubsystem>();
    ASSERT_TRUE(engine.Initialize());
    // Sent messages require no physical foreground, pointer movement or host Update.
    SendMessageW(window.handle, WM_KILLFOCUS, 0, 0);
    EXPECT_EQ(SendMessageW(window.handle, WM_MOUSEACTIVATE, 0,
        MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)), MA_ACTIVATEANDEAT);
    SendMessageW(window.handle, WM_SETFOCUS, 0, 0);
    SendMessageW(window.handle, WM_LBUTTONUP, 0, MAKELPARAM(40, 40));
    SendMessageW(window.handle, WM_CHAR, 0x416, 1);
    input->Update({});
    EXPECT_FALSE(input->WasReleased(MouseButton::Left));
    bool foundText = false, foundFocus = false;
    for (const auto& event : input->GetUiEvents()) {
        EXPECT_FALSE(std::holds_alternative<InputUiPointerButton>(event.payload));
        if (const auto* text = std::get_if<InputUiText>(&event.payload)) foundText = text->utf8 == "\xd0\x96";
        if (const auto* focus = std::get_if<InputUiFocus>(&event.payload)) foundFocus |= focus->focused;
    }
    EXPECT_TRUE(foundText);
    EXPECT_TRUE(foundFocus);
    SendMessageW(window.handle, WM_KILLFOCUS, 0, 0);
    input->Update({});
    ASSERT_EQ(input->GetUiEvents().size(), 1u);
    const auto* focus = std::get_if<InputUiFocus>(&input->GetUiEvents().front().payload);
    ASSERT_NE(focus, nullptr);
    EXPECT_FALSE(focus->focused);
}

TEST(Win32Input, InteractiveCursorArbitrationAndFocusRestore) {
    RootScope root;
    DesktopScope desktop;
    WindowScope window{CreateHostWindow()};
    ASSERT_NE(window.handle, nullptr);
    WindowScope other{CreateWindowExW(0, L"STATIC", L"Other foreground window", WS_OVERLAPPEDWINDOW,
        400, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    ASSERT_NE(other.handle, nullptr);
    // Check both foreground transfers before any behavior assertions. A locked
    // desktop/background runner can still execute the sent-message test above.
    auto reason = ForegroundPrerequisite(other.handle);
    if (!reason.empty()) GTEST_SKIP() << "Other-window prerequisite: " << reason;
    reason = ForegroundPrerequisite(window.handle);
    if (!reason.empty()) GTEST_SKIP() << "Host-window prerequisite: " << reason;
    SetCursorCount(0);
    Engine engine{EngineConfig{.taskThreads = 1}};
    auto* native = engine.AddSubsystem<Win32InputHost>(window.handle);
    auto* input = engine.AddSubsystem<InputSubsystem>();
    ASSERT_TRUE(engine.Initialize());
    auto bridge = native->GetUiHost();
    input->SetCursorMode(CursorMode::Locked);
    native->Update({});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Locked);
    EXPECT_EQ(GetCapture(), window.handle);
    EXPECT_EQ(GetCursor(), nullptr);
    native->Update({});
    EXPECT_EQ(ShowCursor(TRUE), 0);
    EXPECT_EQ(ShowCursor(FALSE), -1);
    ReleaseCapture();
    ASSERT_EQ(GetCapture(), nullptr);
    native->Update({});
    EXPECT_EQ(GetCapture(), window.handle);
    bridge->SetCursor("text");
    bridge->SetCursor("text");
    EXPECT_EQ(GetCursor(), nullptr);
    bridge->SetCapture({.pointer = true});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Normal);
    EXPECT_EQ(GetCapture(), window.handle);
    EXPECT_EQ(GetCursor(), LoadCursorW(nullptr, IDC_IBEAM));
    bridge->SetCapture({.wantsVisibleCursor = true});
    EXPECT_EQ(GetCapture(), nullptr);
    bridge->SetCapture({});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Locked);
    input->SetCursorMode(CursorMode::Hidden);
    native->Update({});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Hidden);
    EXPECT_EQ(GetCapture(), nullptr);
    bridge->SetTextInput({.active = true});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Normal);
    bridge->SetTextInput({});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Hidden);
    SendMessageW(window.handle, WM_KILLFOCUS, 0, 0);
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Normal);
    EXPECT_EQ(input->GetCursorMode(), CursorMode::Hidden);
    input->Update({});
    native->Update({});
    POINT position{};
    ASSERT_TRUE(GetCursorPos(&position));
    ASSERT_TRUE(SetCursorPos(position.x + 12, position.y + 8));
    native->Update({});
    input->Update({});
    EXPECT_FLOAT_EQ(input->GetMouseDelta().x, 0.0f);
    EXPECT_FLOAT_EQ(input->GetMouseDelta().y, 0.0f);
    SendMessageW(window.handle, WM_SETFOCUS, 0, 0);
    native->Update({});
    EXPECT_EQ(native->GetEffectiveCursorMode(), CursorMode::Hidden);
    // Foreground access was established above; subsequent failures must fail.
    ASSERT_TRUE(SetForegroundWindow(other.handle));
    SetFocus(other.handle);
    ASSERT_EQ(GetForegroundWindow(), other.handle);
    ASSERT_EQ(GetFocus(), other.handle);
    SendMessageW(window.handle, WM_SETFOCUS, 0, 0);
    const auto otherCursor = LoadCursorW(nullptr, IDC_CROSS);
    SetCursor(otherCursor);
    bridge->SetCursor("text");
    bridge->SetCursor("pointer");
    EXPECT_EQ(GetCursor(), otherCursor);
    engine.Shutdown();
    EXPECT_EQ(GetCapture(), nullptr);
    EXPECT_EQ(CursorCount(), 0);
}
} // namespace
