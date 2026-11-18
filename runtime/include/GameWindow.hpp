
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace engine {
    struct NativeSurface;
}

class GameWindow {
public:

    struct Options {
        std::wstring title;
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        bool fullscreen = false;
    };

    [[nodiscard]] static std::unique_ptr<GameWindow> Create(const Options& options, std::string& error);

    ~GameWindow();

    GameWindow(const GameWindow&) = delete;
    GameWindow& operator=(const GameWindow&) = delete;

    GameWindow(GameWindow&&) = delete;
    GameWindow& operator=(GameWindow&&) = delete;

    [[nodiscard]] HWND Handle() const noexcept { return hwnd_; }
    [[nodiscard]] engine::NativeSurface Surface() const;
    [[nodiscard]] bool IsMinimized() const noexcept { return minimized_; }
    [[nodiscard]] bool IsFullscreen() const noexcept { return fullscreen_; }

    // Borderless over the window's monitor; leaving it restores the previous placement. Alt+Enter toggles it.
    void SetFullscreen(bool fullscreen);

    void SetResizeCallback(std::function<void(std::uint32_t, std::uint32_t)> cb) {
        onResize_ = std::move(cb);
    }

private:

    GameWindow() = default;

    static LRESULT CALLBACK Proc(
        HWND hwnd,
        UINT msg,
        WPARAM wp,
        LPARAM lp
    );

    void NotifyResize();
    void FitToMonitor();

private:

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;

    std::wstring className_;

    bool classRegistered_ = false;
    bool minimized_ = false;
    bool resizing_ = false;
    bool fullscreen_ = false;

    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};

    std::function<void(std::uint32_t, std::uint32_t)> onResize_;

};
