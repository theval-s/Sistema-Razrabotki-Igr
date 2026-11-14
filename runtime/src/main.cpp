
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <GameWindow.hpp>
#include <engine/engine.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/DefaultSubsystems.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/input/Win32InputHost.hpp>
#include <engine/project/Project.hpp>
#include <engine/project/RuntimeSettings.hpp>

#include <filesystem>
#include <string>
#include <exception>
#include <memory>
#include <cstdint>

#include <vector>
#include <string_view>
#include <system_error>
#include <utility>


namespace {

    namespace fs = std::filesystem;

    bool Utf8ToWide(std::string_view text, std::wstring& result) {
        result.clear();

        if (text.empty()) {
            return true;
        }

        const int required = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0
        );

        if (required <= 0) {
            return false;
        }

        result.resize(static_cast<std::size_t>(required));

        const int converted = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            required
        );

        if (converted != required) {
            result.clear();
            return false;
        }

        return true;
    }

    int Fail(const std::string& message) noexcept {
        try {
            engine::log::Engine().error("Runtime startup failed: {}", message);
        }
        catch (...) { }

        std::wstring wideMessage;

        if (!Utf8ToWide(message, wideMessage)) {
            wideMessage = L"An error occurred while starting the game. " L"See the runtime log for details.";
        }

        MessageBoxW(
            nullptr,
            wideMessage.c_str(),
            L"Game Runtime",
            MB_OK | MB_ICONERROR
        );

        return 1;
    }

    bool UserDataDirectory(
        const std::string& projectName,
        fs::path& result,
        std::string& error
    ) {
        const fs::path name = engine::FromUtf8(projectName);

        if (name.empty() ||
            name == L"." ||
            name == L".." ||
            name.has_parent_path() ||
            name.filename() != name) {
            error = "Invalid project name for the user data directory";
            return false;
        }

        const DWORD required = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            nullptr,
            0
        );

        if (required == 0) {
            error = "Could not determine the LOCALAPPDATA directory";
            return false;
        }

        std::vector<wchar_t> buffer(required);

        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            buffer.data(),
            required
        );

        if (length == 0 || length >= required) {
            error = "Could not read the LOCALAPPDATA directory";
            return false;
        }

        const fs::path localAppData(std::wstring(buffer.data(), length));

        result = localAppData / L"SRI" / name;

        std::error_code ec;
        fs::create_directories(result, ec);

        if (ec) {
            error = "Could not create the user data directory: " + engine::ToUtf8(result) + ": " + ec.message();
            return false;
        }

        return true;
    }

} // namespace

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int
) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        const fs::path exeDir = engine::AssetPathResolver::GetRoot();

        std::string error;

        auto project = engine::Project::Load(exeDir / L"game.sriproject", error);

        if (!project) {
            return Fail(error);
        }

        engine::RuntimeSettings settings;

        const fs::path settingsPath = exeDir / engine::RuntimeSettings::FileName;

        std::error_code fileError;
        const bool settingsExist = fs::exists(settingsPath, fileError);

        if (fileError) {
            return Fail("Could not check runtime settings file: " + engine::ToUtf8(settingsPath) + ": " + fileError.message());
        }

        if (settingsExist) {
            std::string settingsText;

            if (!engine::ReadTextFile(
                settingsPath,
                settingsText,
                error
            )) {
                return Fail(error);
            }

            if (!engine::ParseRuntimeSettings(
                settingsText,
                settings,
                error
            )) {
                return Fail(error);
            }
        }

        fs::path userDir;

        if (!UserDataDirectory(project->name, userDir, error)) {
            return Fail(error);
        }

        const fs::path logPath = userDir / L"logs" / L"game.log";

        if (!engine::log::AddFileSink(logPath)) {
            engine::log::Engine().warn("Cannot open log file {}, logging to console only", engine::ToUtf8(logPath));
        }

        engine::log::Engine().info("Starting game '{}'",project->name);

        std::wstring windowTitle;

        const std::string& title = settings.title.empty() ? project->name : settings.title;

        if (!Utf8ToWide(title, windowTitle)) {
            return Fail("Could not convert the game window title to UTF-16");
        }

        GameWindow::Options windowOptions;
        windowOptions.title = std::move(windowTitle);
        windowOptions.width = settings.width;
        windowOptions.height = settings.height;
        windowOptions.fullscreen = settings.fullscreen;

        auto window = GameWindow::Create(windowOptions, error);

        if (!window) {
            return Fail(error);
        }

        engine::EngineConfig config;
        config.applicationName = project->name;
        config.surface = window->Surface();

        engine::Engine runtime(config);

        runtime.SetSaveDirectory(
            userDir / L"saves"
        );

        auto* host = runtime.AddSubsystem<engine::Win32InputHost>(window->Handle());

        if (!host) {
            return Fail("Could not register Win32InputHost");
        }

        auto* input = engine::RegisterDefaultSubsystems(runtime, host->GetUiHost());

        if (!input) {
            return Fail("Could not register the default input subsystem");
        }

        window->SetResizeCallback([&runtime](std::uint32_t width, std::uint32_t height) { runtime.ResizeSurface(width, height); });

        if (!runtime.Initialize()) {
            return Fail("Engine initialization failed. See the runtime log: " + engine::ToUtf8(logPath));
        }

        engine::log::Engine().info("Engine initialized successfully");

        if (project->startupScene.empty()) {
            return Fail("The project does not specify a startup scene");
        }

        const fs::path startupScene = engine::FromUtf8(project->startupScene);

        if (!runtime.LoadScene(startupScene, error)) {
            return Fail(error);
        }

        if (!error.empty()) {
            engine::log::Engine().warn("Startup scene loaded with diagnostics: {}", error);
        }

        // Initial mode only: scripts may change it, Win32InputHost reapplies it on focus.
        if (settings.captureMouse) {
            input->SetCursorMode(engine::CursorMode::Locked);
        }

        if (!runtime.StartGame(
            engine::AssetPathResolver::GetGameRoot(),
            project->autoloads,
            error
        )) {
            return Fail(error);
        }

        engine::log::Engine().info("Game session started");

        while (!runtime.IsExitRequested()) {

            if (window->IsMinimized()) {
                Sleep(50);
            }
            runtime.Tick();
        }

        runtime.StopGame();
        runtime.Shutdown();

        engine::log::Engine().info(
            "Game runtime stopped successfully"
        );

        return 0;
    }
    catch (const std::exception& e) {
        return Fail(e.what());
    }
    catch (...) {
        return Fail("Unknown exception occurred in the game runtime");
    }
}