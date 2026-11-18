#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <RuntimeEntry.hpp>

#include <string>

// Static CRT and no engine code: nothing here may need a DLL before bin\ is on the search path.

namespace {

    std::wstring ExecutableDirectory() {
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0) return {};
            if (length < path.size()) {
                path.resize(length);
                break;
            }
            path.resize(path.size() * 2);
        }
        return path.substr(0, path.find_last_of(L"\\/"));
    }

    bool IsFile(const std::wstring& path) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring SystemMessage(DWORD code) {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::wstring message = length ? std::wstring(buffer, length) : L"Error " + std::to_wstring(code);
        LocalFree(buffer);
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ')) {
            message.pop_back();
        }
        return message;
    }

    int Fail(const std::wstring& message) {
        MessageBoxW(nullptr, message.c_str(), L"Game Runtime", MB_OK | MB_ICONERROR);
        return 1;
    }

} // namespace

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int
) {
    const std::wstring root = ExecutableDirectory();

    if (root.empty()) {
        return Fail(L"Could not locate the game executable: " + SystemMessage(GetLastError()));
    }

    // Built games keep their DLLs in bin\; the build tree keeps them beside the executable.
    const std::wstring libraries = root + L"\\" + runtime_entry::LibraryDirectory;
    std::wstring library = libraries + L"\\" + runtime_entry::GameLibrary;

    if (IsFile(library)) {
        // Windows has no rpath: from here on the loader also searches bin\, for the game DLL's imports too.
        if (!SetDllDirectoryW(libraries.c_str())) {
            return Fail(L"Could not add " + libraries + L" to the DLL search path: " + SystemMessage(GetLastError()));
        }
    }
    else {
        library = root + L"\\" + runtime_entry::GameLibrary;
    }

    const HMODULE module = LoadLibraryW(library.c_str());

    if (!module) {
        return Fail(L"Could not load " + library + L":\n" + SystemMessage(GetLastError()));
    }

    const auto run = reinterpret_cast<runtime_entry::RunGameFunction>(
        GetProcAddress(module, runtime_entry::RunGameExport));

    if (!run) {
        return Fail(library + L" is not a game runtime library.");
    }

    return run();
}
